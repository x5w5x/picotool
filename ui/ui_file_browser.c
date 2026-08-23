#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "ui_file_browser.h"
#include "ui_draw.h"

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

static bool str_ends_with_case_insensitive(const char *str, const char *ext) {
    if (!str || !ext) return false; /*[cite: 18] */
    size_t str_len = strlen(str); /*[cite: 18] */
    size_t ext_len = strlen(ext); /*[cite: 18] */
    if (str_len < ext_len) return false; /*[cite: 18] */

    const char *p_str = str + (str_len - ext_len); /*[cite: 18] */
    while (*p_str && *ext) { /*[cite: 18] */
        if (tolower((unsigned char)*p_str) != tolower((unsigned char)*ext)) { /*[cite: 18] */
            return false; /*[cite: 18] */
        }
        p_str++; /*[cite: 18] */
        ext++; /*[cite: 18] */
    }
    return true; /*[cite: 18] */
}

static ui_file_type_t detect_file_type(const char *name, bool is_dir) {
    if (is_dir) return UI_FILE_TYPE_DIR; /*[cite: 18] */
    if (!name) return UI_FILE_TYPE_UNKNOWN; /*[cite: 18] */

    if (str_ends_with_case_insensitive(name, ".bin")) return UI_FILE_TYPE_BIN; /*[cite: 18] */
    if (str_ends_with_case_insensitive(name, ".hex")) return UI_FILE_TYPE_HEX; /*[cite: 18] */
    if (str_ends_with_case_insensitive(name, ".uf2")) return UI_FILE_TYPE_UF2; /*[cite: 18] */
    if (str_ends_with_case_insensitive(name, ".txt") || str_ends_with_case_insensitive(name, ".log")) { /*[cite: 18] */
        return UI_FILE_TYPE_TXT; /*[cite: 18] */
    }

    return UI_FILE_TYPE_UNKNOWN; /*[cite: 18] */
}

// ⚡ 极速文件名按像素宽度安全截断与防溢出算法
static void truncate_filename_by_width(const char *src, char *dst, size_t dst_size, int16_t max_pixel_w) {
    if (!src || !dst || dst_size == 0 || max_pixel_w <= 0) {
        if (dst && dst_size) dst[0] = '\0';
        return;
    }

    int16_t cur_w = 0;
    size_t s_idx = 0;
    size_t d_idx = 0;

    int16_t limit_w = max_pixel_w - 12; // 预留 ".." (12px) 宽度
    if (limit_w < 18) limit_w = max_pixel_w;

    bool truncated = false;
    while (src[s_idx] && (d_idx + 3 < dst_size)) {
        uint8_t c = (uint8_t)src[s_idx];
        int16_t char_w = (c < 0x80) ? 6 : 16;
        size_t step = (c < 0x80) ? 1 : 2;

        if (cur_w + char_w > limit_w) {
            truncated = true;
            break;
        }

        if (step == 2 && src[s_idx + 1] == '\0') break;

        for (size_t k = 0; k < step; k++) {
            dst[d_idx++] = src[s_idx++];
        }
        cur_w += char_w;
    }

    if (truncated && (d_idx + 2 < dst_size)) {
        dst[d_idx++] = '.';
        dst[d_idx++] = '.';
    }
    dst[d_idx] = '\0';
}

void ui_file_browser_init(ui_file_browser_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, ui_file_select_cb_t on_select) {
    if (!fb) return; /*[cite: 18] */

    fb->rect = ui_rect_make(x, y, w, h); /*[cite: 18] */
    fb->item_count   = 0; /*[cite: 18] */
    fb->selected_idx = -1; /*[cite: 18] */
    fb->item_h       = 30; // 紧凑行高 (30px)
    fb->scroll_y     = 0; /*[cite: 18] */

    fb->is_scrolling    = false; /*[cite: 18] */
    fb->is_touch_down   = false;
    fb->is_dragged      = false;
    fb->down_x          = 0;
    fb->down_y          = 0;
    fb->scroll_start_y  = 0;

    fb->color_bg        = 0x0842; /* 极深灰黑底[cite: 18] */
    fb->color_item_bg   = 0x1082; /* 深蓝黑条目底[cite: 18] */
    fb->color_active_bg = 0x2146; /* 选中条目暗青蓝高亮[cite: 18] */
    fb->color_border    = 0x39E7; /* 分割线深灰[cite: 18] */
    fb->color_scrollbar = 0x07FF; /* 青蓝滚动指示条[cite: 18] */

    fb->on_select = on_select; /*[cite: 18] */
    fb->user_data = NULL; /*[cite: 18] */
}

void ui_file_browser_clear(ui_file_browser_t *fb) {
    if (!fb) return; /*[cite: 18] */
    fb->item_count   = 0; /*[cite: 18] */
    fb->selected_idx = -1; /*[cite: 18] */
    fb->scroll_y     = 0; /*[cite: 18] */
}

bool ui_file_browser_add_item(ui_file_browser_t *fb, const char *name, uint32_t size_bytes, bool is_dir) {
    if (!fb || !name || fb->item_count >= UI_FB_MAX_ITEMS) return false; /*[cite: 18] */

    ui_file_item_t *item = &fb->items[fb->item_count]; /*[cite: 18] */
    strncpy(item->name, name, UI_FB_NAME_LEN - 1); /*[cite: 18] */
    item->name[UI_FB_NAME_LEN - 1] = '\0'; /*[cite: 18] */
    item->size_bytes = size_bytes; /*[cite: 18] */
    item->type       = detect_file_type(name, is_dir); /*[cite: 18] */

    fb->item_count++; /*[cite: 18] */
    return true; /*[cite: 18] */
}

const ui_file_item_t *ui_file_browser_get_selected(const ui_file_browser_t *fb) {
    if (!fb || fb->selected_idx < 0 || fb->selected_idx >= (int8_t)fb->item_count) return NULL; /*[cite: 18] */
    return &fb->items[fb->selected_idx]; /*[cite: 18] */
}

/* ================= 触控状态机 (8px 防抖 + Touch-Up 触发) ================= */

bool ui_file_browser_process_touch(ui_file_browser_t *fb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!fb || fb->item_count == 0) return false; /*[cite: 18] */

    int16_t total_h = (int16_t)fb->item_count * fb->item_h; /*[cite: 18] */
    int16_t max_scroll = total_h - fb->rect.h; /*[cite: 18] */
    if (max_scroll < 0) max_scroll = 0; /*[cite: 18] */

    bool need_redraw = false;

    if (is_touching) {
        if (!fb->is_touch_down) {
            /* A. 初次按下 (Touch Down): 仅在视口内记录起点 */
            if (ui_rect_contains_point(&fb->rect, tx, ty)) {
                fb->is_touch_down  = true;
                fb->is_dragged     = false;
                fb->is_scrolling   = false;
                fb->down_x         = tx;
                fb->down_y         = ty;
                fb->scroll_start_y = fb->scroll_y;
            }
        } else {
            /* B. 持续移动 (Touch Move): 8px 防抖死区判定 */
            int16_t dy = ty - fb->down_y;
            int16_t dx = tx - fb->down_x;

            if (!fb->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                fb->is_dragged   = true;
                fb->is_scrolling = true;
            }

            /* C. 拖拽滚动计算 */
            if (fb->is_dragged && max_scroll > 0) {
                int16_t new_scroll = fb->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (fb->scroll_y != new_scroll) {
                    fb->scroll_y = new_scroll;
                    need_redraw  = true;
                }
            }
        }
    } else {
        /* D. ⚡ 手指松开 (Touch Up): 唯一允许触发条目选中的时机 */
        if (fb->is_touch_down) {
            fb->is_touch_down = false;

            /* 只有从未发生滑动的纯点按，且松开点在视口内才触发选中 */
            if (!fb->is_dragged && ui_rect_contains_point(&fb->rect, tx, ty)) {
                int16_t rel_y = ty - (fb->rect.y - fb->scroll_y);
                if (rel_y >= 0 && rel_y < total_h) {
                    int8_t clicked_idx = (int8_t)(rel_y / fb->item_h);
                    if (clicked_idx < (int8_t)fb->item_count) {
                        fb->selected_idx = clicked_idx;
                        if (fb->on_select) {
                            fb->on_select(fb, clicked_idx, &fb->items[clicked_idx], fb->user_data);
                        }
                        need_redraw = true;
                    }
                }
            }

            fb->is_dragged   = false;
            fb->is_scrolling = false;
        }
    }

    return need_redraw;
}

/* ================= 切片安全光栅化渲染 ================= */

void ui_file_browser_draw(ui_tile_t *tile, const ui_file_browser_t *fb) {
    if (!tile || !fb) return; /*[cite: 18] */

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &fb->rect, &overlap)) return; /*[cite: 18] */

    /* 1. 视口底板与外框 */
    ui_draw_fill_rect(tile, &overlap, fb->color_bg); /*[cite: 18] */
    ui_draw_rect(tile, &fb->rect, fb->color_border); /*[cite: 18] */

    /* 2. 开启视口裁剪保护 */
    ui_draw_set_clip(&fb->rect); /*[cite: 18] */

    int16_t base_y = fb->rect.y - fb->scroll_y; /*[cite: 18] */

    /* 3. 遍历渲染文件条目 */
    for (uint8_t i = 0; i < fb->item_count; i++) {
        int16_t item_y = base_y + (int16_t)i * fb->item_h; /*[cite: 18] */
        ui_rect_t item_rect = ui_rect_make(fb->rect.x + 2, item_y + 1, fb->rect.w - 4, fb->item_h - 2);

        /* 切片相交裁剪 */
        ui_rect_t item_overlap;
        if (!ui_rect_intersect(&tile->rect, &item_rect, &item_overlap)) continue; /*[cite: 18] */

        bool is_selected = ((int8_t)i == fb->selected_idx);
        uint16_t row_bg = is_selected ? fb->color_active_bg : fb->color_item_bg; /*[cite: 18] */

        ui_draw_fill_rect(tile, &item_overlap, row_bg); /*[cite: 18] */
        ui_draw_rect(tile, &item_rect, is_selected ? 0x07FF : 0x2124); /*[cite: 18] */

        /* 文字视口边界守卫 */
        int16_t text_y = item_rect.y + 7;
        if ((text_y + 16) > fb->rect.y && text_y < (fb->rect.y + fb->rect.h)) {
            const ui_file_item_t *item = &fb->items[i]; /*[cite: 18] */

            /* 绘制文件类型图标标签 (占宽 36px) */
            const char *tag_str = "BIN";
            uint16_t tag_color  = 0x07E0; /*[cite: 18] */
            if (item->type == UI_FILE_TYPE_DIR) {
                tag_str = "DIR"; tag_color = 0x07FF;
            } else if (item->type == UI_FILE_TYPE_HEX) {
                tag_str = "HEX"; tag_color = 0xFFE0;
            } else if (item->type == UI_FILE_TYPE_UF2) {
                tag_str = "UF2"; tag_color = 0xF81F;
            } else if (item->type == UI_FILE_TYPE_TXT) {
                tag_str = "TXT"; tag_color = 0xFD20;
            }

            ui_draw_string(tile, item_rect.x + 4, text_y, tag_str, FONT_16, tag_color, row_bg);

            /* 计算文件大小文本与所需宽度 */
            char size_str[16] = "";
            int16_t size_w = 0;
            if (item->type != UI_FILE_TYPE_DIR && fb->rect.w >= 180) {
                if (item->size_bytes < 1024) {
                    snprintf(size_str, sizeof(size_str), "%luB", (unsigned long)item->size_bytes);
                } else if (item->size_bytes < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%luK", (unsigned long)(item->size_bytes / 1024));
                } else {
                    snprintf(size_str, sizeof(size_str), "%.1fM", (float)item->size_bytes / (1024.0f * 1024.0f));
                }
                size_w = (int16_t)strlen(size_str) * 6 + 6;
            }

            /* ⚡ 核心优化：根据可用宽度（总宽 - 标签宽 - 大小宽）自动截断文件名，超出追加 '..' */
            int16_t name_start_x = item_rect.x + 32;
            int16_t max_name_w = item_rect.w - 36 - size_w;
            if (max_name_w < 20) max_name_w = 20;

            char truncated_name[UI_FB_NAME_LEN];
            truncate_filename_by_width(item->name, truncated_name, sizeof(truncated_name), max_name_w);

            /* 绘制截断后的安全文件名 (绝不产生任何重叠或 Flash 算力浪费) */
            ui_draw_string(tile, name_start_x, text_y, truncated_name, FONT_16, is_selected ? 0xFFFF : 0xC618, row_bg);

            /* 绘制右对齐文件大小 */
            if (size_w > 0) {
                ui_draw_string(tile, item_rect.x + item_rect.w - size_w, text_y, size_str, FONT_16, 0x8410, row_bg);
            }
        }
    }

    /* 4. 绘制滚动指示滑块 */
    int16_t total_h = (int16_t)fb->item_count * fb->item_h; /*[cite: 18] */
    if (total_h > fb->rect.h) { /*[cite: 18] */
        int16_t bar_w = 2; /*[cite: 18] */
        int16_t track_h = fb->rect.h - 4; /*[cite: 18] */
        int16_t thumb_h = (int16_t)(((int32_t)fb->rect.h * track_h) / total_h); /*[cite: 18] */
        if (thumb_h < 12) thumb_h = 12; /*[cite: 18] */

        int16_t max_scroll = total_h - fb->rect.h; /*[cite: 18] */
        int16_t thumb_y = fb->rect.y + 2; /*[cite: 18] */
        if (max_scroll > 0) { /*[cite: 18] */
            thumb_y += (int16_t)(((int32_t)fb->scroll_y * (track_h - thumb_h)) / max_scroll); /*[cite: 18] */
        }
        int16_t thumb_x = fb->rect.x + fb->rect.w - bar_w - 1;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h); /*[cite: 18] */
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) { /*[cite: 18] */
            ui_draw_fill_rect(tile, &thumb_overlap, fb->color_scrollbar); /*[cite: 18] */
        }
    }

    ui_draw_reset_clip(); /*[cite: 18] */
}