#include "ui_stepper.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX       8   // 8 像素滑动防抖死区
#define INITIAL_REPEAT_MS   400 // 长按初始触发延时 (400ms)
#define FASTEST_REPEAT_MS   50  // 长按最高连发频率 (50ms/次)

static inline int32_t clamp_val(int32_t v, int32_t min_v, int32_t max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

void ui_stepper_init(ui_stepper_t *sp, int16_t x, int16_t y, int16_t w, int16_t h,
                     int32_t min_val, int32_t max_val, int32_t cur_val, int32_t step,
                     const char *unit, ui_stepper_change_cb_t on_change) {
    if (!sp) return;

    sp->rect    = ui_rect_make(x, y, w, h);
    sp->min_val = min_val;
    sp->max_val = (max_val > min_val) ? max_val : min_val + 1;
    sp->cur_val = clamp_val(cur_val, sp->min_val, sp->max_val);
    sp->step    = (step > 0) ? step : 1;

    sp->unit    = unit ? unit : "";
    sp->prefix  = NULL;

    sp->hit_target         = UI_STEPPER_HIT_NONE;
    sp->down_x             = 0;
    sp->down_y             = 0;
    sp->is_touch_down      = false;
    sp->is_dragged         = false;
    sp->press_start_tick   = 0;
    sp->last_step_tick     = 0;
    sp->repeat_interval_ms = INITIAL_REPEAT_MS;

    sp->color_bg         = 0x0842; // 中心深灰底
    sp->color_btn_bg     = 0x2126; // 按钮暗灰底
    sp->color_btn_active = 0x07E0; // 按下亮绿高亮
    sp->color_border     = 0x39E7; // 边框深灰
    sp->color_text       = 0xFFFF; // 纯白字
    sp->color_unit       = 0x07FF; // 青蓝单位文本

    sp->on_change      = on_change;
    sp->on_click_val   = NULL;
    sp->user_data      = NULL;
}

void ui_stepper_set_val(ui_stepper_t *sp, int32_t val) {
    if (!sp) return;
    int32_t clamped = clamp_val(val, sp->min_val, sp->max_val);
    if (sp->cur_val != clamped) {
        sp->cur_val = clamped;
        if (sp->on_change) {
            sp->on_change(sp, sp->cur_val, sp->user_data);
        }
    }
}

void ui_stepper_set_prefix(ui_stepper_t *sp, const char *prefix) {
    if (sp) sp->prefix = prefix;
}

/* ================= 触控状态机 (短按点选 + 长按动态加速连发) ================= */

bool ui_stepper_process_touch(ui_stepper_t *sp, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!sp) return false;

    int16_t btn_w = sp->rect.h; // 减/加按钮宽度等于控件高度 (正方形手柄)
    ui_rect_t dec_rect = ui_rect_make(sp->rect.x, sp->rect.y, btn_w, sp->rect.h);
    ui_rect_t inc_rect = ui_rect_make((int16_t)(sp->rect.x + sp->rect.w - btn_w), sp->rect.y, btn_w, sp->rect.h);
    ui_rect_t val_rect = ui_rect_make((int16_t)(sp->rect.x + btn_w), sp->rect.y, (int16_t)(sp->rect.w - 2 * btn_w), sp->rect.h);

    bool need_redraw = false;

    if (is_touching) {
        if (!sp->is_touch_down) {
            // A. 初次按下 (Touch Down): 判定具体命中区域
            if (ui_rect_contains_point(&dec_rect, tx, ty)) {
                sp->is_touch_down      = true;
                sp->hit_target         = UI_STEPPER_HIT_DEC;
                sp->down_x             = tx;
                sp->down_y             = ty;
                sp->is_dragged         = false;
                sp->press_start_tick   = now_tick;
                sp->last_step_tick     = now_tick;
                sp->repeat_interval_ms = INITIAL_REPEAT_MS;

                // 立即执行单次减步进
                if (sp->cur_val > sp->min_val) {
                    sp->cur_val = clamp_val(sp->cur_val - sp->step, sp->min_val, sp->max_val);
                    if (sp->on_change) sp->on_change(sp, sp->cur_val, sp->user_data);
                }
                need_redraw = true;
            } else if (ui_rect_contains_point(&inc_rect, tx, ty)) {
                sp->is_touch_down      = true;
                sp->hit_target         = UI_STEPPER_HIT_INC;
                sp->down_x             = tx;
                sp->down_y             = ty;
                sp->is_dragged         = false;
                sp->press_start_tick   = now_tick;
                sp->last_step_tick     = now_tick;
                sp->repeat_interval_ms = INITIAL_REPEAT_MS;

                // 立即执行单次加步进
                if (sp->cur_val < sp->max_val) {
                    sp->cur_val = clamp_val(sp->cur_val + sp->step, sp->min_val, sp->max_val);
                    if (sp->on_change) sp->on_change(sp, sp->cur_val, sp->user_data);
                }
                need_redraw = true;
            } else if (ui_rect_contains_point(&val_rect, tx, ty)) {
                sp->is_touch_down = true;
                sp->hit_target    = UI_STEPPER_HIT_VAL;
                sp->down_x        = tx;
                sp->down_y        = ty;
                sp->is_dragged    = false;
            }
        } else {
            // B. 持续按压 (Touch Hold): 8px 防抖死区检测
            int16_t dx = abs(tx - sp->down_x);
            int16_t dy = abs(ty - sp->down_y);
            if (!sp->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                sp->is_dragged = true; // 锁定为滑动，放弃长按连发
            }

            // ⚡ 长按动态加速连发逻辑
            if (!sp->is_dragged && (sp->hit_target == UI_STEPPER_HIT_DEC || sp->hit_target == UI_STEPPER_HIT_INC)) {
                uint32_t hold_time = now_tick - sp->press_start_tick;
                if (hold_time > 300) {
                    if (now_tick - sp->last_step_tick >= sp->repeat_interval_ms) {
                        sp->last_step_tick = now_tick;

                        // 线性加速递减连发间隔
                        if (sp->repeat_interval_ms > FASTEST_REPEAT_MS) {
                            sp->repeat_interval_ms = (sp->repeat_interval_ms > 20) ? (sp->repeat_interval_ms - 20) : FASTEST_REPEAT_MS;
                        }

                        if (sp->hit_target == UI_STEPPER_HIT_DEC && sp->cur_val > sp->min_val) {
                            sp->cur_val = clamp_val(sp->cur_val - sp->step, sp->min_val, sp->max_val);
                            if (sp->on_change) sp->on_change(sp, sp->cur_val, sp->user_data);
                            need_redraw = true;
                        } else if (sp->hit_target == UI_STEPPER_HIT_INC && sp->cur_val < sp->max_val) {
                            sp->cur_val = clamp_val(sp->cur_val + sp->step, sp->min_val, sp->max_val);
                            if (sp->on_change) sp->on_change(sp, sp->cur_val, sp->user_data);
                            need_redraw = true;
                        }
                    }
                }
            }
        }
        return need_redraw;
    } else {
        // C. ⚡ 手指松开 (Touch Up)
        if (sp->is_touch_down) {
            sp->is_touch_down = false;

            // 若松手点在中心数值区域且未发生滑动，触发数值框点击回调
            if (!sp->is_dragged && sp->hit_target == UI_STEPPER_HIT_VAL) {
                if (ui_rect_contains_point(&val_rect, tx, ty) && sp->on_click_val) {
                    sp->on_click_val(sp, sp->user_data);
                }
            }

            sp->hit_target = UI_STEPPER_HIT_NONE;
            sp->is_dragged = false;
            return true; // 恢复按钮非高亮底色
        }
    }

    return false;
}

/* ================= 切片光栅化渲染 (三段式独立切片相交裁剪) ================= */

void ui_stepper_draw(ui_tile_t *tile, const ui_stepper_t *sp) {
    if (!tile || !sp) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &sp->rect, &overlap)) {
        return;
    }

    int16_t btn_w = sp->rect.h;
    ui_rect_t dec_rect = ui_rect_make(sp->rect.x, sp->rect.y, btn_w, sp->rect.h);
    ui_rect_t inc_rect = ui_rect_make((int16_t)(sp->rect.x + sp->rect.w - btn_w), sp->rect.y, btn_w, sp->rect.h);
    ui_rect_t val_rect = ui_rect_make((int16_t)(sp->rect.x + btn_w), sp->rect.y, (int16_t)(sp->rect.w - 2 * btn_w), sp->rect.h);

    // 1. 绘制左侧减号按钮 [-]
    ui_rect_t dec_ov;
    if (ui_rect_intersect(&tile->rect, &dec_rect, &dec_ov)) {
        bool is_dec_active = (sp->is_touch_down && sp->hit_target == UI_STEPPER_HIT_DEC);
        uint16_t dec_bg = is_dec_active ? sp->color_btn_active : sp->color_btn_bg;

        ui_draw_fill_rect(tile, &dec_ov, dec_bg);
        ui_draw_rect(tile, &dec_rect, sp->color_border);

        // 居中绘制 "-" 号
        int16_t text_y = dec_rect.y + (dec_rect.h - 16) / 2;
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_string(tile, dec_rect.x + (btn_w - 8) / 2, text_y, "-", FONT_16, 
                           is_dec_active ? 0x0000 : sp->color_text, dec_bg);
        }
    }

    // 2. 绘制中间数值回显框 [ Prefix + Value + Unit ]
    ui_rect_t val_ov;
    if (ui_rect_intersect(&tile->rect, &val_rect, &val_ov)) {
        ui_draw_fill_rect(tile, &val_ov, sp->color_bg);
        ui_draw_rect(tile, &val_rect, sp->color_border);

        char num_str[32];
        snprintf(num_str, sizeof(num_str), "%ld", (long)sp->cur_val);

        int16_t num_w  = (int16_t)strlen(num_str) * 8;
        int16_t unit_w = sp->unit ? ((int16_t)strlen(sp->unit) * 8 + 4) : 0;
        int16_t pref_w = sp->prefix ? ((int16_t)strlen(sp->prefix) * 8 + 4) : 0;
        int16_t total_w = pref_w + num_w + unit_w;

        int16_t start_x = val_rect.x + (val_rect.w - total_w) / 2;
        int16_t text_y  = val_rect.y + (val_rect.h - 16) / 2;

        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_set_clip(&val_rect);

            // 绘制前缀
            if (sp->prefix) {
                ui_draw_string(tile, start_x, text_y, sp->prefix, FONT_16, 0x8410, sp->color_bg);
                start_x += pref_w;
            }

            // 绘制当前数值
            ui_draw_string(tile, start_x, text_y, num_str, FONT_16, sp->color_text, sp->color_bg);
            start_x += num_w;

            // 绘制单位
            if (sp->unit && sp->unit[0] != '\0') {
                ui_draw_string(tile, start_x + 4, text_y, sp->unit, FONT_16, sp->color_unit, sp->color_bg);
            }

            ui_draw_reset_clip();
        }
    }

    // 3. 绘制右侧加号按钮 [+]
    ui_rect_t inc_ov;
    if (ui_rect_intersect(&tile->rect, &inc_rect, &inc_ov)) {
        bool is_inc_active = (sp->is_touch_down && sp->hit_target == UI_STEPPER_HIT_INC);
        uint16_t inc_bg = is_inc_active ? sp->color_btn_active : sp->color_btn_bg;

        ui_draw_fill_rect(tile, &inc_ov, inc_bg);
        ui_draw_rect(tile, &inc_rect, sp->color_border);

        // 居中绘制 "+" 号
        int16_t text_y = inc_rect.y + (inc_rect.h - 16) / 2;
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_string(tile, inc_rect.x + (btn_w - 8) / 2, text_y, "+", FONT_16, 
                           is_inc_active ? 0x0000 : sp->color_text, inc_bg);
        }
    }
}