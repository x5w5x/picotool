#include "ui_slider.h"
#include "ui_draw.h"
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

/* ================= 1. 内部辅助函数 ================= */

static inline bool ui_slider_hit_test(const ui_slider_t *slider, int16_t x, int16_t y) {
    // 扩展手柄半径 + 6px 触控容差，显著提高触控跟手度
    int16_t pad = slider->knob_r + 6;
    return (x >= (slider->rect.x - pad) && x < (slider->rect.x + slider->rect.w + pad) &&
            y >= (slider->rect.y - pad) && y < (slider->rect.y + slider->rect.h + pad));
}

static inline int32_t clamp_val(int32_t v, int32_t min_v, int32_t max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

/* ================= 2. 初始化与设值 ================= */

void ui_slider_init(ui_slider_t *slider, int16_t x, int16_t y, int16_t w, int16_t h,
                    int32_t min_val, int32_t max_val, int32_t cur_val,
                    ui_slider_cb_t on_change) {
    if (!slider) return;

    slider->rect = ui_rect_make(x, y, w, h);
    slider->dir  = (w >= h) ? UI_SLIDER_DIR_HOR : UI_SLIDER_DIR_VER;

    slider->min_val = min_val;
    slider->max_val = max_val;
    slider->cur_val = clamp_val(cur_val, min_val, max_val);
    slider->step    = 1;

    slider->color_track = 0x2104; // 深暗灰底轨
    slider->color_fill  = 0x07E0; // 亮绿激活进度
    slider->color_knob  = 0xFFFF; // 纯白手柄
    slider->knob_r      = (slider->dir == UI_SLIDER_DIR_HOR) ? (uint8_t)(h / 2 + 3) : (uint8_t)(w / 2 + 3);

    slider->down_x        = 0;
    slider->down_y        = 0;
    slider->is_touch_down = false;
    slider->is_dragging   = false;

    slider->on_change = on_change;
    slider->user_data = NULL;
}

void ui_slider_set_val(ui_slider_t *slider, int32_t val) {
    if (!slider) return;
    int32_t clamped = clamp_val(val, slider->min_val, slider->max_val);
    if (slider->cur_val != clamped) {
        slider->cur_val = clamped;
        if (slider->on_change) {
            slider->on_change(slider, slider->cur_val, slider->user_data);
        }
    }
}

void ui_slider_set_color(ui_slider_t *slider, uint16_t track, uint16_t fill, uint16_t knob) {
    if (!slider) return;
    slider->color_track = track;
    slider->color_fill  = fill;
    slider->color_knob  = knob;
}

/* ================= 3. 触控拖拽状态机 (Touch Capture + 8px 死区) ================= */

bool ui_slider_process_touch(ui_slider_t *slider, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!slider) return false;

    bool value_changed = false;

    if (is_touching) {
        if (!slider->is_touch_down) {
            // A. 初次按下 (Touch Down): 命中判定
            if (ui_slider_hit_test(slider, tx, ty)) {
                slider->is_touch_down = true;
                slider->is_dragging   = true; // 获得 Touch Capture，进入拖拽追踪模式
                slider->down_x        = tx;
                slider->down_y        = ty;
            }
        }

        // B. 持续移动 (Touch Move): 只要获得 Touch Capture，哪怕滑出感应区依然平滑调节
        if (slider->is_touch_down && slider->is_dragging) {
            int32_t range = slider->max_val - slider->min_val;
            if (range <= 0) range = 1;

            int32_t new_val;
            if (slider->dir == UI_SLIDER_DIR_HOR) {
                // 水平方向物理映射 (左侧 min -> 右侧 max)
                int16_t rel_x = tx - slider->rect.x;
                if (rel_x < 0) rel_x = 0;
                if (rel_x > slider->rect.w) rel_x = slider->rect.w;
                new_val = slider->min_val + (int32_t)(((int64_t)rel_x * range) / slider->rect.w);
            } else {
                // 垂直方向物理映射 (底部 min -> 顶部 max)
                int16_t rel_y = (slider->rect.y + slider->rect.h) - ty;
                if (rel_y < 0) rel_y = 0;
                if (rel_y > slider->rect.h) rel_y = slider->rect.h;
                new_val = slider->min_val + (int32_t)(((int64_t)rel_y * range) / slider->rect.h);
            }

            // 步进对齐
            if (slider->step > 1) {
                new_val = ((new_val + slider->step / 2) / slider->step) * slider->step;
            }
            new_val = clamp_val(new_val, slider->min_val, slider->max_val);

            if (slider->cur_val != new_val) {
                slider->cur_val = new_val;
                value_changed = true;
                if (slider->on_change) {
                    slider->on_change(slider, slider->cur_val, slider->user_data);
                }
            }
            return value_changed || slider->is_touch_down;
        }
    } else {
        // C. ⚡ 手指松开 (Touch Up): 释放手势锁
        if (slider->is_touch_down) {
            slider->is_touch_down = false;
            slider->is_dragging   = false;
            return true; // 重绘恢复手柄高亮颜色
        }
    }

    return false;
}

/* ================= 4. 切片光栅化渲染 (外扩手柄半径硬裁剪) ================= */

void ui_slider_draw(ui_tile_t *tile, const ui_slider_t *slider) {
    if (!tile || !slider) return;

    // 1. 包围盒外扩手柄半径，执行相交裁剪判定
    ui_rect_t bound = ui_slider_get_dirty_rect(slider);
    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &bound, &overlap)) {
        return;
    }

    int32_t range = slider->max_val - slider->min_val;
    if (range <= 0) range = 1;

    // 2. 水平滑动条绘制
    if (slider->dir == UI_SLIDER_DIR_HOR) {
        int16_t track_h = (slider->rect.h > 4) ? (slider->rect.h / 3) : 3;
        if (track_h < 3) track_h = 3;
        int16_t track_y = slider->rect.y + (slider->rect.h - track_h) / 2;

        // 计算滑块中心与已填充宽度
        int32_t progress_w = ((int64_t)(slider->cur_val - slider->min_val) * slider->rect.w) / range;
        int16_t knob_x = slider->rect.x + (int16_t)progress_w;
        int16_t knob_y = slider->rect.y + slider->rect.h / 2;

        // A. 绘制未填充背景轨道
        ui_rect_t bg_track = ui_rect_make(
            (int16_t)(slider->rect.x + progress_w),
            track_y,
            (int16_t)(slider->rect.w - progress_w),
            track_h
        );
        ui_draw_fill_rect(tile, &bg_track, slider->color_track);

        // B. 绘制已激活填充轨道
        if (progress_w > 0) {
            ui_rect_t fill_track = ui_rect_make(slider->rect.x, track_y, (int16_t)progress_w, track_h);
            ui_draw_fill_rect(tile, &fill_track, slider->color_fill);
        }

        // C. 绘制圆形手柄 (拖拽时金黄高亮，静止时纯白，内部带黑心造型)
        uint16_t k_color = slider->is_dragging ? 0xFFE0 : slider->color_knob;
        ui_draw_fill_circle(tile, knob_x, knob_y, slider->knob_r, k_color);
        if (slider->knob_r > 3) {
            ui_draw_fill_circle(tile, knob_x, knob_y, slider->knob_r - 3, 0x10A2); // 极深蓝黑心造型
        }
    }
    // 3. 垂直滑动条绘制
    else {
        int16_t track_w = (slider->rect.w > 4) ? (slider->rect.w / 3) : 3;
        if (track_w < 3) track_w = 3;
        int16_t track_x = slider->rect.x + (slider->rect.w - track_w) / 2;

        int32_t progress_h = ((int64_t)(slider->cur_val - slider->min_val) * slider->rect.h) / range;
        int16_t knob_x = slider->rect.x + slider->rect.w / 2;
        int16_t knob_y = (slider->rect.y + slider->rect.h) - (int16_t)progress_h;

        // A. 绘制顶部未填充轨道
        ui_rect_t bg_track = ui_rect_make(track_x, slider->rect.y, track_w, (int16_t)(slider->rect.h - progress_h));
        ui_draw_fill_rect(tile, &bg_track, slider->color_track);

        // B. 绘制底部已激活填充轨道
        if (progress_h > 0) {
            ui_rect_t fill_track = ui_rect_make(track_x, knob_y, track_w, (int16_t)progress_h);
            ui_draw_fill_rect(tile, &fill_track, slider->color_fill);
        }

        // C. 绘制垂直圆形手柄
        uint16_t k_color = slider->is_dragging ? 0xFFE0 : slider->color_knob;
        ui_draw_fill_circle(tile, knob_x, knob_y, slider->knob_r, k_color);
        if (slider->knob_r > 3) {
            ui_draw_fill_circle(tile, knob_x, knob_y, slider->knob_r - 3, 0x10A2);
        }
    }
}