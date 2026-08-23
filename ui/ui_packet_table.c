#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui_packet_table.h"
#include "ui_draw.h"

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_packet_table_init(ui_packet_table_t *tbl, int16_t x, int16_t y, int16_t w, int16_t h, ui_pkt_select_cb_t on_select) {
    if (!tbl) return;

    tbl->rect = ui_rect_make(x, y, w, h);
    tbl->head         = 0;
    tbl->count        = 0;
    tbl->selected_idx = -1;
    tbl->header_h     = 22;
    tbl->row_h        = 24;
    tbl->scroll_y     = 0;
    tbl->auto_scroll  = true;

    tbl->down_x         = 0;
    tbl->down_y         = 0;
    tbl->scroll_start_y = 0;
    tbl->is_touch_down  = false;
    tbl->is_dragged     = false;
    tbl->is_scrolling   = false;

    tbl->color_bg          = 0x0842; // 极深灰底
    tbl->color_header_bg   = 0x10A2; // 表头深蓝黑底
    tbl->color_row_even    = 0x1082; // 偶数行底色
    tbl->color_row_odd     = 0x18C3; // 奇数行稍亮底色
    tbl->color_selected_bg = 0x2146; // 选中行暗青蓝高亮
    tbl->color_border      = 0x39E7; // 分割线颜色
    tbl->color_scrollbar   = 0x07FF; // 青蓝指示滑块

    tbl->on_select  = on_select;
    tbl->user_data  = NULL;
}

void ui_packet_table_clear(ui_packet_table_t *tbl) {
    if (!tbl) return;
    tbl->head         = 0;
    tbl->count        = 0;
    tbl->selected_idx = -1;
    tbl->scroll_y     = 0;
    tbl->auto_scroll  = true;
}

void ui_packet_table_push(ui_packet_table_t *tbl, uint32_t time_us, uint16_t addr, bool is_read, 
                          const uint8_t *data, uint8_t len, ui_pkt_status_t status) {
    if (!tbl) return;

    ui_pkt_row_t *row = &tbl->rows[tbl->head];
    row->time_us  = time_us;
    row->addr     = addr;
    row->is_read  = is_read;
    row->status   = status;
    row->data_len = (len > UI_PKT_DATA_MAX) ? UI_PKT_DATA_MAX : len;

    if (data && row->data_len > 0) {
        memcpy(row->data, data, row->data_len);
    }

    tbl->head = (tbl->head + 1) % UI_PKT_MAX_ROWS;
    if (tbl->count < UI_PKT_MAX_ROWS) {
        tbl->count++;
    }

    // 自动跟随吸底追踪最新报文
    if (tbl->auto_scroll) {
        int16_t body_h = tbl->rect.h - tbl->header_h;
        int16_t total_h = (int16_t)tbl->count * tbl->row_h;
        int16_t max_scroll = total_h - body_h;
        tbl->scroll_y = (max_scroll > 0) ? max_scroll : 0;
    }
}

const ui_pkt_row_t *ui_packet_table_get_selected(const ui_packet_table_t *tbl) {
    if (!tbl || tbl->selected_idx < 0 || tbl->selected_idx >= (int16_t)tbl->count) return NULL;

    uint16_t start_idx = (tbl->count < UI_PKT_MAX_ROWS) ? 0 : tbl->head;
    uint16_t ring_idx = (start_idx + tbl->selected_idx) % UI_PKT_MAX_ROWS;
    return &tbl->rows[ring_idx];
}

/* ================= 触控状态机 (Touch Slop + Touch-Up 触发) ================= */

bool ui_packet_table_process_touch(ui_packet_table_t *tbl, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!tbl || tbl->count == 0) return false;

    ui_rect_t body_rect = ui_rect_make(tbl->rect.x, (int16_t)(tbl->rect.y + tbl->header_h), tbl->rect.w, (int16_t)(tbl->rect.h - tbl->header_h));

    int16_t total_h = (int16_t)tbl->count * tbl->row_h;
    int16_t max_scroll = total_h - body_rect.h;
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;

    if (is_touching) {
        if (!tbl->is_touch_down) {
            // A. 初次按下 (Touch Down): 仅在表体区域记录起点
            if (ui_rect_contains_point(&body_rect, tx, ty)) {
                tbl->is_touch_down  = true;
                tbl->is_dragged     = false;
                tbl->is_scrolling   = false;
                tbl->down_x         = tx;
                tbl->down_y         = ty;
                tbl->scroll_start_y = tbl->scroll_y;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与 Touch Capture 全局追踪
            int16_t dy = ty - tbl->down_y;
            int16_t dx = tx - tbl->down_x;

            if (!tbl->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                tbl->is_dragged   = true;
                tbl->is_scrolling = true;
                tbl->auto_scroll  = false; // 手动滑动时自动解除吸底
            }

            if (tbl->is_dragged && max_scroll > 0) {
                int16_t new_scroll = tbl->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (tbl->scroll_y != new_scroll) {
                    tbl->scroll_y = new_scroll;
                    // 滑动到最底部时重新恢复吸底
                    tbl->auto_scroll = (tbl->scroll_y >= max_scroll);
                    need_redraw = true;
                }
            }
        }
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许触发条目选中的时机
        if (tbl->is_touch_down) {
            tbl->is_touch_down = false;

            // 只有从未发生滑动的纯点按才判定点击行
            if (!tbl->is_dragged && ui_rect_contains_point(&body_rect, tx, ty)) {
                int16_t rel_y = ty - (body_rect.y - tbl->scroll_y);
                if (rel_y >= 0 && rel_y < total_h) {
                    int16_t clicked_idx = rel_y / tbl->row_h;
                    if (clicked_idx < (int16_t)tbl->count) {
                        tbl->selected_idx = clicked_idx;
                        if (tbl->on_select) {
                            tbl->on_select(tbl, clicked_idx, ui_packet_table_get_selected(tbl), tbl->user_data);
                        }
                        need_redraw = true;
                    }
                }
            }

            tbl->is_dragged   = false;
            tbl->is_scrolling = false;
        }
    }

    return need_redraw;
}

static void format_pkt_time(char *buf, size_t sz, uint32_t us) {
    if (us < 1000) {
        snprintf(buf, sz, "%lu us", (unsigned long)us);
    } else if (us < 1000000) {
        snprintf(buf, sz, "%.2f ms", (float)us / 1000.0f);
    } else {
        snprintf(buf, sz, "%.3f s", (float)us / 1000000.0f);
    }
}

/* ================= 4. 切片光栅化渲染 (O(1) 索引剪枝) ================= */

void ui_packet_table_draw(ui_tile_t *tile, const ui_packet_table_t *tbl) {
    if (!tile || !tbl) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &tbl->rect, &overlap)) return;

    // 1. 擦除背景与外边框
    ui_draw_fill_rect(tile, &overlap, tbl->color_bg);
    ui_draw_rect(tile, &tbl->rect, tbl->color_border);

    // ================= A. 顶部固定表头 =================
    ui_rect_t header_rect = ui_rect_make(tbl->rect.x, tbl->rect.y, tbl->rect.w, tbl->header_h);
    ui_rect_t hdr_overlap;
    if (ui_rect_intersect(&tile->rect, &header_rect, &hdr_overlap)) {
        ui_draw_fill_rect(tile, &hdr_overlap, tbl->color_header_bg);
        ui_draw_fast_hline(tile, tbl->rect.x, (int16_t)(tbl->rect.y + tbl->header_h - 1), tbl->rect.w, 0x07FF);

        int16_t hy = tbl->rect.y + 3;
        ui_draw_string(tile, tbl->rect.x + 4,   hy, "#",      FONT_16, 0x07FF, tbl->color_header_bg);
        ui_draw_string(tile, tbl->rect.x + 36,  hy, "Time",   FONT_16, 0x07FF, tbl->color_header_bg);
        ui_draw_string(tile, tbl->rect.x + 104, hy, "Dir",    FONT_16, 0x07FF, tbl->color_header_bg);
        ui_draw_string(tile, tbl->rect.x + 138, hy, "Addr",   FONT_16, 0x07FF, tbl->color_header_bg);
        ui_draw_string(tile, tbl->rect.x + 186, hy, "Payload",FONT_16, 0x07FF, tbl->color_header_bg);
        ui_draw_string(tile, tbl->rect.x + 322, hy, "Status", FONT_16, 0x07FF, tbl->color_header_bg);
    }

    // ================= B. 表体滚动区域 =================
    ui_rect_t body_rect = ui_rect_make(tbl->rect.x, (int16_t)(tbl->rect.y + tbl->header_h), tbl->rect.w, (int16_t)(tbl->rect.h - tbl->header_h));
    ui_draw_set_clip(&body_rect);

    int16_t base_y = body_rect.y - tbl->scroll_y;
    uint16_t start_idx = (tbl->count < UI_PKT_MAX_ROWS) ? 0 : tbl->head;

    // ⚡ 核心优化：仅计算与当前切片相交的行索引范围 (从 O(N) 降至 O(1))
    int16_t first_idx = (tile->rect.y - base_y) / tbl->row_h;
    int16_t last_idx  = (tile->rect.y + tile->rect.h - 1 - base_y) / tbl->row_h;

    if (first_idx < 0) first_idx = 0;
    if (last_idx >= (int16_t)tbl->count) last_idx = (int16_t)tbl->count - 1;

    for (int16_t i = first_idx; i <= last_idx; i++) {
        int16_t row_y = (int16_t)(base_y + i * tbl->row_h);
        ui_rect_t row_rect = ui_rect_make((int16_t)(tbl->rect.x + 2), (int16_t)(row_y + 1), (int16_t)(tbl->rect.w - 4), (int16_t)(tbl->row_h - 2));

        ui_rect_t row_overlap;
        if (!ui_rect_intersect(&tile->rect, &row_rect, &row_overlap)) continue;

        bool is_selected = (i == tbl->selected_idx);
        uint16_t row_bg = is_selected ? tbl->color_selected_bg : ((i % 2 == 0) ? tbl->color_row_even : tbl->color_row_odd);

        ui_draw_fill_rect(tile, &row_overlap, row_bg);
        if (is_selected) {
            ui_draw_rect(tile, &row_rect, 0x07FF);
        }

        uint16_t ring_idx = (start_idx + i) % UI_PKT_MAX_ROWS;
        const ui_pkt_row_t *row = &tbl->rows[ring_idx];
        int16_t ty = row_y + 4;

        if ((ty + 16) > tile->rect.y && ty < (tile->rect.y + tile->rect.h)) {
            // 1. 序号
            char num_str[8];
            snprintf(num_str, sizeof(num_str), "%u", (unsigned int)(i + 1));
            ui_draw_string(tile, tbl->rect.x + 4, ty, num_str, FONT_16, 0x8410, row_bg);

            // 2. 时间戳
            char time_str[16];
            format_pkt_time(time_str, sizeof(time_str), row->time_us);
            ui_draw_string(tile, tbl->rect.x + 36, ty, time_str, FONT_16, 0xFFFF, row_bg);

            // 3. 读写方向
            if (row->status == UI_PKT_STAT_START || row->status == UI_PKT_STAT_STOP) {
                ui_draw_string(tile, tbl->rect.x + 104, ty, "-", FONT_16, 0x8410, row_bg);
            } else {
                ui_draw_string(tile, tbl->rect.x + 104, ty, row->is_read ? "R" : "W", FONT_16, row->is_read ? 0x07FF : 0xFFE0, row_bg);
            }

            // 4. 设备地址
            char addr_str[8];
            if (row->addr != 0xFFFF) {
                snprintf(addr_str, sizeof(addr_str), "0x%02X", (unsigned int)(row->addr & 0xFF));
                ui_draw_string(tile, tbl->rect.x + 138, ty, addr_str, FONT_16, 0xFD20, row_bg);
            } else {
                ui_draw_string(tile, tbl->rect.x + 138, ty, "--", FONT_16, 0x8410, row_bg);
            }

            // 5. 负载数据 Payload
            if (row->data_len > 0) {
                char data_str[24] = "";
                int off = 0;
                for (uint8_t d = 0; d < row->data_len && off < 20; d++) {
                    off += snprintf(data_str + off, sizeof(data_str) - off, "%02X ", row->data[d]);
                }
                ui_draw_string(tile, tbl->rect.x + 186, ty, data_str, FONT_16, 0xFFFF, row_bg);
            } else {
                ui_draw_string(tile, tbl->rect.x + 186, ty, "--", FONT_16, 0x8410, row_bg);
            }

            // 6. 状态 Status
            const char *stat_str = "ACK";
            uint16_t stat_col    = 0x07E0; // 亮绿
            if (row->status == UI_PKT_STAT_NACK) {
                stat_str = "NACK"; stat_col = 0xF800;
            } else if (row->status == UI_PKT_STAT_ERR) {
                stat_str = "ERR";  stat_col = 0xF800;
            } else if (row->status == UI_PKT_STAT_START) {
                stat_str = "START"; stat_col = 0xFFE0;
            } else if (row->status == UI_PKT_STAT_STOP) {
                stat_str = "STOP";  stat_col = 0xF81F;
            }
            ui_draw_string(tile, tbl->rect.x + 322, ty, stat_str, FONT_16, stat_col, row_bg);
        }
    }

    // ================= C. 右侧微型滚动条 =================
    int16_t total_h = (int16_t)tbl->count * tbl->row_h;
    if (total_h > body_rect.h) {
        int16_t bar_w = 2;
        int16_t track_h = body_rect.h - 4;
        int16_t thumb_h = (int16_t)(((int32_t)body_rect.h * track_h) / total_h);
        if (thumb_h < 12) thumb_h = 12;

        int16_t max_scroll = total_h - body_rect.h;
        int16_t thumb_y = body_rect.y + 2;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)tbl->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x = tbl->rect.x + tbl->rect.w - bar_w - 2;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, tbl->color_scrollbar);
        }
    }

    ui_draw_reset_clip();
}