#include "ui_terminal.h"
#include "ui_draw.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_terminal_init(ui_terminal_t *term, int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!term) return;

    term->rect = ui_rect_make(x, y, w, h);
    term->mode = UI_TERM_MODE_ASCII;

    term->head  = 0;
    term->count = 0;
    memset(term->lines, 0, sizeof(term->lines));

    term->line_height = 18;
    term->scroll_y    = 0;
    term->auto_scroll = true;

    term->down_x         = 0;
    term->down_y         = 0;
    term->scroll_start_y = 0;
    term->is_touch_down  = false;
    term->is_dragged     = false;
    term->is_scrolling   = false;

    term->color_bg        = 0x0842; // 极深灰黑底
    term->color_border    = 0x39E7; // 分割边框
    term->color_text_def  = 0xFFFF; // 默认白字
    term->color_scrollbar = 0x07FF; // 青蓝滚动指示条
}

static void push_single_line(ui_terminal_t *term, const char *str, uint16_t color) {
    if (!term || !str) return;

    strncpy(term->lines[term->head], str, UI_TERM_LINE_LEN - 1);
    term->lines[term->head][UI_TERM_LINE_LEN - 1] = '\0';
    term->line_colors[term->head] = color;

    term->head = (term->head + 1) % UI_TERM_MAX_LINES;
    if (term->count < UI_TERM_MAX_LINES) {
        term->count++;
    }

    // 自动跟随吸附到底端
    if (term->auto_scroll) {
        int16_t total_h = term->count * term->line_height;
        int16_t max_scroll = total_h - (term->rect.h - 8);
        term->scroll_y = (max_scroll > 0) ? max_scroll : 0;
    }
}

void ui_terminal_write_line(ui_terminal_t *term, const char *str, uint16_t color) {
    if (!term || !str) return;

    char buf[UI_TERM_LINE_LEN];
    uint16_t b_idx = 0;

    while (*str) {
        if (*str == '\r') {
            str++;
            continue;
        }
        if (*str == '\n') {
            buf[b_idx] = '\0';
            push_single_line(term, buf, color);
            b_idx = 0;
            str++;
            continue;
        }

        buf[b_idx++] = *str++;
        if (b_idx >= UI_TERM_LINE_LEN - 1) {
            buf[b_idx] = '\0';
            push_single_line(term, buf, color);
            b_idx = 0;
        }
    }

    if (b_idx > 0) {
        buf[b_idx] = '\0';
        push_single_line(term, buf, color);
    }
}

void ui_terminal_printf(ui_terminal_t *term, uint16_t color, const char *fmt, ...) {
    if (!term || !fmt) return;
    char temp[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);
    ui_terminal_write_line(term, temp, color);
}

void ui_terminal_write_hex(ui_terminal_t *term, const uint8_t *data, size_t len, uint16_t color) {
    if (!term || !data || len == 0) return;

    char line_buf[UI_TERM_LINE_LEN];
    for (size_t i = 0; i < len; i += 8) {
        int offset = snprintf(line_buf, sizeof(line_buf), "[%04X] ", (unsigned int)i);
        for (size_t j = 0; j < 8; j++) {
            if (i + j < len) {
                offset += snprintf(line_buf + offset, sizeof(line_buf) - offset, "%02X ", data[i + j]);
            } else {
                offset += snprintf(line_buf + offset, sizeof(line_buf) - offset, "   ");
            }
        }
        push_single_line(term, line_buf, color);
    }
}

void ui_terminal_clear(ui_terminal_t *term) {
    if (!term) return;
    term->head        = 0;
    term->count       = 0;
    term->scroll_y    = 0;
    term->auto_scroll = true;
}

void ui_terminal_set_autoscroll(ui_terminal_t *term, bool enable) {
    if (term) term->auto_scroll = enable;
}

void ui_terminal_set_mode(ui_terminal_t *term, ui_terminal_mode_t mode) {
    if (term) term->mode = mode;
}

/* ================= 3. 触控状态机 (Touch Slop + Touch Capture) ================= */

bool ui_terminal_process_touch(ui_terminal_t *term, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!term || term->count == 0) return false;

    int16_t total_h = term->count * term->line_height;
    int16_t max_scroll = total_h - (term->rect.h - 8);
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;

    if (is_touching) {
        if (!term->is_touch_down) {
            // A. 初次按下 (Touch Down): 命中视口
            if (ui_rect_contains_point(&term->rect, tx, ty)) {
                term->is_touch_down  = true;
                term->is_dragged     = false;
                term->is_scrolling   = false;
                term->down_x         = tx;
                term->down_y         = ty;
                term->scroll_start_y = term->scroll_y;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与 Touch Capture 追踪
            int16_t dy = ty - term->down_y;
            int16_t dx = tx - term->down_x;

            if (!term->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                term->is_dragged   = true;
                term->is_scrolling = true;
                term->auto_scroll  = false; // 向上拖动查看历史时暂停吸底
            }

            if (term->is_dragged && max_scroll > 0) {
                int16_t new_scroll = term->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (term->scroll_y != new_scroll) {
                    term->scroll_y = new_scroll;
                    term->auto_scroll = (term->scroll_y >= max_scroll); // 滑回底部恢复吸底
                    need_redraw = true;
                }
            }
        }
    } else {
        // C. 手指松开 (Touch Up): 释放手势锁
        if (term->is_touch_down) {
            term->is_touch_down = false;
            term->is_dragged    = false;
            if (term->is_scrolling) {
                term->is_scrolling = false;
                need_redraw = true;
            }
        }
    }

    return need_redraw;
}

/* ================= 4. 切片光栅化渲染 (O(1) 索引剪枝) ================= */

void ui_terminal_draw(ui_tile_t *tile, const ui_terminal_t *term) {
    if (!tile || !term) return;

    ui_rect_t overlap;
    // 1. 切片相交快速裁剪
    if (!ui_rect_intersect(&tile->rect, &term->rect, &overlap)) {
        return;
    }

    // 2. 擦除背景与绘制边框
    ui_draw_fill_rect(tile, &overlap, term->color_bg);
    ui_draw_rect(tile, &term->rect, term->color_border);

    // 3. 锁定像素级视口裁剪保护
    ui_draw_set_clip(&term->rect);

    int16_t base_y = term->rect.y + 4 - term->scroll_y;
    int16_t text_x = term->rect.x + 6;

    // ⚡ 核心优化：仅计算与当前切片相交的行索引范围 (从 O(N) 降至 O(1))
    int16_t first_idx = (tile->rect.y - base_y) / term->line_height;
    int16_t last_idx  = (tile->rect.y + tile->rect.h - 1 - base_y) / term->line_height;

    if (first_idx < 0) first_idx = 0;
    if (last_idx >= (int16_t)term->count) last_idx = (int16_t)term->count - 1;

    uint16_t start_idx = (term->count < UI_TERM_MAX_LINES) ? 0 : term->head;

    for (int16_t i = first_idx; i <= last_idx; i++) {
        int16_t line_y = base_y + i * term->line_height;

        if ((line_y + 16) > tile->rect.y && line_y < (tile->rect.y + tile->rect.h)) {
            uint16_t ring_idx = (start_idx + i) % UI_TERM_MAX_LINES;
            const char *line_str = term->lines[ring_idx];
            uint16_t color = term->line_colors[ring_idx];

            ui_draw_string(tile, text_x, line_y, line_str, FONT_16, color, term->color_bg);
        }
    }

    // 4. 绘制右侧微型滚动条
    int16_t total_h = term->count * term->line_height;
    if (total_h > (term->rect.h - 8)) {
        int16_t bar_w = 2;
        int16_t track_h = term->rect.h - 8;
        int16_t thumb_h = (int16_t)(((int32_t)(term->rect.h - 8) * track_h) / total_h);
        if (thumb_h < 10) thumb_h = 10;

        int16_t max_scroll = total_h - (term->rect.h - 8);
        int16_t thumb_y = term->rect.y + 4;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)term->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x = term->rect.x + term->rect.w - bar_w - 3;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, term->color_scrollbar);
        }
    }

    ui_draw_reset_clip();
}