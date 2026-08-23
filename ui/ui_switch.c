#include "ui_switch.h"
#include "ui_draw.h"
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

/* ================= 1. 内部辅助函数 ================= */

static inline bool ui_switch_hit_test(const ui_switch_t *sw, int16_t x, int16_t ty) {
    // 扩展 4px 触控容差，增强微型开关的触控命中率
    return (x >= (sw->rect.x - 4) && x < (sw->rect.x + sw->rect.w + 4) &&
            ty >= (sw->rect.y - 4) && ty < (sw->rect.y + sw->rect.h + 4));
}

/* ================= 2. 初始化与配置 ================= */

void ui_switch_init(ui_switch_t *sw, int16_t x, int16_t y, int16_t w, int16_t h,
                    bool init_state, ui_switch_cb_t on_change) {
    if (!sw) return;

    sw->rect = ui_rect_make(x, y, w, h);
    sw->is_checked = init_state;

    sw->down_x        = 0;
    sw->down_y        = 0;
    sw->is_touch_down = false;
    sw->is_dragged    = false;

    sw->color_off  = 0x39E7; // 默认深灰关闭轨
    sw->color_on   = 0x07E0; // 默认亮绿开启轨
    sw->color_knob = 0xFFFF; // 默认纯白手柄

    sw->on_change  = on_change;
    sw->user_data  = NULL;
}

void ui_switch_set_state(ui_switch_t *sw, bool state) {
    if (!sw) return;
    if (sw->is_checked != state) {
        sw->is_checked = state;
        if (sw->on_change) {
            sw->on_change(sw, sw->is_checked, sw->user_data);
        }
    }
}

void ui_switch_set_color(ui_switch_t *sw, uint16_t off_color, uint16_t on_color, uint16_t knob_color) {
    if (!sw) return;
    sw->color_off  = off_color;
    sw->color_on   = on_color;
    sw->color_knob = knob_color;
}

/* ================= 3. 触控状态机 (8px 防抖 + Touch-Up 触发) ================= */

bool ui_switch_process_touch(ui_switch_t *sw, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!sw) return false;

    if (is_touching) {
        if (!sw->is_touch_down) {
            // A. 初次按下 (Touch Down): 仅在命中本开关时记录起点
            if (ui_switch_hit_test(sw, tx, ty)) {
                sw->is_touch_down = true;
                sw->is_dragged    = false;
                sw->down_x        = tx;
                sw->down_y        = ty;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区判定
            int16_t dx = abs(tx - sw->down_x);
            int16_t dy = abs(ty - sw->down_y);
            if (!sw->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                sw->is_dragged = true; // 锁定为滑动，放弃翻转开关
            }
        }
        return false;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许翻转开关状态的时机
        if (sw->is_touch_down) {
            sw->is_touch_down = false;

            // 只有未发生滑动的纯点按，且松开点仍在开关附近才触发翻转
            if (!sw->is_dragged && ui_switch_hit_test(sw, tx, ty)) {
                sw->is_checked = !sw->is_checked;
                if (sw->on_change) {
                    sw->on_change(sw, sw->is_checked, sw->user_data);
                }
                sw->is_dragged = false;
                return true; // 状态改变，触发局部切片重绘
            }
            sw->is_dragged = false;
        }
    }

    return false;
}

/* ================= 4. 切片光栅化渲染 ================= */

void ui_switch_draw(ui_tile_t *tile, const ui_switch_t *sw) {
    if (!tile || !sw) return;

    // 1. 切片相交快速裁剪 (Early-Culling)
    ui_rect_t dirty_bound = ui_switch_get_dirty_rect(sw);
    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &dirty_bound, &overlap)) {
        return;
    }

    int16_t r = sw->rect.h / 2;
    int16_t left_cx  = sw->rect.x + r;
    int16_t right_cx = sw->rect.x + sw->rect.w - r;
    int16_t cy       = sw->rect.y + r;

    // 2. 绘制胶囊形底轨 (两侧半圆 + 中间矩形连接段)
    uint16_t track_color = sw->is_checked ? sw->color_on : sw->color_off;
    ui_draw_fill_circle(tile, left_cx, cy, r, track_color);
    ui_draw_fill_circle(tile, right_cx, cy, r, track_color);

    if (sw->rect.w > sw->rect.h) {
        ui_rect_t mid_rect = ui_rect_make(left_cx, sw->rect.y, (int16_t)(sw->rect.w - 2 * r), sw->rect.h);
        ui_draw_fill_rect(tile, &mid_rect, track_color);
    }

    // 3. 绘制圆形滑块手柄 (内缩 2px 呈现立体感，开启居右，关闭居左)
    int16_t knob_r = (r > 2) ? (r - 2) : r;
    int16_t knob_cx = sw->is_checked ? right_cx : left_cx;

    ui_draw_fill_circle(tile, knob_cx, cy, knob_r, sw->color_knob);
}