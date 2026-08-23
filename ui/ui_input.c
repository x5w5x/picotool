#include "ui_input.h"
#include "ui_draw.h"
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_input_init(ui_input_t *input, int16_t x, int16_t y, int16_t w, int16_t h, 
                   const char *placeholder, ui_input_cb_t on_focus) {
    if (!input) return;

    input->rect = ui_rect_make(x, y, w, h);
    input->text[0] = '\0';
    input->placeholder = placeholder ? placeholder : "";
    input->cursor_pos = 0;
    input->is_focused = false;

    input->color_bg           = 0x1082; // 极深蓝黑底
    input->color_border       = 0x39E7; // 默认深灰边框
    input->color_focus_border = 0x07FF; // 聚焦时青蓝高亮光圈
    input->color_text         = 0xFFFF; // 纯白字
    input->color_placeholder  = 0x8410; // 暗灰提示字

    input->down_x        = 0;
    input->down_y        = 0;
    input->is_touch_down = false;
    input->is_dragged    = false;

    input->on_focus   = on_focus;
    input->user_data  = NULL;
}

void ui_input_set_text(ui_input_t *input, const char *str) {
    if (!input) return;
    if (str) {
        strncpy(input->text, str, UI_INPUT_MAX_LEN - 1);
        input->text[UI_INPUT_MAX_LEN - 1] = '\0';
        input->cursor_pos = (uint8_t)strlen(input->text);
    } else {
        input->text[0] = '\0';
        input->cursor_pos = 0;
    }
}

void ui_input_clear(ui_input_t *input) {
    if (!input) return;
    input->text[0] = '\0';
    input->cursor_pos = 0;
}

bool ui_input_process_touch(ui_input_t *input, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!input) return false;

    if (is_touching) {
        if (!input->is_touch_down) {
            // A. 初次按下 (Touch Down): 仅在命中本输入框时记录起点
            if (ui_rect_contains_point(&input->rect, tx, ty)) {
                input->is_touch_down = true;
                input->is_dragged    = false;
                input->down_x        = tx;
                input->down_y        = ty;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区判定
            int16_t dx = abs(tx - input->down_x);
            int16_t dy = abs(ty - input->down_y);
            if (!input->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                input->is_dragged = true; // 锁定为滑动，放弃聚焦
            }
        }
        return false;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许触发聚焦与呼出键盘的时机
        if (input->is_touch_down) {
            input->is_touch_down = false;

            // 只有未发生滑动的纯点按，且松开点仍在输入框内才触发聚焦
            if (!input->is_dragged && ui_rect_contains_point(&input->rect, tx, ty)) {
                input->is_focused = true;
                if (input->on_focus) {
                    input->on_focus(input, input->user_data);
                }
                return true;
            }
            input->is_dragged = false;
        }
    }

    return false;
}

void ui_input_draw(ui_tile_t *tile, const ui_input_t *input) {
    if (!tile || !input) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &input->rect, &overlap)) return;

    // 1. 绘制底色与边框 (相交裁剪安全保护)
    ui_draw_fill_rect(tile, &overlap, input->color_bg);
    ui_draw_rect(tile, &input->rect, input->is_focused ? input->color_focus_border : input->color_border);

    // 2. 局部视口裁剪
    ui_draw_set_clip(&input->rect);

    int16_t text_x = input->rect.x + 8;
    int16_t text_y = input->rect.y + (input->rect.h - 16) / 2;

    if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
        if (input->text[0] != '\0') {
            ui_draw_string(tile, text_x, text_y, input->text, FONT_16, input->color_text, input->color_bg);
        } else if (input->placeholder && input->placeholder[0] != '\0') {
            ui_draw_string(tile, text_x, text_y, input->placeholder, FONT_16, input->color_placeholder, input->color_bg);
        }
    }

    ui_draw_reset_clip();
}