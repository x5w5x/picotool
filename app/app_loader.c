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

/* wire_quality plugin 需要主动驱动引脚(TX 推 HIGH/LOW)做杜邦线通断 + 丢包测试。
 * API 表里这两个槽追加在末尾,旧 plugin (led_controller) 不会读到,所以不破坏 ABI。 */
static void host_gpio_set_dir(uint32_t pin, bool is_output) {
    gpio_init((unsigned int)pin);
    gpio_set_dir((unsigned int)pin, is_output);
}

static void host_gpio_put(uint32_t pin, bool value) {
    gpio_put((unsigned int)pin, value);
}

/* wire_quality plugin MODE_CONT 需要 RX 引脚上拉,
 * 让 OPEN (线断了) 时 RX 稳定读 HIGH,与 PASS (跟随 TX) 区分开。
 * 没上拉的话浮空 RX 随机跳动,OPEN 容易误判成 SHORT-VCC。
 * SDK 的 gpio_set_pulls 形参 up/down 分别是使能上拉/下拉。 */
static void host_gpio_set_pulls(uint32_t pin, bool up, bool down) {
    gpio_set_pulls((unsigned int)pin, up, down);
}

static uint16_t host_unicode_to_oem(uint32_t codepoint) {
    if (codepoint > 0x10FFFFu) return 0;
    return (uint16_t)ff_uni2oem((DWORD)codepoint, 936);
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

/* ================= FATFS 暴露给 plugin =================
 * plugin 是 freestanding 不链 fatfs.a;这里维护 4-slot fd table,
 * plugin 通过 fat_open 拿 fd,再 read/seek/size/close。
 * 同时给 list_dir,只返 .txt 文件名 (0:/ 根目录扫一遍) */
#define MAX_FAT_FILES  4
static FIL      s_fat_fds[MAX_FAT_FILES];
static bool     s_fat_used[MAX_FAT_FILES];

/* ⚡ plugin 用的 FATFS 工作区
 * app_sd_msc 在退出时会 f_mount(NULL) 把 FatFs[0] 清空,
 * 但 FATFS R0.15 不自动 mount → plugin 立刻 FR_NOT_ENABLED。
 * 这里 host 自带一份 FATFS,plugin 调 fat_open 时如果发现
 * FatFs[0] == NULL 就用这份工作区主动挂载一次。
 *
 * ⚠️ 重大陷阱 (已踩):R0.15 每次 f_mount 都会 ++Fsid。
 *   validate() 内部检查 obj->id == obj->fs->id,所以后挂的 mount
 *   会让前面打开的 FIL 全部失效 (FR_INVALID_OBJECT)。
 *   修:整个 plugin 生命周期内只挂一次。
 *   plugin 退出时清 s_plugin_fat_mounted,下一个 plugin 进来再挂。
 *   ❗ 千万别在 fat_open 每次都挂 ——
 *      open_file 内 fat_open(主文件) + load_bookmark fat_open(.bmk) 会
 *      第二次挂载,把主文件 FIL 的 id 作废 → 后续 fat_read 全 -1 (r=-1) */
static FATFS    s_plugin_fatfs;
static bool     s_plugin_fat_mounted = false;

static int host_fat_open(const char *path, uint8_t mode) {
    if (!path) return -1;

    /* ⚡ mount 状态管理 (见 s_plugin_fat_mounted 注释):
     *   1) 第一次进入 plugin → 挂
     *   2) plugin 已在跑但 s_plugin_fatfs.fs_type == 0 → 重挂
     *      (典型场景:plugin init 里 fat_list_dir 挂了一次,然后
     *       app_manager_switch_to 触发 app_sd_msc.exit,后者又
     *       f_mount(NULL) 把 FatFs[0] 和 s_plugin_fatfs.fs_type 清掉。
     *       此时 s_plugin_fat_mounted 还是 true,但底层 mount 已经丢了)
     *   3) 正常 plugin 跑的过程里连续 fat_open (open_file + load_bookmark)
     *      不重挂 — 否则 Fsid++ 让前面开的 FIL 全部 FR_INVALID_OBJECT
     *
     * ❗ 重挂时一定要保证没有 plugin 的 FIL 还开着,否则 FIL.id 会失效。
     *    场景 2 重挂时 plugin 还没打开任何文件,所以安全。
     *    场景 3 不重挂,直接复用现有 mount,FATFS id 保持稳定。 */
    if (!s_plugin_fat_mounted || s_plugin_fatfs.fs_type == 0) {
        FRESULT mr = f_mount(&s_plugin_fatfs, "0:", 1);
        if (mr != FR_OK) {
            printf("[PAPP] fat_open mount FAIL err=%d\r\n", (int)mr);
            return -1;
        }
        s_plugin_fat_mounted = true;
    }

    /* mode: 0=read, 1=write(create/truncate) — 简单两档 */
    BYTE fa_mode = (mode == 1) ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ;
    for (int i = 0; i < MAX_FAT_FILES; i++) {
        if (!s_fat_used[i]) {
            FRESULT r = f_open(&s_fat_fds[i], path, fa_mode);
            if (r != FR_OK) {
                printf("[PAPP] fat_open FAIL path=\"%s\" err=%d\r\n", path, (int)r);
                return -1;
            }
            s_fat_used[i] = true;
            return i;
        }
    }
    printf("[PAPP] fat_open FAIL no free fd\r\n");
    return -1;
}

static int host_fat_read(int fd, uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_FAT_FILES || !s_fat_used[fd] || !buf) return -1;
    UINT br = 0;
    FRESULT r = f_read(&s_fat_fds[fd], buf, len, &br);
    if (r != FR_OK) return -1;
    return (int)br;
}

static int host_fat_write(int fd, const uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_FAT_FILES || !s_fat_used[fd] || !buf) return -1;
    UINT bw = 0;
    FRESULT r = f_write(&s_fat_fds[fd], buf, len, &bw);
    if (r != FR_OK) return -1;
    /* ⚡ 必须 f_sync 一下,否则断电书签丢失 */
    f_sync(&s_fat_fds[fd]);
    return (int)bw;
}

static int host_fat_seek(int fd, uint32_t pos) {
    if (fd < 0 || fd >= MAX_FAT_FILES || !s_fat_used[fd]) return -1;
    FRESULT r = f_lseek(&s_fat_fds[fd], pos);
    return (r == FR_OK) ? 0 : -1;
}

static int host_fat_size(int fd) {
    if (fd < 0 || fd >= MAX_FAT_FILES || !s_fat_used[fd]) return -1;
    FSIZE_t sz = f_size(&s_fat_fds[fd]);
    return (int)(sz & 0x7FFFFFFFu);
}

static void host_fat_close(int fd) {
    if (fd < 0 || fd >= MAX_FAT_FILES || !s_fat_used[fd]) return;
    f_close(&s_fat_fds[fd]);
    s_fat_used[fd] = false;
}

/* ⚡ 列目录里所有 .txt 文件名
 * ⚠️ 写入格式:plugin 端把 out_buf 当成 `char file_names[MAX_FILES][NAME_LEN]`
 *   固定 stride 的二维数组读 (每格 NAME_LEN 字节)。host 必须按相同 stride 写,
 *   否则 file_names[i] 落在两个相邻字符串的中间空白区 → 读到空串。
 *   修:每写一个名字,先 memset 整格 0,再 memcpy 名字到格首 + '\0' 终止
 *
 * 返写出的文件名数,负数 = 失败
 * 不递归子目录,只看 dir 这一层 */
static int host_fat_list_dir(const char *dir, char *out_buf, int out_buf_size, int max_files) {
    if (!dir || !out_buf || out_buf_size < 2 || max_files < 1) return -1;
    /* ⚡ 与 host_fat_open 共享同款 mount 策略 (见 s_plugin_fat_mounted 注释):
     * 整个 plugin 会话最多挂一次;挂载丢失 (fs_type==0) 时再补挂一次。
     * plugin 跑的过程里连续调用不重挂,否则 ++Fsid 会把 FIL 全部作废 */
    if (!s_plugin_fat_mounted || s_plugin_fatfs.fs_type == 0) {
        FRESULT mr = f_mount(&s_plugin_fatfs, "0:", 1);
        if (mr != FR_OK) {
            printf("[PAPP] fat_list_dir mount FAIL err=%d\r\n", (int)mr);
            return -1;
        }
        s_plugin_fat_mounted = true;
    }
    /* ⚡ stride = out_buf_size / max_files 必须和 plugin 的 NAME_LEN 一致
     *   (out_buf_size == sizeof(s_nr.file_names) == MAX_FILES * NAME_LEN) */
    int stride = out_buf_size / max_files;
    if (stride < 2) return -1;
    DIR dj;
    FILINFO fno;
    FRESULT r = f_opendir(&dj, dir);
    if (r != FR_OK) return -1;
    int count = 0;
    while (count < max_files) {
        r = f_readdir(&dj, &fno);
        if (r != FR_OK || fno.fname[0] == 0) break;  /* 0 = 目录读完 */
        if (fno.fattrib & AM_DIR) continue;          /* 跳子目录 */
        /* 后缀 == ".txt" 才留 (大小写不敏感)
         * ⚡ 没有 LFN 条目的旧文件,ff.c 的 get_fileinfo 会把短名 TESE.TXT
         *   复制到 fno.fname。如果用户是用某工具直接写到 8.3 的目录项上、
         *   没产生 LFN 的话,fno.fname 就是全大写 → strcmp(".txt") 漏掉。
         *   改成大小写不敏感统一放过。 */
        const char *dot = strrchr(fno.fname, '.');
        if (!dot) continue;
        if (dot[1] == 0) continue;                   /* 以 . 结尾不是扩展名 */
        {
            const char *s = dot + 1;
            const char *pat = "txt";
            while (*s && *pat) {
                char a = *s, b = *pat;
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
                s++; pat++;
            }
            if (*pat != 0 || *s != 0) continue;      /* 长度也要等于 3 */
        }
        int len = (int)strlen(fno.fname);
        if (len >= stride) len = stride - 1;         /* 截断以免越界 */
        /* ⚡ 固定 stride 写:第 i 个名字写到 out_buf + i*stride,
         *   先清零整格,避免上一次 plugin session 的残留字符出现在中间 */
        char *slot = out_buf + count * stride;
        for (int k = 0; k < stride; k++) slot[k] = 0;
        for (int k = 0; k < len; k++) slot[k] = fno.fname[k];
        slot[len] = '\0';
        count++;
    }
    f_closedir(&dj);
    return count;
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

    /* ===== 末尾扩展槽 (wire_quality plugin 需要) ===== */
    .gpio_set_dir             = host_gpio_set_dir,
    .gpio_put                 = host_gpio_put,
    .gpio_set_pulls           = host_gpio_set_pulls,

    /* ===== FATFS 暴露 (novel_reader 等需要打开文件的 plugin) ===== */
    .fat_open                 = host_fat_open,
    .fat_read                 = host_fat_read,
    .fat_write                = host_fat_write,
    .fat_seek                 = host_fat_seek,
    .fat_size                 = host_fat_size,
    .fat_close                = host_fat_close,
    .fat_list_dir             = host_fat_list_dir,
    .unicode_to_oem           = host_unicode_to_oem,
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

    /* ⚡ plugin 退出时强制关闭它还开着的文件,免得 fd table 漏 */
    for (int i = 0; i < MAX_FAT_FILES; i++) {
        if (s_fat_used[i]) {
            f_close(&s_fat_fds[i]);
            s_fat_used[i] = false;
        }
    }

    /* ⚡ 主动卸载 s_plugin_fatfs,避免它还占用 FatFs[0]。
     * 如果不卸载,app_sd_msc.exit 后又再次进来再 mount 时,FATFS 内部逻辑会
     * 把 s_plugin_fatfs.fs_type 清零,但 s_plugin_fat_mounted 还留着 true,
     * 下次 plugin 进来时会以为已经挂过 → 不重新 mount → fs_type 还是 0 →
     * f_open 时 validate 失败 (obj->fs->fs_type == 0) → FR_INVALID_OBJECT。
     * 干脆卸载并清标志,下一个 plugin 进来时从头挂一份。 */
    if (s_plugin_fat_mounted) {
        f_mount(NULL, "0:", 1);
        s_plugin_fat_mounted = false;
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
uint8_t *app_loader_get_ram_pool(void) {
    return s_app_ram_pool;
}

uint32_t app_loader_get_ram_pool_size(void) {
    return APP_RAM_SIZE; // 48KB
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
