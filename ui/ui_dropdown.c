#include "ui_dropdown.h"
#include "ui_draw.h"
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

/* ================= 1. 内部辅助函数 ================= */

static inline bool ui_dd_hit_rect(const ui_rect_t *rect, int16_t x, int16_t y) {
    return (x >= rect->x && x < (rect->x + rect->w) &&
            y >= rect->y && y < (rect->y + rect->h));
}

static uint8_t ui_dd_get_font_height(FontSize_t size) {
    switch (size) {
        case FONT_16: return 16;
        case FONT_20: return 20;
        case FONT_24: return 24;
        default:      return 16;
    }
}

static uint8_t ui_dd_visible_count(const ui_dropdown_t *dd) {
    if (!dd || dd->item_count == 0) return 0;
    if (dd->max_visible_items == 0 || dd->max_visible_items >= dd->item_count) {
        return dd->item_count;
    }
    return dd->max_visible_items;
}

ui_rect_t ui_dropdown_get_total_rect(const ui_dropdown_t *dd) {
    if (!dd) return ui_rect_make(0, 0, 0, 0);
    ui_rect_t r = dd->rect;
    if (dd->is_open && dd->items && dd->item_count > 0) {
        r.h = (int16_t)(dd->rect.h * (1 + ui_dd_visible_count(dd)));
    }
    return r;
}

/* ================= 2. 初始化与配置 ================= */

void ui_dropdown_init(ui_dropdown_t *dd, int16_t x, int16_t y, int16_t w, int16_t h,
                      const char **items, uint8_t item_count, int8_t default_idx,
                      ui_dropdown_cb_t on_change) {
    if (!dd) return;

    dd->rect          = ui_rect_make(x, y, w, h);
    dd->items         = items;
    dd->item_count    = item_count;
    dd->selected_idx  = (default_idx >= 0 && default_idx < item_count) ? default_idx : 0;
    dd->max_visible_items = 0;
    dd->first_visible_idx = 0;
    dd->list_scroll_y = 0;
    dd->scroll_start_y = 0;

    dd->is_open       = false;
    dd->font_size     = FONT_16;

    dd->color_bg      = 0x18C3; // 深灰黑底色
    dd->color_border  = 0x7BEF; // 浅灰边框
    dd->color_text    = 0xFFFF; // 白色文字
    dd->color_active  = 0x07E0; // 绿色高亮

    dd->down_x        = 0;
    dd->down_y        = 0;
    dd->is_touch_down = false;
    dd->is_dragged    = false;

    dd->on_change     = on_change;
    dd->user_data     = NULL;
}

void ui_dropdown_set_color(ui_dropdown_t *dd, uint16_t bg, uint16_t border, uint16_t text, uint16_t active) {
    if (!dd) return;
    dd->color_bg     = bg;
    dd->color_border = border;
    dd->color_text   = text;
    dd->color_active = active;
}

void ui_dropdown_set_max_visible_items(ui_dropdown_t *dd, uint8_t max_visible_items) {
    if (!dd) return;

    dd->max_visible_items = max_visible_items;
    uint8_t visible_count = ui_dd_visible_count(dd);
    uint8_t max_first = (dd->item_count > visible_count)
        ? (uint8_t)(dd->item_count - visible_count)
        : 0;

    if (dd->selected_idx >= 0 && (uint8_t)dd->selected_idx >= visible_count) {
        dd->first_visible_idx = (uint8_t)dd->selected_idx - visible_count + 1u;
    }
    if (dd->first_visible_idx > max_first) {
        dd->first_visible_idx = max_first;
    }
    dd->list_scroll_y = (int16_t)(dd->first_visible_idx * dd->rect.h);
}

/* ================= 3. 触控状态机 (Touch Slop + Touch-Up 触发) ================= */

bool ui_dropdown_process_touch(ui_dropdown_t *dd, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!dd) return false;

    if (is_touching) {
        if (!dd->is_touch_down) {
            ui_rect_t hit_rect = dd->is_open
                ? ui_dropdown_get_total_rect(dd)
                : dd->rect;
            if (!ui_dd_hit_rect(&hit_rect, tx, ty)) return false;

            // A. 初次按下 (Touch Down): 仅在控件范围内记录起点坐标
            dd->is_touch_down = true;
            dd->is_dragged    = false;
            dd->down_x        = tx;
            dd->down_y        = ty;
            dd->scroll_start_y = dd->list_scroll_y;
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区判定
            int16_t dx = abs(tx - dd->down_x);
            int16_t dy = abs(ty - dd->down_y);
            if (!dd->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                dd->is_dragged = true; // 锁定为滑动，禁止触发选择
            }

            uint8_t visible_count = ui_dd_visible_count(dd);
            if (dd->is_open && dd->item_count > visible_count && dd->is_dragged) {
                int16_t max_scroll = (int16_t)(
                    (dd->item_count - visible_count) * dd->rect.h);
                int16_t new_scroll = (int16_t)(
                    dd->scroll_start_y - (ty - dd->down_y));

                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (new_scroll != dd->list_scroll_y) {
                    dd->list_scroll_y = new_scroll;
                    dd->first_visible_idx = (uint8_t)(new_scroll / dd->rect.h);
                    return true;
                }
            }
        }
        return false;
    } else {
        // C. ⚡ 手指松开 (Touch Up): 唯一允许触发展开、收起或选中的时机
        if (dd->is_touch_down) {
            dd->is_touch_down = false;

            if (dd->is_dragged && dd->rect.h > 0) {
                /* 松手后吸附到最近一行，避免停在半行位置。 */
                dd->list_scroll_y = (int16_t)(
                    ((dd->list_scroll_y + dd->rect.h / 2) / dd->rect.h) *
                    dd->rect.h);
                dd->first_visible_idx = (uint8_t)(dd->list_scroll_y / dd->rect.h);
            }

            // 只有未发生滑动的纯点按才执行动作
            if (!dd->is_dragged) {
                // 1. 收起状态：点击表头展开
                if (!dd->is_open) {
                    if (ui_dd_hit_rect(&dd->rect, tx, ty)) {
                        dd->is_open = true;
                        return true;
                    }
                }
                // 2. 展开状态：点击选项、表头或外部
                else {
                    ui_rect_t total_rect = ui_dropdown_get_total_rect(dd);

                    // 点击在表头上 -> 收起
                    if (ui_dd_hit_rect(&dd->rect, tx, ty)) {
                        dd->is_open = false;
                        return true;
                    }
                    // 点击在下拉选项列表中 -> 选中并收起
                    else if (ui_dd_hit_rect(&total_rect, tx, ty)) {
                        int16_t rel_y = ty - (dd->rect.y + dd->rect.h);
                        int8_t clicked_idx = (int8_t)(
                            (rel_y + dd->list_scroll_y) / dd->rect.h);

                        if (clicked_idx >= 0 && clicked_idx < dd->item_count) {
                            dd->selected_idx = clicked_idx;
                            dd->is_open = false;

                            if (dd->on_change) {
                                dd->on_change(dd, dd->selected_idx, dd->user_data);
                            }
                            return true;
                        }
                    }
                    // 点击在外部空白区 -> 收起
                    else {
                        dd->is_open = false;
                        return true;
                    }
                }
            }
            dd->is_dragged = false;
        }
    }

    return false;
}

/* ================= 4. 切片安全光栅化渲染 ================= */

void ui_dropdown_draw(ui_tile_t *tile, const ui_dropdown_t *dd) {
    if (!tile || !dd) return;

    ui_rect_t total_rect = ui_dropdown_get_total_rect(dd);
    ui_rect_t overlap;

    // 1. 📐 与当前切片显存做快速包围盒裁剪
    if (!ui_rect_intersect(&tile->rect, &total_rect, &overlap)) {
        return;
    }

    uint8_t font_h = ui_dd_get_font_height(dd->font_size);
    int16_t text_pad_y = (dd->rect.h - font_h) / 2;

    // 2. 🔲 绘制主表头
    ui_rect_t hdr_ov;
    if (ui_rect_intersect(&tile->rect, &dd->rect, &hdr_ov)) {
        ui_draw_fill_rect(tile, &hdr_ov, dd->color_bg);
        ui_draw_rect(tile, &dd->rect, dd->is_open ? dd->color_active : dd->color_border);

        // 绘制表头选中文字 (带切片相交防护)
        if (dd->items && dd->selected_idx >= 0 && dd->selected_idx < dd->item_count) {
            int16_t ty = dd->rect.y + text_pad_y;
            if ((ty + font_h) > tile->rect.y && ty < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, dd->rect.x + 6, ty, 
                               dd->items[dd->selected_idx], dd->font_size, dd->color_text, dd->color_bg);
            }
        }

        // 绘制右侧三角指示图标 (▼ / ▲)
        int16_t arrow_cx = dd->rect.x + dd->rect.w - 10;
        int16_t arrow_cy = dd->rect.y + dd->rect.h / 2;
        if (dd->is_open) {
            ui_draw_line(tile, arrow_cx - 4, arrow_cy + 2, arrow_cx + 4, arrow_cy + 2, dd->color_active);
            ui_draw_line(tile, arrow_cx - 2, arrow_cy,     arrow_cx + 2, arrow_cy,     dd->color_active);
            ui_draw_line(tile, arrow_cx,     arrow_cy - 2, arrow_cx,     arrow_cy - 2, dd->color_active);
        } else {
            ui_draw_line(tile, arrow_cx - 4, arrow_cy - 2, arrow_cx + 4, arrow_cy - 2, dd->color_border);
            ui_draw_line(tile, arrow_cx - 2, arrow_cy,     arrow_cx + 2, arrow_cy,     dd->color_border);
            ui_draw_line(tile, arrow_cx,     arrow_cy + 2, arrow_cx,     arrow_cy + 2, dd->color_border);
        }
    }

    // 3. 📜 绘制下拉选项列表
    if (dd->is_open && dd->items) {
        uint8_t visible_count = ui_dd_visible_count(dd);
        ui_rect_t list_rect = ui_rect_make(
            dd->rect.x,
            (int16_t)(dd->rect.y + dd->rect.h),
            dd->rect.w,
            (int16_t)(visible_count * dd->rect.h));

        /*
         * 选项在列表内滚动时，item_rect 会暂时越过列表边界。
         * 保存外层（例如 ScrollView）的裁剪区，并临时收紧到列表区，
         * 这样边框、文字和选中指示条都不会画进上面的数值表头。
         */
        ui_rect_t old_clip = ui_draw_get_clip();
        ui_rect_t list_clip;
        if (ui_rect_intersect(&old_clip, &list_rect, &list_clip)) {
            ui_draw_set_clip(&list_clip);
        } else {
            ui_draw_set_clip(&list_rect);
        }

        for (uint8_t i = 0; i < dd->item_count; i++) {
            ui_rect_t item_rect = ui_rect_make(
                dd->rect.x,
                (int16_t)(dd->rect.y + dd->rect.h +
                          i * dd->rect.h - dd->list_scroll_y),
                dd->rect.w,
                dd->rect.h);
            ui_rect_t item_ov;

            ui_rect_t visible_item;
            if (ui_rect_intersect(&item_rect, &list_rect, &visible_item) &&
                ui_rect_intersect(&tile->rect, &visible_item, &item_ov)) {
                bool is_cur = (i == dd->selected_idx);
                uint16_t item_bg = is_cur ? 0xE71C : dd->color_bg;
                uint16_t item_fg = dd->color_text;

                ui_draw_fill_rect(tile, &item_ov, item_bg);
                ui_draw_rect(tile, &item_rect, dd->color_border);

                if (is_cur) {
                    ui_rect_t active_bar = ui_rect_make(
                        item_rect.x, item_rect.y, 3, item_rect.h);
                    ui_rect_t active_ov;
                    if (ui_rect_intersect(&active_bar, &list_rect, &active_bar) &&
                        ui_rect_intersect(&tile->rect, &active_bar, &active_ov)) {
                        ui_draw_fill_rect(tile, &active_ov, dd->color_active);
                    }
                }

                int16_t ty = item_rect.y + text_pad_y;
                if (item_rect.y >= list_rect.y &&
                    item_rect.y + item_rect.h <= list_rect.y + list_rect.h &&
                    (ty + font_h) > tile->rect.y &&
                    ty < (tile->rect.y + tile->rect.h)) {
                    ui_draw_string(tile, item_rect.x + 6, ty,
                                   dd->items[i], dd->font_size, item_fg, item_bg);
                }
            }
        }

        /* 恢复调用者的裁剪区，不能直接 reset 到全屏。 */
        ui_draw_set_clip(&old_clip);
    }
}
