/* =============================================================
 * 演示 plugin: LED 电平监视器
 *
 * 架构契约:
 *   - 整个二进制 ≤ 32KB,落 APP_RAM_BASE..APP_RAM_BASE+32K
 *     (当前 host 配置 = 0x20038000..0x20040000)
 *   - 入口 app_entry() 必须在二进制 offset 0 (见 plugin.ld .entry)
 *     本文件不实现 app_entry,只提供描述符单例;入口由 main.c 提供
 *   - 所有 UI 调用走 host_api 函数指针 (Core 0 上下文 = draw())
 *     update_core1 跑 Core 1,严禁碰 UI / LCD / GPIO
 *
 * 风格对齐 host app (app_led.c):
 *   - 状态结构体 + static 单例 + getter 函数 (led_controller_get_descriptor)
 *   - dirty_rect 字段放第 1 位,与 host app_manager 约定一致
 *   - 控件 user_data 指向自身状态结构体,回调用 ud 取数据
 *
 * 功能:
 *   - 通过 host_api->gpio_get(LED_PIN) 周期性读 PICO_DEFAULT_LED_PIN (GPIO 25)
 *     的实时电平,在屏幕中央以大块 LED 形式显示 on/off
 *   - STOP 按钮退出 plugin
 *
 * 源文件编码:
 *   源 .c 文件以 UTF-8 保存,编译选项 -finput-charset=UTF-8 -fexec-charset=GBK
 *   将中文常量字符串从 UTF-8 源字符转换为 GBK 字节塞进 .rodata。
 *   ui_draw_string 内部遇到 ≥0x80 字节会按 GB2312 双字节解释并查 W25Q64 字库。
 *
 * 注意:本 plugin 不使用 printf/snprintf,不调任何 libc / libgcc,
 *      所以无需内联 __aeabi_uidiv 之类。
 * ============================================================= */

#include "plugin_led_controller.h"
#include <stddef.h>

/* ================= 1. 屏幕分辨率与 plugin 私有状态 ================= */

#define SCREEN_W  480
#define SCREEN_H  320

/* PICO_DEFAULT_LED_PIN = 25 (RP2040 Pico 板载 LED)
 * 复用 app_led.c 的同一引脚,保持与 host app 引用一致 */
#define LED_PIN   25

typedef struct {
    ui_rect_t              dirty_rect;   // ⚠️ 必须保持在第 1 位,与 host app_manager 约定一致
    ui_btn_t               btn_stop;     // 退出按钮

    // init 阶段缓存的 host api,回调用 user_data 取
    const pico_host_api_t *api;

    // Core 1 周期采样 + Core 0 读出 (volatile 避免编译器优化掉)
    volatile bool          led_level;
} led_controller_data_t;

static led_controller_data_t s_lc;

/* ================= 2. 交互回调 ================= */

/* STOP 按钮点击:触发 plugin 主动退出流程。
 * host loader_app_update 检测到 s_plugin_running==false 后会:
 *   1) 调 plugin->exit()
 *   2) 卸载 RAM 池
 *   3) app_manager_return_home() 回主界面
 * 注意:这是 plugin 唯一受支持的退出方式 (host 没提供 gesture 等手势系统) */
static void on_stop_click(ui_btn_t *btn, void *ud) {
    (void)btn;
    led_controller_data_t *data = (led_controller_data_t *)ud;
    if (data && data->api && data->api->request_exit) {
        data->api->request_exit();
    }
}

/* ================= 3. 生命周期与事件回调 ================= */

static bool led_controller_init(const pico_host_api_t *api) {
    if (!api) return false;

    s_lc.dirty_rect = ui_rect_make(0, 0, 0, 0);
    s_lc.api        = api;
    s_lc.led_level  = false;

    // STOP 按钮:右下角
    if (s_lc.api->ui_btn_init) {
        s_lc.api->ui_btn_init(&s_lc.btn_stop,
                              (int16_t)(SCREEN_W - 160),
                              (int16_t)(SCREEN_H - 60),
                              140, 50,
                              "STOP", on_stop_click);
        if (s_lc.api->ui_btn_set_color) {
            s_lc.api->ui_btn_set_color(&s_lc.btn_stop, 0x9000, 0xF800, 0xFFFF);
        }
    }
    s_lc.btn_stop.user_data = &s_lc;

    if (s_lc.api->post_log) {
        s_lc.api->post_log("LED Monitor Plugin Initialized!");
    }
    return true;
}

/* Core 1 周期性读 GPIO 电平 → 写入共享 led_level。
 * 频率 ~20Hz (50ms),足够人眼观察,又不占总线。
 * 电平翻转时主动调 request_redraw,让 Core 0 主循环下一帧全屏重画 */
static void led_controller_update_core1(void) {
    if (!s_lc.api || !s_lc.api->gpio_get) return;
    bool cur = s_lc.api->gpio_get(LED_PIN) ? true : false;
    if (cur != s_lc.led_level) {
        s_lc.led_level = cur;
        if (s_lc.api->request_redraw) {
            s_lc.api->request_redraw();
        }
    }
    if (s_lc.api->delay_ms) {
        s_lc.api->delay_ms(50);
    }
}

static void led_controller_process_touch(bool is_touch, int16_t x, int16_t y) {
    // Core 0 上下文:host 在 loader_app_process_touch 里转发触控给 plugin
    if (!s_lc.api || !s_lc.api->ui_btn_process_touch) return;
    uint32_t now = s_lc.api->get_time_ms ? s_lc.api->get_time_ms() : 0u;
    s_lc.api->ui_btn_process_touch(&s_lc.btn_stop, is_touch, x, y, now);
}

static void led_controller_draw(ui_tile_t *tile) {
    if (!tile || !s_lc.api) return;

    /* ---------------- 全屏底色 (浅白) ---------------- */
    if (s_lc.api->ui_draw_fill_rect) {
        ui_rect_t full = ui_rect_make(0, 0, SCREEN_W, SCREEN_H);
        s_lc.api->ui_draw_fill_rect(tile, &full, 0xFFFF);
    }

    /* ---------------- 标题栏 ---------------- */
    if (s_lc.api->ui_draw_fill_rect) {
        ui_rect_t title_bar = ui_rect_make(0, 32, SCREEN_W, 40);
        s_lc.api->ui_draw_fill_rect(tile, &title_bar, 0x1A7B);
    }
    if (s_lc.api->ui_draw_string) {
        s_lc.api->ui_draw_string(tile, 30, 42,
                                 "LED 电平监视器",
                                 FONT_24, 0xFFFF, 0x1A7B);
    }

    /* ---------------- 分隔线 ---------------- */
    if (s_lc.api->ui_draw_fast_hline) {
        s_lc.api->ui_draw_fast_hline(tile, 20, 80, SCREEN_W - 40, 0xC618);
    }

    /* ---------------- 虚拟 LED (颜色跟实际引脚电平走) ----------------
     * ON (高电平) : 亮绿 0x07E0
     * OFF(低电平) : 暗灰 0x4208
     * 用嵌套 fill_rect 模拟"圆灯":外圈深色边框 + 内圈随状态变色 */
    {
        int16_t cx = SCREEN_W / 2;        // 240
        int16_t cy = 140;                 // 灯中心 y
        int16_t outer_half = 45;           // 外半边
        int16_t inner_half = 36;           // 内半边

        if (s_lc.api->ui_draw_fill_rect) {
            ui_rect_t led_outer = ui_rect_make(
                (int16_t)(cx - outer_half), (int16_t)(cy - outer_half),
                (uint16_t)(outer_half * 2), (uint16_t)(outer_half * 2));
            s_lc.api->ui_draw_fill_rect(tile, &led_outer, 0x0000);

            ui_rect_t led_inner = ui_rect_make(
                (int16_t)(cx - inner_half), (int16_t)(cy - inner_half),
                (uint16_t)(inner_half * 2), (uint16_t)(inner_half * 2));
            uint16_t led_color = s_lc.led_level ? 0x07E0 : 0x4208;
            s_lc.api->ui_draw_fill_rect(tile, &led_inner, led_color);
        }
    }

    /* ---------------- 状态文字 ---------------- */
    if (s_lc.api->ui_draw_string) {
        const char *state_str = s_lc.led_level ? "状态: 开启 (HIGH / ON)" : "状态: 关闭 (LOW / OFF)";
        uint16_t state_color = s_lc.led_level ? 0x05E0 : 0x4208;
        s_lc.api->ui_draw_string(tile, 120, 200, state_str, FONT_20, state_color, 0xFFFF);
    }

    /* ---------------- 引脚信息 ---------------- */
    if (s_lc.api->ui_draw_string) {
        s_lc.api->ui_draw_string(tile, 30, 240,
                                 "引脚: PICO_DEFAULT_LED (GPIO 25)",
                                 FONT_16, 0x4208, 0xFFFF);
    }

    /* ---------------- 按钮渲染 ---------------- */
    if (s_lc.api->ui_btn_draw) {
        s_lc.api->ui_btn_draw(tile, &s_lc.btn_stop);
    }
}

static void led_controller_exit(void) {
    if (s_lc.api && s_lc.api->post_log) {
        s_lc.api->post_log("LED Monitor Plugin Stopped.");
    }
    s_lc.api = NULL;
}

/* ================= 4. 描述符单例 ================= */

static const app_plugin_t s_led_controller_plugin = {
    .magic         = APP_MAGIC,        /* 0x50415050 ("PAPP") */
    .version       = 0x00010000u,
    .name          = "LED Monitor",
    .init          = led_controller_init,
    .update_core1  = led_controller_update_core1,
    .process_touch = led_controller_process_touch,
    .draw          = led_controller_draw,
    .exit          = led_controller_exit,
};

const app_plugin_t *led_controller_get_descriptor(void) {
    return &s_led_controller_plugin;
}
