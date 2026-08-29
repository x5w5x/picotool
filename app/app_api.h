#ifndef APP_API_H
#define APP_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_tile.h"
#include "ui_btn.h"    // plugin 需要 ui_btn_t 与 ui_btn_cb_t 类型才能注册按钮回调
#include "chinese.h"   // FontSize_t / FONT_16 / FONT_20 / FONT_24

#define APP_MAGIC       0x50415050 // "PAPP"
#define APP_RAM_BASE    0x20038000 // 动态 App 物理基地址 (32KB 空间)
#define APP_RAM_SIZE    (32 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 主系统导出的 API 表 ================= */
typedef struct {
    uint32_t version;

    // 系统时钟与延时
    uint32_t (*get_time_ms)(void);
    uint64_t (*get_time_us)(void);
    void     (*delay_ms)(uint32_t ms);
    void     (*delay_us)(uint64_t us);

    // 屏幕显示 — LCD 原语(由主系统统一安全调度)
    void     (*lcd_set_window)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void     (*lcd_bulk_write)(const uint8_t *buf, uint32_t len);
    void     (*lcd_fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

    // 硬件外设读 — 暴露给 plugin 读取真实引脚电平
    bool     (*gpio_get)(uint32_t pin);

    // 系统交互
    void     (*post_log)(const char *str);
    void     (*request_exit)(void);
    void     (*request_redraw)(void);   // plugin 状态变化时请求主系统下一帧全屏重画

    /* ===== UI 渲染组件 (Core 0 上下文,即 plugin draw() 内调用) =====
     * plugin 是 freestanding (-nostdlib -nodefaultlibs),不链 libuigeom.a,
     * 所有 UI 函数必须通过 host_api 函数指针调用,不能直接调 ui_* 符号。
     * update_core1() 严禁碰以下任何 UI 函数(会跟 Core 0 的 LCD DMA 抢总线)。 */
    void (*ui_draw_fill_rect)(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color);
    void (*ui_draw_fast_hline)(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, uint16_t color);
    void (*ui_draw_string)   (ui_tile_t *tile, int16_t x, int16_t y, const char *str,
                              FontSize_t size, uint16_t color, uint16_t bg);

    /* 控件:按钮(同样跑 Core 0,host 框架在 loader_app_process_touch 里调用
     * ui_btn_process_touch,在 loader_app_draw 里调用 ui_btn_draw) */
    void (*ui_btn_init)         (ui_btn_t *btn, int16_t x, int16_t y, int16_t w, int16_t h,
                                 const char *text, ui_btn_cb_t on_click);
    void (*ui_btn_set_color)    (ui_btn_t *btn, uint16_t bg, uint16_t active, uint16_t text);
    bool (*ui_btn_process_touch)(ui_btn_t *btn, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
    void (*ui_btn_draw)         (ui_tile_t *tile, const ui_btn_t *btn);
} pico_host_api_t;

/* ================= 动态 App 导出的生命周期接口 ================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    const char *name;

    bool (*init)(const pico_host_api_t *api);
    void (*update_core1)(void); // Core 1 独占主循环
    void (*process_touch)(bool is_touch, int16_t x, int16_t y);
    void (*draw)(ui_tile_t *tile);
    void (*exit)(void);
} app_plugin_t;

// 入口点：二进制偏移 0x00 处执行
typedef const app_plugin_t* (*app_entry_t)(const pico_host_api_t *api);

#ifdef __cplusplus
}
#endif

#endif // APP_API_H