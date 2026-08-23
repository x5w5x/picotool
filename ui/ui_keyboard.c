#include "ui_keyboard.h"
#include "ui_draw.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

// 4 行按键定义表
static const char *k_keys_lower[4][10] = {
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
    {"CAPS", "z", "x", "c", "v", "b", "n", "m", "DEL", NULL},
    {"123", "SPACE", "ENTER", NULL}
};

static const char *k_keys_upper[4][10] = {
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", "_"},
    {"caps", "Z", "X", "C", "V", "B", "N", "M", "DEL", NULL},
    {"123", "SPACE", "ENTER", NULL}
};

static const char *k_keys_num_sym[4][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"+", "-", "*", "/", "=", ":", ";", "(", ")", "#"},
    {"abc", "@", ".", ",", "\"", "'", "?", "!", "DEL", NULL},
    {"ABC", "SPACE", "ENTER", NULL}
};

void ui_keyboard_init(ui_keyboard_t *kb, ui_kb_submit_cb_t on_submit) {
    if (!kb) return;
    kb->rect = ui_rect_make(0, 120, 480, 200);
    kb->is_open = false;
    kb->mode = UI_KB_MODE_UPPER;
    kb->buffer[0] = '\0';
    kb->cursor_pos = 0;
    kb->target_input = NULL;

    kb->down_x        = 0;
    kb->down_y        = 0;
    kb->is_touch_down = false;
    kb->is_dragged    = false;

    kb->on_submit = on_submit;
    kb->user_data = NULL;
}

void ui_keyboard_open(ui_keyboard_t *kb, ui_input_t *target_input) {
    if (!kb) return;
    kb->is_open = true;
    kb->target_input = target_input;
    kb->is_touch_down = false;
    kb->is_dragged    = false;

    if (target_input) {
        strncpy(kb->buffer, target_input->text, UI_INPUT_MAX_LEN - 1);
        kb->buffer[UI_INPUT_MAX_LEN - 1] = '\0';
        kb->cursor_pos = (uint8_t)strlen(kb->buffer);
    } else {
        kb->buffer[0] = '\0';
        kb->cursor_pos = 0;
    }
}

void ui_keyboard_close(ui_keyboard_t *kb) {
    if (!kb) return;
    kb->is_open = false;
    kb->is_touch_down = false;
    kb->is_dragged    = false;
    if (kb->target_input) {
        kb->target_input->is_focused = false;
    }
}

static void get_key_rect(uint8_t row, uint8_t col, int16_t kb_y, ui_rect_t *out_rect) {
    int16_t key_h = 36;
    int16_t gap = 4;
    int16_t py = kb_y + 36 + row * (key_h + gap);

    if (row == 0 || row == 1) {
        int16_t key_w = 44;
        out_rect->x = 4 + col * (key_w + gap);
        out_rect->y = py;
        out_rect->w = key_w;
        out_rect->h = key_h;
    } else if (row == 2) {
        if (col == 0) {
            *out_rect = ui_rect_make(4, py, 60, key_h);
        } else if (col == 8) {
            *out_rect = ui_rect_make(416, py, 60, key_h);
        } else {
            *out_rect = ui_rect_make(68 + (col - 1) * 49, py, 45, key_h);
        }
    } else if (row == 3) {
        if (col == 0) {
            *out_rect = ui_rect_make(4, py, 80, key_h);
        } else if (col == 1) {
            *out_rect = ui_rect_make(88, py, 258, key_h);
        } else if (col == 2) {
            *out_rect = ui_rect_make(350, py, 126, key_h);
        }
    }
}

static void handle_key_action(ui_keyboard_t *kb, const char *key_str) {
    if (!key_str) return;

    if (strcmp(key_str, "CAPS") == 0) {
        kb->mode = UI_KB_MODE_UPPER;
    } else if (strcmp(key_str, "caps") == 0 || strcmp(key_str, "abc") == 0) {
        kb->mode = UI_KB_MODE_LOWER;
    } else if (strcmp(key_str, "123") == 0) {
        kb->mode = UI_KB_MODE_NUM_SYM;
    } else if (strcmp(key_str, "ABC") == 0) {
        kb->mode = UI_KB_MODE_UPPER;
    } else if (strcmp(key_str, "DEL") == 0) {
        if (kb->cursor_pos > 0) {
            kb->buffer[--kb->cursor_pos] = '\0';
        }
    } else if (strcmp(key_str, "SPACE") == 0) {
        if (kb->cursor_pos < UI_INPUT_MAX_LEN - 1) {
            kb->buffer[kb->cursor_pos++] = ' ';
            kb->buffer[kb->cursor_pos] = '\0';
        }
    } else if (strcmp(key_str, "ENTER") == 0) {
        if (kb->target_input) {
            ui_input_set_text(kb->target_input, kb->buffer);
        }
        if (kb->on_submit) {
            kb->on_submit(kb, kb->buffer, kb->user_data);
        }
        ui_keyboard_close(kb);
        return;
    } else {
        if (kb->cursor_pos < UI_INPUT_MAX_LEN - 1) {
            kb->buffer[kb->cursor_pos++] = key_str[0];
            kb->buffer[kb->cursor_pos] = '\0';
        }
    }

    if (kb->target_input) {
        ui_input_set_text(kb->target_input, kb->buffer);
    }
}

bool ui_keyboard_process_touch(ui_keyboard_t *kb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!kb || !kb->is_open) return false;

    if (is_touching) {
        if (!kb->is_touch_down) {
            // A. 初次按下 (Touch Down): 记录坐标起点
            kb->is_touch_down = true;
            kb->is_dragged    = false;
            kb->down_x        = tx;
            kb->down_y        = ty;
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区判定
            int16_t dx = abs(tx - kb->down_x);
            int16_t dy = abs(ty - kb->down_y);
            if (!kb->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                kb->is_dragged = true; // 锁定为滑动，放弃按键触发
            }
        }
        return false;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许输入字符或关闭键盘的时机
        if (kb->is_touch_down) {
            kb->is_touch_down = false;

            if (!kb->is_dragged) {
                // 1. 点击键盘上方空白区域 -> 收起键盘
                if (!ui_rect_contains_point(&kb->rect, tx, ty)) {
                    ui_keyboard_close(kb);
                    return true;
                }

                // 2. 点击顶部清空按钮 (CLR)
                ui_rect_t clr_rect = ui_rect_make(416, kb->rect.y + 4, 60, 28);
                if (ui_rect_contains_point(&clr_rect, tx, ty)) {
                    kb->buffer[0] = '\0';
                    kb->cursor_pos = 0;
                    if (kb->target_input) {
                        ui_input_clear(kb->target_input);
                    }
                    return true;
                }

                // 3. 遍历键帽判定命中
                const char *(*keys)[10] = (kb->mode == UI_KB_MODE_LOWER) ? k_keys_lower :
                                          ((kb->mode == UI_KB_MODE_UPPER) ? k_keys_upper : k_keys_num_sym);

                for (uint8_t r = 0; r < 4; r++) {
                    for (uint8_t c = 0; c < 10; c++) {
                        const char *k = keys[r][c];
                        if (!k) break;

                        ui_rect_t kr;
                        get_key_rect(r, c, kb->rect.y, &kr);
                        if (ui_rect_contains_point(&kr, tx, ty)) {
                            handle_key_action(kb, k);
                            return true;
                        }
                    }
                }
            }
            kb->is_dragged = false;
        }
    }

    return false;
}
void ui_keyboard_draw(ui_tile_t *tile, const ui_keyboard_t *kb) {
    if (!tile || !kb || !kb->is_open) return;//[cite: 8]

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &kb->rect, &overlap)) return;//[cite: 8]

    // 1. 键盘整体大底色（纯白 0xFFFF）与顶部浅灰分割线 (0xC618)
    ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    ui_draw_fast_hline(tile, kb->rect.x, kb->rect.y, kb->rect.w, 0xC618);

    // 2. 顶部实时预览输入条 (Preview Bar) - 极浅灰底 (0xF7BE) + 纯黑文字 (0x0000)
    ui_rect_t preview_box = ui_rect_make(4, kb->rect.y + 4, 408, 28);//[cite: 8]
    ui_rect_t box_overlap;
    if (ui_rect_intersect(&tile->rect, &preview_box, &box_overlap)) {//[cite: 8]
        ui_draw_fill_rect(tile, &box_overlap, 0xF7BE);
        ui_draw_rect(tile, &preview_box, 0xC618);

        char show_buf[UI_INPUT_MAX_LEN + 4];//[cite: 8]
        if (kb->buffer[0] != '\0') {//[cite: 8]
            snprintf(show_buf, sizeof(show_buf), "%s_", kb->buffer);//[cite: 8]
        } else {
            snprintf(show_buf, sizeof(show_buf), "_");//[cite: 8]
        }

        int16_t str_len = (int16_t)strlen(show_buf);//[cite: 8]
        int16_t max_chars = (preview_box.w - 16) / 8;//[cite: 8]
        const char *draw_str = show_buf;//[cite: 8]
        if (str_len > max_chars) {//[cite: 8]
            draw_str = show_buf + (str_len - max_chars);//[cite: 8]
        }

        ui_draw_set_clip(&preview_box);//[cite: 8]
        ui_draw_string(tile, preview_box.x + 8, preview_box.y + 6, draw_str, FONT_16, 0x0000, 0xF7BE);
        ui_draw_reset_clip();//[cite: 8]
    }

    // 3. 顶部清空按钮 (CLR) - 浅灰底 (0xE73D) + 深灰/红色字
    ui_rect_t clr_rect = ui_rect_make(416, kb->rect.y + 4, 60, 28);//[cite: 8]
    ui_rect_t clr_overlap;
    if (ui_rect_intersect(&tile->rect, &clr_rect, &clr_overlap)) {//[cite: 8]
        ui_draw_fill_rect(tile, &clr_overlap, 0xE73D);
        ui_draw_rect(tile, &clr_rect, 0xC618);
        ui_draw_string(tile, clr_rect.x + 14, clr_rect.y + 6, "清空", FONT_16, 0x0000, 0xE73D);
    }

    // 4. 遍历绘制所有按键
    const char *(*keys)[10] = (kb->mode == UI_KB_MODE_LOWER) ? k_keys_lower :
                              ((kb->mode == UI_KB_MODE_UPPER) ? k_keys_upper : k_keys_num_sym);//[cite: 8]

    for (uint8_t r = 0; r < 4; r++) {//[cite: 8]
        for (uint8_t c = 0; c < 10; c++) {//[cite: 8]
            const char *k = keys[r][c];//[cite: 8]
            if (!k) break;//[cite: 8]

            ui_rect_t kr;
            get_key_rect(r, c, kb->rect.y, &kr);//[cite: 8]

            ui_rect_t k_overlap;
            if (!ui_rect_intersect(&tile->rect, &kr, &k_overlap)) continue;//[cite: 8]

            // 浅色主题按键配色：普通按键为立体白/浅灰，文字为纯黑
            uint16_t key_bg = 0xE73D;       // 常规字母按键背景 (高级浅灰)
            uint16_t key_text_col = 0x0000; // 纯黑文字

            if (strcmp(k, "ENTER") == 0) {//[cite: 8]
                key_bg = 0x03DF;            // 科技纯蓝回车键
                key_text_col = 0xFFFF;      // 白字
            } else if (strcmp(k, "DEL") == 0) {//[cite: 8]
                key_bg = 0xD6BB;            // 功能浅灰底
                key_text_col = 0xF800;      // 红字
            } else if (strcmp(k, "CAPS") == 0 || strcmp(k, "caps") == 0 ||
                       strcmp(k, "123") == 0  || strcmp(k, "ABC") == 0 || strcmp(k, "abc") == 0) {//[cite: 8]
                key_bg = 0xD6BB;            // 功能切换键背景
                key_text_col = 0x03DF;      // 蓝字高亮
            }

            ui_draw_fill_rect(tile, &k_overlap, key_bg);//[cite: 8]
            ui_draw_rect(tile, &kr, 0xC618);             // 浅灰边框

            int16_t str_w = (int16_t)strlen(k) * 8;//[cite: 8]
            int16_t tx = kr.x + (kr.w - str_w) / 2;//[cite: 8]
            int16_t ty = kr.y + (kr.h - 16) / 2;//[cite: 8]

            if ((ty + 16) > tile->rect.y && ty < (tile->rect.y + tile->rect.h)) {//[cite: 8]
                ui_draw_string(tile, tx, ty, k, FONT_16, key_text_col, key_bg);//[cite: 8]
            }
        }
    }
}