#include "ui_numpad.h"
#include "ui_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_numpad_init(ui_numpad_t *np, ui_numpad_cb_t on_confirm) {
    if (!np) return; /*[cite: 23] */

    np->rect = ui_rect_make(50, 30, 380, 260); /*[cite: 23] */
    np->is_open = false; /*[cite: 23] */
    np->title[0] = '\0'; /*[cite: 23] */
    np->buffer[0] = '\0'; /*[cite: 23] */
    np->cursor_pos = 0; /*[cite: 23] */
    np->unit_count = 0; /*[cite: 23] */
    np->selected_unit = -1; /*[cite: 23] */

    np->down_x        = 0;
    np->down_y        = 0;
    np->is_touch_down = false;
    np->is_dragged    = false;

    np->color_bg          = 0x10A2; // 极深蓝黑底[cite: 23]
    np->color_preview_bg  = 0x0842; // 预览框深灰底[cite: 23]
    np->color_key_num_bg  = 0x2126; // 数字按键暗灰底[cite: 23]
    np->color_key_fn_bg   = 0x4208; // 功能按键红棕底[cite: 23]
    np->color_key_unit_bg = 0x18C3; // 单位按键深蓝底[cite: 23]
    np->color_key_ok_bg   = 0x03E0; // 确认按键亮绿底[cite: 23]
    np->color_border      = 0x07FF; // 青蓝边框高亮[cite: 23]

    np->on_confirm = on_confirm; /*[cite: 23] */
    np->user_data  = NULL; /*[cite: 23] */
}

void ui_numpad_open(ui_numpad_t *np, const char *title, const char *init_val,
                    const char **units, uint8_t unit_count, int8_t default_unit_idx, void *user_data) {
    if (!np) return; /*[cite: 23] */

    np->is_open = true; /*[cite: 23] */
    np->is_touch_down = false;
    np->is_dragged    = false;

    strncpy(np->title, title ? title : "数字微调输入", sizeof(np->title) - 1); /*[cite: 23] */
    np->title[sizeof(np->title) - 1] = '\0'; /*[cite: 23] */

    if (init_val) { /*[cite: 23] */
        strncpy(np->buffer, init_val, UI_NUMPAD_BUF_LEN - 1); /*[cite: 23] */
        np->buffer[UI_NUMPAD_BUF_LEN - 1] = '\0'; /*[cite: 23] */
        np->cursor_pos = (uint8_t)strlen(np->buffer); /*[cite: 23] */
    } else {
        np->buffer[0] = '\0'; /*[cite: 23] */
        np->cursor_pos = 0; /*[cite: 23] */
    }

    np->unit_count = (unit_count > UI_NUMPAD_MAX_UNITS) ? UI_NUMPAD_MAX_UNITS : unit_count; /*[cite: 23] */
    for (uint8_t i = 0; i < np->unit_count; i++) { /*[cite: 23] */
        np->units[i] = units ? units[i] : ""; /*[cite: 23] */
    }
    np->selected_unit = (default_unit_idx < (int8_t)np->unit_count) ? default_unit_idx : -1; /*[cite: 23] */
    np->user_data = user_data; /*[cite: 23] */
}

void ui_numpad_close(ui_numpad_t *np) {
    if (!np) return; /*[cite: 23] */
    np->is_open       = false; /*[cite: 23] */
    np->is_touch_down = false;
    np->is_dragged    = false;
}

static void get_key_rect(const ui_numpad_t *np, uint8_t row, uint8_t col, ui_rect_t *out_rect) {
    int16_t start_x = np->rect.x + 8; /*[cite: 23] */
    int16_t start_y = np->rect.y + 76; /*[cite: 23] */
    int16_t key_w   = 68; /*[cite: 23] */
    int16_t key_h   = 38; /*[cite: 23] */
    int16_t gap_x   = 6; /*[cite: 23] */
    int16_t gap_y   = 6; /*[cite: 23] */

    if (col == 3 && row >= 2) { /*[cite: 23] */
        out_rect->x = start_x + 3 * (key_w + gap_x); /*[cite: 23] */
        out_rect->y = start_y + 2 * (key_h + gap_y); /*[cite: 23] */
        out_rect->w = key_w; /*[cite: 23] */
        out_rect->h = key_h * 2 + gap_y; /*[cite: 23] */
        return; /*[cite: 23] */
    }

    out_rect->x = start_x + col * (key_w + gap_x); /*[cite: 23] */
    out_rect->y = start_y + row * (key_h + gap_y); /*[cite: 23] */
    out_rect->w = key_w; /*[cite: 23] */
    out_rect->h = key_h; /*[cite: 23] */
}

static void handle_key_action(ui_numpad_t *np, uint8_t row, uint8_t col) {
    if (col < 3) { /*[cite: 23] */
        static const char k_key_map[4][3] = {
            {'7', '8', '9'},
            {'4', '5', '6'},
            {'1', '2', '3'},
            {'0', '.', '-'}
        };
        char ch = k_key_map[row][col]; /*[cite: 23] */

        if (ch == '-') { /*[cite: 23] */
            if (np->buffer[0] == '-') { /*[cite: 23] */
                memmove(np->buffer, np->buffer + 1, strlen(np->buffer)); /*[cite: 23] */
                if (np->cursor_pos > 0) np->cursor_pos--; /*[cite: 23] */
            } else if (np->cursor_pos < UI_NUMPAD_BUF_LEN - 1) { /*[cite: 23] */
                memmove(np->buffer + 1, np->buffer, strlen(np->buffer) + 1); /*[cite: 23] */
                np->buffer[0] = '-'; /*[cite: 23] */
                np->cursor_pos++; /*[cite: 23] */
            }
        } else if (ch == '.') { /*[cite: 23] */
            if (strchr(np->buffer, '.') == NULL && np->cursor_pos < UI_NUMPAD_BUF_LEN - 1) { /*[cite: 23] */
                np->buffer[np->cursor_pos++] = '.'; /*[cite: 23] */
                np->buffer[np->cursor_pos] = '\0'; /*[cite: 23] */
            }
        } else {
            if (np->cursor_pos < UI_NUMPAD_BUF_LEN - 1) { /*[cite: 23] */
                np->buffer[np->cursor_pos++] = ch; /*[cite: 23] */
                np->buffer[np->cursor_pos] = '\0'; /*[cite: 23] */
            }
        }
    } else if (col == 3) { /*[cite: 23] */
        if (row == 0) { // DEL 退格[cite: 23]
            if (np->cursor_pos > 0) { /*[cite: 23] */
                np->buffer[--np->cursor_pos] = '\0'; /*[cite: 23] */
            }
        } else if (row == 1) { // CLR 清空[cite: 23]
            np->buffer[0] = '\0'; /*[cite: 23] */
            np->cursor_pos = 0; /*[cite: 23] */
        } else { // OK 确认提交[cite: 23]
            double val = strtod(np->buffer, NULL); /*[cite: 23] */
            const char *unit_str = (np->selected_unit >= 0 && np->selected_unit < (int8_t)np->unit_count) ? 
                                    np->units[np->selected_unit] : ""; /*[cite: 23] */
            if (np->on_confirm) { /*[cite: 23] */
                np->on_confirm(np, val, unit_str, np->buffer, np->user_data); /*[cite: 23] */
            }
            ui_numpad_close(np); /*[cite: 23] */
        }
    } else if (col == 4) { /*[cite: 23] */
        if (row < np->unit_count) { /*[cite: 23] */
            np->selected_unit = (int8_t)row; /*[cite: 23] */
        }
    }
}

bool ui_numpad_process_touch(ui_numpad_t *np, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!np || !np->is_open) return false; /*[cite: 23] */

    if (is_touching) {
        if (!np->is_touch_down) {
            // A. 初次按下 (Touch Down): 记录坐标起点
            np->is_touch_down = true;
            np->is_dragged    = false;
            np->down_x        = tx;
            np->down_y        = ty;
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区判定
            int16_t dx = abs(tx - np->down_x);
            int16_t dy = abs(ty - np->down_y);
            if (!np->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                np->is_dragged = true; // 锁定为滑动，放弃按键触发
            }
        }
        return false;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许触发按键输入与关闭的时机
        if (np->is_touch_down) {
            np->is_touch_down = false;

            if (!np->is_dragged) {
                // 1. 点击弹窗外部空白区域 -> 关闭
                if (!ui_rect_contains_point(&np->rect, tx, ty)) {
                    ui_numpad_close(np);
                    return true;
                }

                // 2. 右上角关闭按钮 [X]
                ui_rect_t close_btn = ui_rect_make(np->rect.x + np->rect.w - 36, np->rect.y + 6, 30, 22);
                if (ui_rect_contains_point(&close_btn, tx, ty)) {
                    ui_numpad_close(np);
                    return true;
                }

                // 3. 遍历 4x5 键帽矩阵
                for (uint8_t r = 0; r < 4; r++) {
                    for (uint8_t c = 0; c < 5; c++) {
                        if (c == 3 && r == 3) continue;
                        if (c == 4 && r >= np->unit_count) continue;

                        ui_rect_t kr;
                        get_key_rect(np, r, c, &kr);
                        if (ui_rect_contains_point(&kr, tx, ty)) {
                            handle_key_action(np, r, c);
                            return true;
                        }
                    }
                }
            }
            np->is_dragged = false;
        }
    }

    return false;
}

void ui_numpad_draw(ui_tile_t *tile, const ui_numpad_t *np) {
    if (!tile || !np || !np->is_open) return; /*[cite: 23] */

    ui_rect_t overlap;
    // 1. 切片相交快速裁剪：完全不相交的切片直接 0 开销退出
    if (!ui_rect_intersect(&tile->rect, &np->rect, &overlap)) return; /*[cite: 23] */

    // 2. 绘制弹窗背景与边框
    ui_draw_fill_rect(tile, &overlap, np->color_bg); /*[cite: 23] */
    ui_draw_rect(tile, &np->rect, np->color_border); /*[cite: 23] */

    ui_draw_set_clip(&np->rect); /*[cite: 23] */

    // 3. 绘制标题栏 (带 Y 轴切片重叠守卫)
    int16_t title_y = np->rect.y + 8;
    if ((title_y + 16) > tile->rect.y && title_y < (tile->rect.y + tile->rect.h)) {
        ui_draw_string(tile, np->rect.x + 12, title_y, np->title, FONT_16, 0xFFE0, np->color_bg); /*[cite: 23] */
    }

    // 右上角关闭按钮 [X]
    ui_rect_t close_btn = ui_rect_make(np->rect.x + np->rect.w - 36, np->rect.y + 6, 30, 22); /*[cite: 23] */
    ui_rect_t close_overlap;
    if (ui_rect_intersect(&tile->rect, &close_btn, &close_overlap)) { /*[cite: 23] */
        ui_draw_fill_rect(tile, &close_overlap, 0x4208); /*[cite: 23] */
        ui_draw_rect(tile, &close_btn, 0x8410); /*[cite: 23] */
        ui_draw_string(tile, close_btn.x + 10, close_btn.y + 3, "X", FONT_16, 0xFFFF, 0x4208); /*[cite: 23] */
    }

    // 4. 绘制输入预览看板 (Preview Box)
    ui_rect_t preview_box = ui_rect_make(np->rect.x + 8, np->rect.y + 34, np->rect.w - 16, 34); /*[cite: 23] */
    ui_rect_t box_overlap;
    if (ui_rect_intersect(&tile->rect, &preview_box, &box_overlap)) { /*[cite: 23] */
        ui_draw_fill_rect(tile, &box_overlap, np->color_preview_bg); /*[cite: 23] */
        ui_draw_rect(tile, &preview_box, np->color_border); /*[cite: 23] */

        // 拼接实时字符串与光标
        char disp_str[UI_NUMPAD_BUF_LEN + 4];
        snprintf(disp_str, sizeof(disp_str), "%s_", np->buffer[0] ? np->buffer : "0"); /*[cite: 23] */
        
        int16_t text_y = preview_box.y + 7;
        if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_string(tile, preview_box.x + 8, text_y, disp_str, FONT_20, 0xFFFF, np->color_preview_bg);
        }

        // 选中的单位胶囊 (右对齐)
        if (np->selected_unit >= 0 && np->selected_unit < (int8_t)np->unit_count) { /*[cite: 23] */
            const char *unit_str = np->units[np->selected_unit]; /*[cite: 23] */
            int16_t u_len = (int16_t)strlen(unit_str) * 8 + 12; /*[cite: 23] */
            ui_rect_t u_badge = ui_rect_make(preview_box.x + preview_box.w - u_len - 6, preview_box.y + 6, u_len, 22); /*[cite: 23] */
            ui_rect_t u_ov;
            if (ui_rect_intersect(&tile->rect, &u_badge, &u_ov)) {
                ui_draw_fill_rect(tile, &u_ov, 0x07FF);
                ui_draw_string(tile, u_badge.x + 6, u_badge.y + 3, unit_str, FONT_16, 0x0000, 0x07FF); /*[cite: 23] */
            }
        }
    }

    // 5. 遍历渲染 4x5 键帽矩阵 (算术切片剪枝)
    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t c = 0; c < 5; c++) {
            if (c == 3 && r == 3) continue; // OK 跨行跳过[cite: 23]
            if (c == 4 && r >= np->unit_count) continue; /*[cite: 23] */

            ui_rect_t kr;
            get_key_rect(np, r, c, &kr); /*[cite: 23] */

            ui_rect_t k_overlap;
            if (!ui_rect_intersect(&tile->rect, &kr, &k_overlap)) continue; /*[cite: 23] */

            uint16_t k_bg = np->color_key_num_bg; /*[cite: 23] */
            uint16_t k_text_col = 0xFFFF; /*[cite: 23] */
            char key_str[8] = ""; /*[cite: 23] */

            if (c < 3) { /*[cite: 23] */
                static const char *k_labels[4][3] = {
                    {"7", "8", "9"},
                    {"4", "5", "6"},
                    {"1", "2", "3"},
                    {"0", ".", "+/-"}
                };
                strncpy(key_str, k_labels[r][c], sizeof(key_str) - 1); /*[cite: 23] */
            } else if (c == 3) { /*[cite: 23] */
                if (r == 0) { strncpy(key_str, "DEL", sizeof(key_str)-1); k_bg = np->color_key_fn_bg; k_text_col = 0xF800; } /*[cite: 23] */
                else if (r == 1) { strncpy(key_str, "CLR", sizeof(key_str)-1); k_bg = np->color_key_fn_bg; k_text_col = 0xFFE0; } /*[cite: 23] */
                else { strncpy(key_str, "OK", sizeof(key_str)-1); k_bg = np->color_key_ok_bg; } /*[cite: 23] */
            } else if (c == 4) { /*[cite: 23] */
                strncpy(key_str, np->units[r], sizeof(key_str)-1); /*[cite: 23] */
                bool is_active_u = ((int8_t)r == np->selected_unit); /*[cite: 23] */
                k_bg = is_active_u ? 0x07FF : np->color_key_unit_bg; /*[cite: 23] */
                k_text_col = is_active_u ? 0x0000 : 0x07FF; /*[cite: 23] */
            }

            ui_draw_fill_rect(tile, &k_overlap, k_bg); /*[cite: 23] */
            ui_draw_rect(tile, &kr, 0x39E7); /*[cite: 23] */

            // 键帽文字居中
            int16_t str_w = (int16_t)strlen(key_str) * 8; /*[cite: 23] */
            int16_t tx = kr.x + (kr.w - str_w) / 2; /*[cite: 23] */
            int16_t ty = kr.y + (kr.h - 16) / 2; /*[cite: 23] */

            if ((ty + 16) > tile->rect.y && ty < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, tx, ty, key_str, FONT_16, k_text_col, k_bg); /*[cite: 23] */
            }
        }
    }

    ui_draw_reset_clip(); /*[cite: 23] */
}