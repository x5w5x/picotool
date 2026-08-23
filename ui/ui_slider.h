/*
 * @Author: 轩
 * @Date: 2026-08-19 21:42:20
 * @LastEditTime: 2026-08-21 12:24:21
 * @FilePath: \pico\ui\ui_slider.h
 */
#ifndef UI_SLIDER_H
#define UI_SLIDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_slider ui_slider_t;

// 滑块数值变更回调函数类型
typedef void (*ui_slider_cb_t)(ui_slider_t *slider, int32_t val, void *user_data);

typedef enum {
    UI_SLIDER_DIR_HOR = 0, // 水平滑动
    UI_SLIDER_DIR_VER      // 垂直滑动
} ui_slider_dir_t;

struct ui_slider {
    ui_rect_t       rect;           // 控件本体包围盒（触控感应区域）
    ui_slider_dir_t dir;            // 滑动方向
    
    int32_t         min_val;        // 最小值
    int32_t         max_val;        // 最大值
    int32_t         cur_val;        // 当前值
    int32_t         step;           // 步进值（<=1 为平滑连续）

    uint16_t        color_track;    // 轨道底色 (RGB565)
    uint16_t        color_fill;     // 进度激活填充色 (RGB565)
    uint16_t        color_knob;     // 滑块手柄普通色 (RGB565)
    uint8_t         knob_r;         // 滑块圆形手柄半径 (px)

    // ⚡ 触控手势防抖与全局捕获状态机
    int16_t         down_x;
    int16_t         down_y;
    bool            is_touch_down;  // 是否在滑块区域内按下
    bool            is_dragging;    // 是否处于拖拽滑动中

    ui_slider_cb_t  on_change;      // 数值变化回调
    void           *user_data;      // 用户私有数据
};

// 初始化滑动条
void ui_slider_init(ui_slider_t *slider, int16_t x, int16_t y, int16_t w, int16_t h,
                    int32_t min_val, int32_t max_val, int32_t cur_val,
                    ui_slider_cb_t on_change);

// 设置当前值（自动边界钳位并触发回调）
void ui_slider_set_val(ui_slider_t *slider, int32_t val);

// 设置自定义配色
void ui_slider_set_color(ui_slider_t *slider, uint16_t track, uint16_t fill, uint16_t knob);

// 触控状态机 (集成 8px 死区保护与 Touch Capture 全局拖拽捕获)
bool ui_slider_process_touch(ui_slider_t *slider, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (外扩手柄半径安全相交裁剪)
void ui_slider_draw(ui_tile_t *tile, const ui_slider_t *slider);

// 获取滑块完整脏矩形 (含 knob_r 外扩，用于 ui_render_dirty_clipped 局部极速推流)
static inline ui_rect_t ui_slider_get_dirty_rect(const ui_slider_t *slider) {
    if (!slider) return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make(
        (int16_t)(slider->rect.x - slider->knob_r - 2),
        (int16_t)(slider->rect.y - slider->knob_r - 2),
        (int16_t)(slider->rect.w + (slider->knob_r + 2) * 2),
        (int16_t)(slider->rect.h + (slider->knob_r + 2) * 2)
    );
}

#ifdef __cplusplus
}
#endif

#endif // UI_SLIDER_H