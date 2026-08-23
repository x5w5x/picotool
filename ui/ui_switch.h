/*
 * @Author: 轩
 * @Date: 2026-08-19 21:48:56
 * @LastEditTime: 2026-08-21 12:27:47
 * @FilePath: \pico\ui\ui_switch.h
 */
#ifndef UI_SWITCH_H
#define UI_SWITCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_switch ui_switch_t;

// 开关状态变更回调函数类型[cite: 28]
typedef void (*ui_switch_cb_t)(ui_switch_t *sw, bool state, void *user_data);

struct ui_switch {
    ui_rect_t       rect;           // 控件物理包围盒[cite: 28]
    bool            is_checked;     // 开关状态 (true: 开, false: 关)[cite: 28]

    uint16_t        color_off;      // 关闭状态底轨颜色 (RGB565)[cite: 28]
    uint16_t        color_on;       // 开启状态底轨颜色 (RGB565)[cite: 28]
    uint16_t        color_knob;     // 圆形滑块手柄颜色 (RGB565)[cite: 28]

    // ⚡ 触控手势防抖与防误触状态机
    int16_t         down_x;
    int16_t         down_y;
    bool            is_touch_down;  // 是否在开关范围内按下
    bool            is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)

    ui_switch_cb_t  on_change;      // 状态变更回调[cite: 28]
    void           *user_data;      // 用户私有指针[cite: 28]
};

// 初始化开关[cite: 28]
void ui_switch_init(ui_switch_t *sw, int16_t x, int16_t y, int16_t w, int16_t h,
                    bool init_state, ui_switch_cb_t on_change);

// 设置开关状态 (自动触发回调)[cite: 28]
void ui_switch_set_state(ui_switch_t *sw, bool state);

// 设置开关全套配色[cite: 28]
void ui_switch_set_color(ui_switch_t *sw, uint16_t off_color, uint16_t on_color, uint16_t knob_color);

// 触控驱动状态机 (集成 8px 死区保护与 Touch-Up 松手触发)[cite: 28]
bool ui_switch_process_touch(ui_switch_t *sw, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (胶囊体底轨 + 边缘抗切边保护)[cite: 28]
void ui_switch_draw(ui_tile_t *tile, const ui_switch_t *sw);

// 获取开关脏矩形包围盒 (用于 ui_render_dirty_clipped 局部极速推流)
static inline ui_rect_t ui_switch_get_dirty_rect(const ui_switch_t *sw) {
    if (!sw) return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make(
        (int16_t)(sw->rect.x - 2),
        (int16_t)(sw->rect.y - 2),
        (int16_t)(sw->rect.w + 4),
        (int16_t)(sw->rect.h + 4)
    );
}

#ifdef __cplusplus
}
#endif

#endif // UI_SWITCH_H