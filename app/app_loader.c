#include "app_loader.h"
#include "app_manager.h"
#include "lcd.h"
#include "ff.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_tile.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ⚡ 引用主系统全局管理器实例
extern app_manager_t g_app_mgr;

// ⚡ 锚定在 0x20034000 的 48KB 运行池
static uint8_t s_app_ram_pool[APP_RAM_SIZE] __attribute__((aligned(32), section(".app_pool_sec")));

static const app_plugin_t *s_active_plugin = NULL;
static volatile bool       s_plugin_running = false;
static app_t               s_app_loader_inst;
/* loader 进程专属的脏矩形存储 — app_manager 在 process_touch 返回 true 后
 * 会用 *(ui_rect_t *)cur->user_data 写脏矩形,所以 user_data 必须指向有效内存 */
static ui_rect_t           s_loader_dirty_rect = {0, 0, 0, 0};
/* ⚡ 跨 app 触摸残留防护:switch_to(loader_app) 时用户手指通常还按在屏幕上,
 * 之后 main loop 会把同一触摸序列的 down → up 都转给 loader,plugin 误认为
 * 一次完整点击 → STOP 按钮被误触发 → plugin 自动退出。
 * 修复:loader_app_enter 时置 true,丢弃接下来直到看到一次 up 的全部事件
 * (这一段是上一个 app 的残留触摸)。看到 up 后清零,开始正常接收新触摸。 */
static bool                s_skip_carried_touch = false;
/* plugin 在 update_core1 里发现状态变化(GPIO 电平翻转等)时,
 * 调 host_request_redraw 置 true。loader_app_update 在 Core 0 看到后
 * 把 mgr->needs_full_redraw 置 true,触发下一帧全屏重画。
 * volatile 因为跨核读写,Core 1 写、Core 0 读 */
static volatile bool       s_redraw_pending = false;

/* ================= 导出的具体系统 API ================= */
static uint32_t host_get_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint64_t host_get_time_us(void) {
    return to_us_since_boot(get_absolute_time());
}

static void host_delay_ms(uint32_t ms) {
    sleep_ms(ms);
}

static void host_delay_us(uint64_t us) {
    sleep_us(us);
}

/* GPIO 读 — SDK 的 gpio_get 形参是 uint (= unsigned int),
 * 而 host_api 里 uint32_t 在 C 类型系统里与之不兼容。
 * 加一层小 wrapper 把签名统一到 uint32_t */
static bool host_gpio_get(uint32_t pin) {
    return gpio_get((unsigned int)pin);
}

static void host_request_exit(void) {
    printf("[PAPP] host_request_exit!\r\n");
    s_plugin_running = false;
}

/* plugin 调这个告诉 host "我的状态变了,请下一帧整屏重画"
 * 典型用法:Core 1 后台 polling 读到 GPIO 电平变化时 */
static void host_request_redraw(void) {
    s_redraw_pending = true;
}

static void host_post_log(const char *str) {
    printf("[PAPP] %s\r\n", str ? str : "");
}

static const pico_host_api_t s_host_api_table = {
    .version         = 0x00010000,
    .get_time_ms     = host_get_time_ms,
    .get_time_us     = host_get_time_us,
    .delay_ms        = host_delay_ms,
    .delay_us        = host_delay_us,
    .lcd_set_window  = LCD_SetWindow,
    .lcd_bulk_write  = LCD_BulkWrite,
    .lcd_fill_rect   = LCD_FillRect,
    .gpio_get        = host_gpio_get,
    .post_log        = host_post_log,
    .request_exit    = host_request_exit,
    .request_redraw  = host_request_redraw,

    /* ===== UI 渲染槽位 ===== */
    /* 这些函数都在 libuigeom.a 里,host 主镜像链接时已经拉进来了,
     * 把它们的函数指针塞进表,plugin 通过 s_api->ui_* 就能调到。 */
    .ui_draw_fill_rect        = ui_draw_fill_rect,
    .ui_draw_fast_hline       = ui_draw_fast_hline,
    .ui_draw_string           = ui_draw_string,
    .ui_btn_init              = ui_btn_init,
    .ui_btn_set_color         = ui_btn_set_color,
    .ui_btn_process_touch     = ui_btn_process_touch,
    .ui_btn_draw              = ui_btn_draw,
};

/* ================= Core 1 工作协程 ================= */
static void core1_plugin_worker(void) {
    while (s_plugin_running) {
        if (s_active_plugin && s_active_plugin->update_core1) {
            s_active_plugin->update_core1();
        }
        tight_loop_contents();
    }
}

/* ================= 加载与卸载实现 ================= */
bool app_loader_load_papp(const char *filepath) {
    printf("[PAPP] load_papp(\"%s\")\r\n", filepath ? filepath : "(null)");
    app_loader_unload_current();

    FIL fil;
    FRESULT ro = f_open(&fil, filepath, FA_READ);
    if (ro != FR_OK) {
        printf("[PAPP] f_open FAIL err=%d\r\n", (int)ro);
        return false;
    }
    printf("[PAPP] f_open OK\r\n");

    UINT br = 0;
    FRESULT res = f_read(&fil, s_app_ram_pool, APP_RAM_SIZE, &br);
    f_close(&fil);
    if (res != FR_OK) {
        printf("[PAPP] f_read FAIL err=%d br=%u\r\n", (int)res, (unsigned)br);
        return false;
    }
    if (br < 32) {
        printf("[PAPP] f_read too small br=%u (need >=32)\r\n", (unsigned)br);
        return false;
    }
    printf("[PAPP] f_read OK br=%u bytes\r\n", (unsigned)br);

    // Cortex-M0+ Thumb 入口函数指针 LSB 置 1
    app_entry_t entry = (app_entry_t)((uintptr_t)s_app_ram_pool | 1);
    printf("[PAPP] calling entry @ 0x%08x\r\n", (unsigned)((uintptr_t)s_app_ram_pool | 1u));
    s_active_plugin = entry(&s_host_api_table);
    printf("[PAPP] entry returned %p\r\n", (void*)s_active_plugin);

    if (!s_active_plugin) {
        printf("[PAPP] entry returned NULL\r\n");
        return false;
    }
    if (s_active_plugin->magic != APP_MAGIC) {
        printf("[PAPP] bad magic 0x%08x (want 0x50415050)\r\n",
               (unsigned)s_active_plugin->magic);
        s_active_plugin = NULL;
        return false;
    }
    printf("[PAPP] magic OK, name=\"%s\"\r\n",
           s_active_plugin->name ? s_active_plugin->name : "?");

    if (s_active_plugin->init) {
        printf("[PAPP] calling init()\r\n");
        if (!s_active_plugin->init(&s_host_api_table)) {
            printf("[PAPP] init() returned false\r\n");
            s_active_plugin = NULL;
            return false;
        }
        printf("[PAPP] init() OK\r\n");
    }

    s_plugin_running = true;
    multicore_reset_core1();
    multicore_launch_core1(core1_plugin_worker);
    printf("[PAPP] Core 1 launched, load DONE\r\n");

    return true;
}

void app_loader_unload_current(void) {
    s_plugin_running = false;
    multicore_reset_core1();

    if (s_active_plugin && s_active_plugin->exit) {
        s_active_plugin->exit();
    }

    s_active_plugin = NULL;
    memset(s_app_ram_pool, 0, APP_RAM_SIZE);
}

bool app_loader_is_running(void) {
    return (s_active_plugin != NULL && s_plugin_running);
}

/* ================= 系统 App 回调对接 ================= */
static void loader_app_init(app_t *self) { (void)self; }
static void loader_app_enter(app_t *self) {
    (void)self;
    /* ⚡ 关键:刚进入 loader 时,用户的触摸事件可能还在延续(从 app_sd 切换
     * 过来时手指没抬起)。丢弃接下来直到一次 up 的整段触摸事件,
     * 避免 plugin 把"上一段触摸的尾巴"当成一次新点击处理。 */
    s_skip_carried_touch = true;
}

static void loader_app_exit(app_t *self) {
    (void)self;
    printf("[PAPP] loader_app_exit called\r\n");
    app_loader_unload_current();
}

static void loader_app_update(app_t *self, uint32_t now_tick) {
    (void)self; (void)now_tick;

    /* ⚡ plugin 在 update_core1 里如果检测到状态变化(GPIO 电平翻转等),
     * 会调 host_request_redraw 把 s_redraw_pending 置 true。
     * 这里在 Core 0 把它转成 needs_full_redraw,触发下一帧整屏重画 */
    if (s_redraw_pending) {
        s_redraw_pending = false;
        g_app_mgr.needs_full_redraw = true;
    }

    if (!s_plugin_running && s_active_plugin) {
        printf("[PAPP] loader_app_update -> return_home (running=0)\r\n");
        // App 请求退出时，直接切回系统主页
        app_manager_return_home(&g_app_mgr);
    }
}

static bool loader_app_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)self; (void)now_tick;
    if (!s_active_plugin || !s_active_plugin->process_touch) return false;

    if (s_skip_carried_touch) {
        /* 丢弃上一个 app 残留的触摸事件(可能 down + 多次 move + up)
         * 直到看到一次 up — 表示旧触摸结束,可以接受下一次新触摸了 */
        if (!is_touching) {
            s_skip_carried_touch = false;
        }
        /* 仍返回 true 触发全屏重画,这样 plugin UI 能正常显示出来 */
        s_loader_dirty_rect = (ui_rect_t){0, 0, 480, 320};
        return true;
    }

    /* ⚡ CPU 99% 修复:之前每次 process_touch 都返回 true + 全屏 dirty_rect,
     * 导致 dirty_box 始终非空 → frame_budget=16666us(60 FPS)→
     * 每帧都全屏 SPI DMA 重画 60ms+ → loop_busy_us 远超预算 → CPU 100%。
     *
     * 修复:只在 touch 状态变化(按下/松开)瞬间返回 true。
     *   - 按下瞬间:plugin 收到 true,ui_btn_process_touch 进入 pressed 状态
     *     → 全屏重画一次,显示出"按下"效果
     *   - 中间 move 事件:plugin 收到 true 继续追踪,返回 false → 不重画
     *   - 松开瞬间:plugin 收到 false,ui_btn_process_touch 触发回调 → 重画
     *   - idle (没触摸):plugin 收到 false 是 no-op,返回 false → 不重画
     *
     * 副作用:用户在按钮上按住手指滑动时,按钮高亮不会跟着手指走 ——
     * 这对单/双按钮 plugin 完全可以接受。
     *
     * 如果 plugin 状态变化(GPIO/计时器)需要主动刷屏,调 host_api 的
     * request_redraw() 即可,loader 会通过 loader_app_update 把
     * needs_full_redraw 置 true。 */
    static bool s_last_is_touching = false;
    bool state_changed = (is_touching != s_last_is_touching);
    s_last_is_touching = is_touching;

    s_active_plugin->process_touch(is_touching, tx, ty);

    if (state_changed) {
        s_loader_dirty_rect = (ui_rect_t){0, 0, 480, 320};
        return true;
    }
    return false;
}

static void loader_app_draw(app_t *self, ui_tile_t *tile) {
    (void)self;
    if (s_active_plugin && s_active_plugin->draw) {
        s_active_plugin->draw(tile);
    }
}

app_t *app_loader_get_instance(void) {
    s_app_loader_inst.id            = APP_ID_CUSTOM_0 + 8;
    s_app_loader_inst.name          = "动态扩展";
    s_app_loader_inst.user_data     = &s_loader_dirty_rect;
    s_app_loader_inst.init          = loader_app_init;
    s_app_loader_inst.enter         = loader_app_enter;
    s_app_loader_inst.exit          = loader_app_exit;
    s_app_loader_inst.update        = loader_app_update;
    s_app_loader_inst.process_touch = loader_app_process_touch;
    s_app_loader_inst.draw          = loader_app_draw;
    return &s_app_loader_inst;
}