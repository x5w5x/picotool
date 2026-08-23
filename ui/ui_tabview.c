#include "ui_tabview.h"
#include "ui_draw.h"
#include <stdlib.h>
#include <string.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

static int16_t tab_measure_str_width(const char *str, FontSize_t size) {
    if (!str) return 0;
    uint8_t font_h = (size >= FONT_24) ? 24 : ((size >= FONT_20) ? 20 : 16);
    uint8_t half_w = font_h / 2;
    int16_t width = 0;

    while (*str) {
        if ((uint8_t)*str >= 0x80) {
            if (*(str + 1) == '\0') break;
            width += font_h;
            str += 2;
        } else {
            width += half_w;
            str += 1;
        }
    }
    return width;
}

void ui_tabview_init(ui_tabview_t *tv, int16_t x, int16_t y, int16_t w, int16_t h, 
                     int16_t bar_w, int16_t item_h,
                     const char **tab_names, uint8_t tab_count, uint8_t default_idx,
                     ui_tabview_cb_t on_change) {
    if (!tv) return;

    tv->rect         = ui_rect_make(x, y, w, h);
    tv->bar_rect     = ui_rect_make(x, y, bar_w, h);
    tv->content_rect = ui_rect_make((int16_t)(x + bar_w), y, (int16_t)(w - bar_w), h);

    tv->item_h    = (item_h > 24) ? item_h : 44;
    tv->tab_count = (tab_count > UI_TABVIEW_MAX_TABS) ? UI_TABVIEW_MAX_TABS : tab_count;
    tv->content_h = tv->tab_count * tv->item_h;
    tv->scroll_y  = 0;

    for (uint8_t i = 0; i < tv->tab_count; i++) {
        tv->tab_names[i] = tab_names ? tab_names[i] : "";
    }

    tv->active_tab_idx = (default_idx < tv->tab_count) ? default_idx : 0;
    tv->font_size      = FONT_16;

    tv->down_x         = 0;
    tv->down_y         = 0;
    tv->scroll_start_y = 0;
    tv->is_touch_down  = false;
    tv->is_dragged     = false;
    tv->is_scrolling   = false;

    tv->color_bar_bg        = 0x10A2; // 极深蓝黑底
    tv->color_tab_active_bg = 0x2126; // 选中暗灰蓝底
    tv->color_text          = 0x8410; // 未选中文字暗灰
    tv->color_active_text   = 0xFFFF; // 选中文字纯白
    tv->color_indicator     = 0x07FF; // 青蓝高亮指示条
    tv->color_border        = 0x39E7; // 分割线深灰色
    tv->color_scrollbar     = 0x07FF; // 青蓝指示滑块

    tv->on_change           = on_change;
    tv->user_data           = NULL;
}

void ui_tabview_set_active(ui_tabview_t *tv, uint8_t tab_idx) {
    if (!tv || tab_idx >= tv->tab_count || tv->active_tab_idx == tab_idx) return;
    uint8_t old_idx = tv->active_tab_idx;
    tv->active_tab_idx = tab_idx;
    if (tv->on_change) {
        tv->on_change(tv, old_idx, tab_idx, tv->user_data);
    }
}

/* ================= 3. 触控状态机 (Touch Slop + Touch-Up 触发) ================= */

bool ui_tabview_process_touch(ui_tabview_t *tv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!tv || tv->tab_count == 0) return false;

    int16_t max_scroll = tv->content_h - tv->bar_rect.h;
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;

    if (is_touching) {
        if (!tv->is_touch_down) {
            // A. 初次按下 (Touch Down): 必须命中左侧侧边栏视口
            if (ui_rect_contains_point(&tv->bar_rect, tx, ty)) {
                tv->is_touch_down  = true;
                tv->is_dragged     = false;
                tv->is_scrolling   = false;
                tv->down_x         = tx;
                tv->down_y         = ty;
                tv->scroll_start_y = tv->scroll_y;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与 Touch Capture 全局追踪
            int16_t dy = ty - tv->down_y;
            int16_t dx = tx - tv->down_x;

            if (!tv->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                tv->is_dragged   = true;
                tv->is_scrolling = true;
            }

            if (tv->is_dragged && max_scroll > 0) {
                int16_t new_scroll = tv->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (tv->scroll_y != new_scroll) {
                    tv->scroll_y = new_scroll;
                    need_redraw  = true;
                }
            }
        }
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许触发 Tab 切换的时机
        if (tv->is_touch_down) {
            tv->is_touch_down = false;

            // 只有未发生滑动的纯点按，且松手点在侧边栏内才判定切换
            if (!tv->is_dragged && ui_rect_contains_point(&tv->bar_rect, tx, ty)) {
                int16_t rel_y = ty - (tv->bar_rect.y - tv->scroll_y);
                if (rel_y >= 0 && rel_y < tv->content_h) {
                    uint8_t clicked_tab = (uint8_t)(rel_y / tv->item_h);
                    if (clicked_tab < tv->tab_count && clicked_tab != tv->active_tab_idx) {
                        uint8_t old_tab = tv->active_tab_idx;
                        tv->active_tab_idx = clicked_tab;

                        if (tv->on_change) {
                            tv->on_change(tv, old_tab, clicked_tab, tv->user_data);
                        }
                        need_redraw = true;
                    }
                }
            }

            tv->is_dragged   = false;
            tv->is_scrolling = false;
        }
    }

    return need_redraw;
}

/* ================= 4. 切片光栅化渲染 (O(1) 索引剪枝) ================= */

void ui_tabview_draw(ui_tile_t *tile, const ui_tabview_t *tv) {
    if (!tile || !tv || tv->tab_count == 0) return;

    ui_rect_t overlap;
    // 1. 切片相交快速裁剪 (Early Exit)
    if (!ui_rect_intersect(&tile->rect, &tv->bar_rect, &overlap)) {
        return;
    }

    // 2. 擦除侧边栏背景
    ui_draw_fill_rect(tile, &overlap, tv->color_bar_bg);

    // 3. 启用像素级局部裁剪保护
    ui_draw_set_clip(&tv->bar_rect);

    int16_t base_y = tv->bar_rect.y - tv->scroll_y;
    uint8_t font_h = (tv->font_size >= FONT_24) ? 24 : ((tv->font_size >= FONT_20) ? 20 : 16);

    // ⚡ 核心优化：仅计算与当前切片相交的 Tab 索引范围 (从 O(N) 降至 O(1))
    int16_t first_idx = (tile->rect.y - base_y) / tv->item_h;
    int16_t last_idx  = (tile->rect.y + tile->rect.h - 1 - base_y) / tv->item_h;

    if (first_idx < 0) first_idx = 0;
    if (last_idx >= tv->tab_count) last_idx = (int16_t)tv->tab_count - 1;

    for (int16_t i = first_idx; i <= last_idx; i++) {
        int16_t item_y = base_y + i * tv->item_h;
        ui_rect_t item_rect = ui_rect_make(tv->bar_rect.x, item_y, tv->bar_rect.w, tv->item_h);

        ui_rect_t item_overlap;
        if (!ui_rect_intersect(&tile->rect, &item_rect, &item_overlap)) {
            continue;
        }

        bool is_active = (i == tv->active_tab_idx);

        if (is_active) {
            ui_draw_fill_rect(tile, &item_rect, tv->color_tab_active_bg);

            // 选中项左侧 4px 高亮指示条
            ui_rect_t ind_rect = ui_rect_make(tv->bar_rect.x, item_y + 4, 4, tv->item_h - 8);
            ui_draw_fill_rect(tile, &ind_rect, tv->color_indicator);
        }

        // 居中绘制标题文字
        const char *name = tv->tab_names[i];
        if (name && name[0] != '\0') {
            int16_t text_y = item_y + (tv->item_h - font_h) / 2;

            if ((text_y + font_h) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                int16_t str_w = tab_measure_str_width(name, tv->font_size);
                int16_t text_x = tv->bar_rect.x + (tv->bar_rect.w - str_w) / 2 + (is_active ? 2 : 0);
                uint16_t text_col = is_active ? tv->color_active_text : tv->color_text;
                uint16_t bg_col   = is_active ? tv->color_tab_active_bg : tv->color_bar_bg;

                ui_draw_string(tile, text_x, text_y, name, tv->font_size, text_col, bg_col);
            }
        }

        // 行底部分割线
        ui_draw_fast_hline(tile, tv->bar_rect.x + 4, item_y + tv->item_h - 1, tv->bar_rect.w - 8, tv->color_border);
    }

    // 4. 超出视口时绘制微型滚动条
    if (tv->content_h > tv->bar_rect.h) {
        int16_t bar_w = 2;
        int16_t track_h = tv->bar_rect.h - 4;
        int16_t thumb_h = (int16_t)(((int32_t)tv->bar_rect.h * track_h) / tv->content_h);
        if (thumb_h < 12) thumb_h = 12;

        int16_t max_scroll = tv->content_h - tv->bar_rect.h;
        int16_t thumb_y = tv->bar_rect.y + 2;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)tv->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x = tv->bar_rect.x + tv->bar_rect.w - bar_w - 2;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, tv->color_scrollbar);
        }
    }

    ui_draw_reset_clip();

    // 5. 绘制右侧纵向主分割线
    ui_draw_fast_vline(tile, tv->bar_rect.x + tv->bar_rect.w - 1, tv->bar_rect.y, tv->bar_rect.h, tv->color_border);
}