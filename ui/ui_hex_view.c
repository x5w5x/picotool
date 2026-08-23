#include "ui_hex_view.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

// 紧凑排版常数定义
#define ADDR_COL_W      76  // 地址列宽度 "08000000:" (72px + 4px)
#define HEX_BYTE_W      22  // 每个 Hex 单元宽度 (2 字符 16px + 6px 间距)
#define HEX_COL_W       (UI_HEX_BYTES_PER_ROW * HEX_BYTE_W) // 8 * 22 = 176px
#define ASCII_COL_W     (UI_HEX_BYTES_PER_ROW * 8)          // 8 * 8 = 64px

void ui_hex_view_init(ui_hex_view_t *hv, int16_t x, int16_t y, int16_t w, int16_t h,
                      ui_hex_select_cb_t on_select) {
    if (!hv) return;

    hv->rect              = ui_rect_make(x, y, w, h);
    hv->data              = NULL;
    hv->data_len          = 0;
    hv->base_addr         = 0;

    hv->row_h             = 20;
    hv->scroll_y          = 0;
    hv->selected_byte_idx = -1;

    hv->down_x            = 0;
    hv->down_y            = 0;
    hv->scroll_start_y    = 0;
    hv->is_touch_down     = false;
    hv->is_dragged        = false;
    hv->is_scrolling      = false;

    hv->color_bg            = 0x0842; // 极深蓝黑底
    hv->color_addr          = 0xFD20; // 橙黄基地址
    hv->color_hex           = 0xFFFF; // 纯白 Hex 数据
    hv->color_ascii         = 0x07FF; // 青蓝 ASCII 字符
    hv->color_selected_bg   = 0x07E0; // 亮绿选中底色
    hv->color_selected_text = 0x0000; // 纯黑选中字
    hv->color_border        = 0x39E7; // 深灰外框
    hv->color_scrollbar     = 0x07FF; // 青蓝滚动指示条

    hv->on_select         = on_select;
    hv->user_data         = NULL;
}

void ui_hex_view_bind(ui_hex_view_t *hv, const uint8_t *data, uint32_t data_len, uint32_t base_addr) {
    if (!hv) return;
    hv->data              = data;
    hv->data_len          = data_len;
    hv->base_addr         = base_addr;
    hv->scroll_y          = 0;
    hv->selected_byte_idx = -1;
}

void ui_hex_view_jump_to_addr(ui_hex_view_t *hv, uint32_t target_addr) {
    if (!hv || !hv->data || hv->data_len == 0) return;

    uint32_t offset = (target_addr >= hv->base_addr) ? (target_addr - hv->base_addr) : 0;
    if (offset >= hv->data_len) offset = hv->data_len - 1;

    uint32_t target_row = offset / UI_HEX_BYTES_PER_ROW;
    int16_t new_scroll = (int16_t)(target_row * hv->row_h);

    uint32_t total_rows = (hv->data_len + UI_HEX_BYTES_PER_ROW - 1) / UI_HEX_BYTES_PER_ROW;
    int16_t max_scroll = (int16_t)(total_rows * hv->row_h) - hv->rect.h;
    if (max_scroll < 0) max_scroll = 0;

    if (new_scroll > max_scroll) new_scroll = max_scroll;
    hv->scroll_y = new_scroll;
}

/* ================= 触控状态机 (8px 防抖 + 精准点选) ================= */

bool ui_hex_view_process_touch(ui_hex_view_t *hv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!hv || !hv->data || hv->data_len == 0) return false;

    uint32_t total_rows = (hv->data_len + UI_HEX_BYTES_PER_ROW - 1) / UI_HEX_BYTES_PER_ROW;
    int16_t total_h = (int16_t)(total_rows * hv->row_h);
    int16_t max_scroll = total_h - hv->rect.h;
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;

    if (is_touching) {
        if (!hv->is_touch_down) {
            if (ui_rect_contains_point(&hv->rect, tx, ty)) {
                hv->is_touch_down  = true;
                hv->is_dragged     = false;
                hv->is_scrolling   = false;
                hv->down_x         = tx;
                hv->down_y         = ty;
                hv->scroll_start_y = hv->scroll_y;
            }
        } else {
            int16_t dy = ty - hv->down_y;
            int16_t dx = tx - hv->down_x;

            if (!hv->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                hv->is_dragged   = true;
                hv->is_scrolling = true;
            }

            if (hv->is_dragged && max_scroll > 0) {
                int16_t new_scroll = hv->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (hv->scroll_y != new_scroll) {
                    hv->scroll_y = new_scroll;
                    need_redraw  = true;
                }
            }
        }
        return need_redraw;
    } else {
        if (hv->is_touch_down) {
            hv->is_touch_down = false;

            if (!hv->is_dragged && ui_rect_contains_point(&hv->rect, tx, ty)) {
                int16_t rel_y = ty - (hv->rect.y - hv->scroll_y);
                uint32_t row = (uint32_t)(rel_y / hv->row_h);

                int16_t hex_start_x   = hv->rect.x + ADDR_COL_W + 4;
                int16_t ascii_start_x = hex_start_x + HEX_COL_W + 8;

                int8_t col = -1;
                if (tx >= hex_start_x && tx < (hex_start_x + HEX_COL_W)) {
                    col = (int8_t)((tx - hex_start_x) / HEX_BYTE_W);
                } else if (tx >= ascii_start_x && tx < (ascii_start_x + ASCII_COL_W)) {
                    col = (int8_t)((tx - ascii_start_x) / 8);
                }

                if (col >= 0 && col < UI_HEX_BYTES_PER_ROW) {
                    uint32_t byte_idx = row * UI_HEX_BYTES_PER_ROW + col;
                    if (byte_idx < hv->data_len) {
                        hv->selected_byte_idx = (int32_t)byte_idx;
                        if (hv->on_select) {
                            hv->on_select(hv, byte_idx, hv->data[byte_idx], hv->user_data);
                        }
                        need_redraw = true;
                    }
                }
            }

            hv->is_dragged   = false;
            hv->is_scrolling = false;
        }
    }

    return need_redraw;
}

/* ================= 切片光栅化渲染 (零截断精确排版) ================= */

void ui_hex_view_draw(ui_tile_t *tile, const ui_hex_view_t *hv) {
    if (!tile || !hv || !hv->data || hv->data_len == 0) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &hv->rect, &overlap)) {
        return;
    }

    // 1. 擦除背景与绘制边框
    ui_draw_fill_rect(tile, &overlap, hv->color_bg);
    ui_draw_rect(tile, &hv->rect, hv->color_border);

    // 2. 启用像素级局部视口裁剪保护
    ui_draw_set_clip(&hv->rect);

    int16_t base_y = hv->rect.y + 4 - hv->scroll_y;
    uint32_t total_rows = (hv->data_len + UI_HEX_BYTES_PER_ROW - 1) / UI_HEX_BYTES_PER_ROW;

    int16_t first_row = (tile->rect.y - base_y) / hv->row_h;
    int16_t last_row  = (tile->rect.y + tile->rect.h - 1 - base_y) / hv->row_h;

    if (first_row < 0) first_row = 0;
    if (last_row >= (int16_t)total_rows) last_row = (int16_t)total_rows - 1;

    int16_t hex_start_x   = hv->rect.x + ADDR_COL_W + 4;
    int16_t ascii_start_x = hex_start_x + HEX_COL_W + 8;

    for (int16_t r = first_row; r <= last_row; r++) {
        int16_t row_y = base_y + r * hv->row_h;
        uint32_t row_byte_start = (uint32_t)r * UI_HEX_BYTES_PER_ROW;

        if ((row_y + 16) > tile->rect.y && row_y < (tile->rect.y + tile->rect.h)) {
            // A. 绘制地址列 "08000000:" (宽 72px)
            char addr_str[16];
            snprintf(addr_str, sizeof(addr_str), "%08lX:", (unsigned long)(hv->base_addr + row_byte_start));
            ui_draw_string(tile, hv->rect.x + 4, row_y, addr_str, FONT_16, hv->color_addr, hv->color_bg);

            // B. 绘制 8 个 Hex 字节与 ASCII 对照
            for (uint8_t c = 0; c < UI_HEX_BYTES_PER_ROW; c++) {
                uint32_t byte_idx = row_byte_start + c;
                if (byte_idx >= hv->data_len) break;

                uint8_t b_val = hv->data[byte_idx];
                bool is_selected = (hv->selected_byte_idx == (int32_t)byte_idx);

                int16_t bx = hex_start_x + c * HEX_BYTE_W;
                int16_t ax = ascii_start_x + c * 8;

                // 1) 绘制 Hex 字符 (如 "5A")
                char hex_str[4];
                snprintf(hex_str, sizeof(hex_str), "%02X", b_val);

                if (is_selected) {
                    ui_rect_t sel_box = ui_rect_make((int16_t)(bx - 1), (int16_t)(row_y - 1), 18, 18);
                    ui_rect_t sel_ov;
                    if (ui_rect_intersect(&tile->rect, &sel_box, &sel_ov)) {
                        ui_draw_fill_rect(tile, &sel_ov, hv->color_selected_bg);
                    }
                    ui_draw_string(tile, bx, row_y, hex_str, FONT_16, hv->color_selected_text, hv->color_selected_bg);
                } else {
                    ui_draw_string(tile, bx, row_y, hex_str, FONT_16, hv->color_hex, hv->color_bg);
                }

                // 2) 绘制 ASCII 对照字符 (8 字节连续无缝排列)
                char ch = (b_val >= 32 && b_val <= 126) ? (char)b_val : '.';
                char ch_str[2] = {ch, '\0'};

                if (is_selected) {
                    ui_draw_string(tile, ax, row_y, ch_str, FONT_16, hv->color_selected_bg, hv->color_bg);
                } else {
                    ui_draw_string(tile, ax, row_y, ch_str, FONT_16, hv->color_ascii, hv->color_bg);
                }
            }
        }
    }

    // 4. 超出视口时绘制右侧细滚动条
    int16_t total_h = (int16_t)(total_rows * hv->row_h);
    if (total_h > (hv->rect.h - 8)) {
        int16_t bar_w = 2;
        int16_t track_h = hv->rect.h - 8;
        int16_t thumb_h = (int16_t)(((int32_t)(hv->rect.h - 8) * track_h) / total_h);
        if (thumb_h < 12) thumb_h = 12;

        int16_t max_scroll = total_h - hv->rect.h;
        int16_t thumb_y = hv->rect.y + 4;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)hv->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x = hv->rect.x + hv->rect.w - bar_w - 2;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, hv->color_scrollbar);
        }
    }

    ui_draw_reset_clip();
}