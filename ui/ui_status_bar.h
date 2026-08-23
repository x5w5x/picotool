#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_status_bar ui_status_bar_t;

typedef enum {
    UI_SB_HIT_NONE = 0,
    UI_SB_HIT_TITLE,
    UI_SB_HIT_TELEMETRY,
    UI_SB_HIT_W25,
    UI_SB_HIT_SD,
    UI_SB_HIT_TOUCH,
    UI_SB_HIT_USB
} ui_sb_hit_t;

typedef void (*ui_status_bar_click_cb_t)(ui_status_bar_t *sb, ui_sb_hit_t hit_area, void *user_data);

struct ui_status_bar {
    ui_rect_t               rect;
    char                    title[32];

    uint8_t                 cpu_usage;
    uint8_t                 fps;
    

    bool                    w25_ok;
    bool                    sd_mounted;
    bool                    touch_active;
    bool                    usb_connected;

    int16_t                 down_x;
    int16_t                 down_y;

    int16_t                 chip_temp;
    bool                    is_touch_down;
    bool                    is_dragged;

    uint16_t                color_bg;
    uint16_t                color_border;
    uint16_t                color_title;
    uint16_t                color_telemetry;
    uint16_t                color_icon_active;
    uint16_t                color_icon_inactive;
    uint16_t                color_icon_touch;

    ui_status_bar_click_cb_t on_click;
    void                   *user_data;
};

void ui_status_bar_init(ui_status_bar_t *sb, int16_t x, int16_t y, int16_t w, int16_t h,
                        const char *title, ui_status_bar_click_cb_t on_click);

void ui_status_bar_set_title(ui_status_bar_t *sb, const char *title);
void ui_status_bar_set_telemetry(ui_status_bar_t *sb, uint8_t cpu_usage, int16_t temp,uint8_t fps);
void ui_status_bar_set_w25(ui_status_bar_t *sb, bool is_ok);
void ui_status_bar_set_sd(ui_status_bar_t *sb, bool is_mounted);
void ui_status_bar_set_touch(ui_status_bar_t *sb, bool is_active);
void ui_status_bar_set_usb(ui_status_bar_t *sb, bool is_connected);

bool ui_status_bar_process_touch(ui_status_bar_t *sb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
void ui_status_bar_draw(ui_tile_t *tile, const ui_status_bar_t *sb);

// 获取状态栏整体脏矩形
static inline ui_rect_t ui_status_bar_get_dirty_rect(const ui_status_bar_t *sb) {
    return sb ? sb->rect : ui_rect_make(0, 0, 0, 0);
}

// ⚡ 仅获取 CPU% 与 FPS 字符所在的微型脏矩形 (X: 196 ~ 340, Y: 4 ~ 26, 宽 144px)
static inline ui_rect_t ui_status_bar_get_telemetry_rect(const ui_status_bar_t *sb) {
    if (!sb) return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make((int16_t)(sb->rect.x + 196), (int16_t)(sb->rect.y + 4), 144, 22);
}

// ⚡ 仅获取 Touch 靶心图标微型脏矩形 (X: 414 ~ 434, Y: 4 ~ 26, 宽 20px)
static inline ui_rect_t ui_status_bar_get_touch_icon_rect(const ui_status_bar_t *sb) {
    if (!sb) return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make((int16_t)(sb->rect.x + 414), (int16_t)(sb->rect.y + 4), 20, 22);
}

#ifdef __cplusplus
}
#endif

#endif // UI_STATUS_BAR_H