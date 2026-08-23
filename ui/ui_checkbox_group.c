#include "ui_checkbox_group.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_checkbox_group_init(ui_checkbox_group_t *cg, int16_t x, int16_t y, int16_t w, int16_t h,
                            ui_check_mode_t mode, int16_t item_h, ui_checkbox_cb_t on_change) {
    if (!cg) return;

    cg->rect       = ui_rect_make(x, y, w, h);
    cg->mode       = mode;
    cg->item_count = 0;
    cg->item_h     = (item_h > 20) ? item_h : 36;
    cg->scroll_y   = 0;
    cg->content_h  = 0;

    cg->down_x         = 0;
    cg->down_y         = 0;
    cg->scroll_start_y = 0;
    cg->is_touch_down  = false;
    cg->is_dragged     = false;
    cg->is_scrolling   = false;

    cg->color_bg            = 0x0842; // 极深蓝黑底
    cg->color_box_bg        = 0x18C3; // 选项框深灰底
    cg->color_box_border    = 0x39E7; // 选项框外框
    cg->color_checked       = 0x07E0; // 亮绿勾选色
    cg->color_text          = 0xFFFF; // 纯白字
    cg->color_text_disabled = 0x632C; // 禁用暗灰
    cg->color_border        = 0x39E7; // 外框分割线
    cg->color_scrollbar     = 0x07FF; // 青蓝滚动指示条

    cg->on_change = on_change;
    cg->user_data = NULL;
}

bool ui_checkbox_group_add_item(ui_checkbox_group_t *cg, const char *label, bool init_checked, bool is_disabled) {
    if (!cg || cg->item_count >= UI_CHECK_MAX_ITEMS) return false;

    uint8_t idx = cg->item_count;
    strncpy(cg->items[idx].label, label ? label : "", sizeof(cg->items[idx].label) - 1);
    cg->items[idx].label[sizeof(cg->items[idx].label) - 1] = '\0';
    cg->items[idx].is_checked  = init_checked;
    cg->items[idx].is_disabled = is_disabled;

    cg->item_count++;
    cg->content_h = cg->item_count * cg->item_h;
    return true;
}

void ui_checkbox_group_set_checked(ui_checkbox_group_t *cg, uint8_t index, bool checked) {
    if (!cg || index >= cg->item_count || cg->items[index].is_disabled) return;

    if (cg->mode == UI_CHECK_MODE_SINGLE) {
        if (checked) {
            for (uint8_t i = 0; i < cg->item_count; i++) {
                cg->items[i].is_checked = (i == index);
            }
            if (cg->on_change) cg->on_change(cg, index, true, cg->user_data);
        }
    } else {
        if (cg->items[index].is_checked != checked) {
            cg->items[index].is_checked = checked;
            if (cg->on_change) cg->on_change(cg, index, checked, cg->user_data);
        }
    }
}

bool ui_checkbox_group_is_checked(const ui_checkbox_group_t *cg, uint8_t index) {
    if (!cg || index >= cg->item_count) return false;
    return cg->items[index].is_checked;
}

void ui_checkbox_group_clear(ui_checkbox_group_t *cg) {
    if (!cg) return;
    cg->item_count = 0;
    cg->content_h  = 0;
    cg->scroll_y   = 0;
}

/* ================= 触控状态机 (拖拽滚动 + Touch-Up 勾选) ================= */

bool ui_checkbox_group_process_touch(ui_checkbox_group_t *cg, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!cg || cg->item_count == 0) return false;

    int16_t max_scroll = cg->content_h - cg->rect.h;
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;

    if (is_touching) {
        if (!cg->is_touch_down) {
            // A. 初次按下 (Touch Down): 命中视口
            if (ui_rect_contains_point(&cg->rect, tx, ty)) {
                cg->is_touch_down  = true;
                cg->is_dragged     = false;
                cg->is_scrolling   = false;
                cg->down_x         = tx;
                cg->down_y         = ty;
                cg->scroll_start_y = cg->scroll_y;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与平滑滚动追踪
            int16_t dy = ty - cg->down_y;
            int16_t dx = tx - cg->down_x;

            if (!cg->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                cg->is_dragged   = true;
                cg->is_scrolling = true;
            }

            if (cg->is_dragged && max_scroll > 0) {
                int16_t new_scroll = cg->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (cg->scroll_y != new_scroll) {
                    cg->scroll_y = new_scroll;
                    need_redraw  = true;
                }
            }
        }
        return need_redraw;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 判定点击切换
        if (cg->is_touch_down) {
            cg->is_touch_down = false;

            // 仅在未发生滑动且松手点仍在视口内时结算点击
            if (!cg->is_dragged && ui_rect_contains_point(&cg->rect, tx, ty)) {
                int16_t rel_y = ty - (cg->rect.y - cg->scroll_y);
                if (rel_y >= 0 && rel_y < cg->content_h) {
                    uint8_t clicked_idx = (uint8_t)(rel_y / cg->item_h);

                    if (clicked_idx < cg->item_count && !cg->items[clicked_idx].is_disabled) {
                        if (cg->mode == UI_CHECK_MODE_SINGLE) {
                            for (uint8_t i = 0; i < cg->item_count; i++) {
                                cg->items[i].is_checked = (i == clicked_idx);
                            }
                            if (cg->on_change) cg->on_change(cg, clicked_idx, true, cg->user_data);
                        } else {
                            cg->items[clicked_idx].is_checked = !cg->items[clicked_idx].is_checked;
                            if (cg->on_change) cg->on_change(cg, clicked_idx, cg->items[clicked_idx].is_checked, cg->user_data);
                        }
                        need_redraw = true;
                    }
                }
            }

            cg->is_dragged   = false;
            cg->is_scrolling = false;
        }
    }

    return need_redraw;
}

/* ================= 切片光栅化渲染 (O(1) 行级剪枝) ================= */

void ui_checkbox_group_draw(ui_tile_t *tile, const ui_checkbox_group_t *cg) {
    if (!tile || !cg || cg->item_count == 0) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &cg->rect, &overlap)) {
        return;
    }

    // 1. 擦除背景与外边框
    ui_draw_fill_rect(tile, &overlap, cg->color_bg);
    ui_draw_rect(tile, &cg->rect, cg->color_border);

    // 2. 启用像素级局部视口裁剪
    ui_draw_set_clip(&cg->rect);

    int16_t base_y = cg->rect.y - cg->scroll_y;

    // ⚡ 3. 核心行级算术剪枝 (从 O(N) 降至 O(1))
    int16_t first_idx = (tile->rect.y - base_y) / cg->item_h;
    int16_t last_idx  = (tile->rect.y + tile->rect.h - 1 - base_y) / cg->item_h;

    if (first_idx < 0) first_idx = 0;
    if (last_idx >= (int16_t)cg->item_count) last_idx = (int16_t)cg->item_count - 1;

    for (int16_t i = first_idx; i <= last_idx; i++) {
        int16_t item_y = base_y + i * cg->item_h;
        const ui_check_item_t *item = &cg->items[i];

        int16_t box_size = 18;
        int16_t box_x = cg->rect.x + 10;
        int16_t box_y = item_y + (cg->item_h - box_size) / 2;

        // A. 绘制勾选框 / 单选圆钮
        if (cg->mode == UI_CHECK_MODE_MULTI) {
            ui_rect_t box_rect = ui_rect_make(box_x, box_y, box_size, box_size);
            ui_rect_t box_ov;
            if (ui_rect_intersect(&tile->rect, &box_rect, &box_ov)) {
                ui_draw_fill_rect(tile, &box_ov, cg->color_box_bg);
                ui_draw_rect(tile, &box_rect, item->is_checked ? cg->color_checked : cg->color_box_border);

                if (item->is_checked) {
                    int16_t cx = box_x + box_size / 2;
                    int16_t cy = box_y + box_size / 2;
                    uint16_t col = item->is_disabled ? cg->color_text_disabled : cg->color_checked;
                    ui_draw_line(tile, cx - 4, cy, cx - 1, cy + 3, col);
                    ui_draw_line(tile, cx - 1, cy + 3, cx + 5, cy - 4, col);
                }
            }
        } else {
            int16_t cx = box_x + box_size / 2;
            int16_t cy = box_y + box_size / 2;
            int16_t r  = box_size / 2;

            ui_draw_fill_circle(tile, cx, cy, r, cg->color_box_bg);
            ui_draw_circle(tile, cx, cy, r, item->is_checked ? cg->color_checked : cg->color_box_border);

            if (item->is_checked) {
                uint16_t col = item->is_disabled ? cg->color_text_disabled : cg->color_checked;
                ui_draw_fill_circle(tile, cx, cy, r - 4, col);
            }
        }

        // B. 绘制选项文字
        int16_t text_x = box_x + box_size + 10;
        int16_t text_y = item_y + (cg->item_h - 16) / 2;

        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            uint16_t text_col = item->is_disabled ? cg->color_text_disabled : cg->color_text;
            ui_draw_string(tile, text_x, text_y, item->label, FONT_16, text_col, cg->color_bg);
        }

        // C. 行底部分割线
        int16_t line_y = item_y + cg->item_h - 1;
        if (line_y >= tile->rect.y && line_y < (tile->rect.y + tile->rect.h) && (i < cg->item_count - 1)) {
            ui_draw_fast_hline(tile, cg->rect.x + 6, line_y, cg->rect.w - 12, 0x18C3);
        }
    }

    // 4. 超出视口时绘制微型滚动条
    if (cg->content_h > cg->rect.h) {
        int16_t bar_w = 2;
        int16_t track_h = cg->rect.h - 4;
        int16_t thumb_h = (int16_t)(((int32_t)cg->rect.h * track_h) / cg->content_h);
        if (thumb_h < 12) thumb_h = 12;

        int16_t max_scroll = cg->content_h - cg->rect.h;
        int16_t thumb_y = cg->rect.y + 2;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)cg->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x = cg->rect.x + cg->rect.w - bar_w - 2;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, cg->color_scrollbar);
        }
    }

    ui_draw_reset_clip();
}