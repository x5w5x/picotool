#include "ui_scrollview.h"
#include "ui_draw.h"
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

void ui_scrollview_init(ui_scrollview_t *sv, int16_t x, int16_t y, int16_t w, int16_t h,
                        int16_t content_h, ui_scrollview_draw_cb_t draw_content) {
    if (!sv) return;

    sv->view_rect = ui_rect_make(x, y, w, h);
    sv->content_h = (content_h < h) ? h : content_h;
    sv->scroll_y  = 0;

    sv->down_x         = 0;
    sv->down_y         = 0;
    sv->scroll_start_y = 0;
    sv->is_touch_down  = false;
    sv->is_dragged     = false;
    sv->is_scrolling   = false;

    sv->draw_content   = draw_content;
    sv->touch_content  = NULL;
    sv->user_data      = NULL;

    sv->color_bg        = 0x10A2; // 默认深蓝黑底
    sv->color_scrollbar = 0x07FF; // 默认青蓝滚动条
    sv->show_scrollbar  = true;
}

void ui_scrollview_set_touch_cb(ui_scrollview_t *sv, ui_scrollview_touch_cb_t touch_content) {
    if (sv) sv->touch_content = touch_content;
}

ui_rect_t ui_scrollview_content_rect_to_screen(const ui_scrollview_t *sv, ui_rect_t content_rect) {
    if (!sv) return ui_rect_make(0, 0, 0, 0);
    content_rect.x += sv->view_rect.x;
    content_rect.y += sv->view_rect.y - sv->scroll_y;
    return content_rect;
}

bool ui_scrollview_process_touch(ui_scrollview_t *sv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!sv) return false;

    int16_t max_scroll = sv->content_h - sv->view_rect.h;
    if (max_scroll < 0) max_scroll = 0;

    bool need_redraw = false;
    bool child_handled = false;

    if (is_touching) {
        if (!sv->is_touch_down) {
            // A. 初次按下 (Touch Down): 必须落在视口物理区域内
            if (ui_rect_contains_point(&sv->view_rect, tx, ty)) {
                sv->is_touch_down  = true;
                sv->is_dragged     = false;
                sv->is_scrolling   = false;
                sv->down_x         = tx;
                sv->down_y         = ty;
                sv->scroll_start_y = sv->scroll_y;
                if (sv->touch_content)
                    child_handled = sv->touch_content(true, tx - sv->view_rect.x,
                                                      ty - sv->view_rect.y + sv->scroll_y,
                                                      now_tick, sv->user_data);
            }
        } else {
            // B. 持续移动 (Touch Move): ⚡ 获得 Touch Capture，哪怕滑出视口依然平滑追踪
            int16_t dy = ty - sv->down_y;
            int16_t dx = tx - sv->down_x;

            if (!sv->is_dragged && (abs(dy) >= TOUCH_SLOP_PX || abs(dx) >= TOUCH_SLOP_PX)) {
                sv->is_dragged   = true;
                sv->is_scrolling = true;
            }

            if (sv->is_dragged && max_scroll > 0) {
                int16_t new_scroll = sv->scroll_start_y - dy;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > max_scroll) new_scroll = max_scroll;

                if (sv->scroll_y != new_scroll) {
                    sv->scroll_y = new_scroll;
                    need_redraw  = true;
                }
            }
            if (sv->touch_content)
                child_handled = sv->touch_content(true, tx - sv->view_rect.x,
                                                  ty - sv->view_rect.y + sv->scroll_y,
                                                  now_tick, sv->user_data);
        }
    } else {
        // C. 手指松开 (Touch Up): 释放手势锁
        if (sv->is_touch_down) {
            if (sv->touch_content)
                child_handled = sv->touch_content(false, tx - sv->view_rect.x,
                                                  ty - sv->view_rect.y + sv->scroll_y,
                                                  now_tick, sv->user_data);
            sv->is_touch_down = false;
            sv->is_dragged    = false;
            if (sv->is_scrolling) {
                sv->is_scrolling = false;
                need_redraw      = true;
            }
        }
    }

    return need_redraw || child_handled;
}

void ui_scrollview_draw(ui_tile_t *tile, const ui_scrollview_t *sv) {
    if (!tile || !sv) return;

    ui_rect_t overlap;
    // 1. 切片相交裁剪：完全不相交的切片直接 0 开销退出
    if (!ui_rect_intersect(&tile->rect, &sv->view_rect, &overlap)) {
        return;
    }

    // 2. 擦除当前切片内的视口底色 (彻底消除旧图元残影与拖影)
    ui_draw_fill_rect(tile, &overlap, sv->color_bg);

    // 3. 开启像素级视口裁剪
    ui_draw_set_clip(&sv->view_rect);

    // 4. 触发内容光栅化回调
    if (sv->draw_content) {
        sv->draw_content(tile, &sv->view_rect, sv->scroll_y, sv->user_data);
    }

    // 5. 恢复裁剪
    ui_draw_reset_clip();

    // 6. 绘制右侧平滑滚动条
    if (sv->show_scrollbar && (sv->content_h > sv->view_rect.h)) {
        int16_t bar_w      = 3;
        int16_t bar_margin = 2;
        int16_t track_h    = sv->view_rect.h - (bar_margin * 2);

        int16_t thumb_h = (int16_t)(((int32_t)sv->view_rect.h * track_h) / sv->content_h);
        if (thumb_h < 12) thumb_h = 12;
        if (thumb_h > track_h) thumb_h = track_h;

        int16_t max_scroll = sv->content_h - sv->view_rect.h;
        int16_t thumb_y    = sv->view_rect.y + bar_margin;
        if (max_scroll > 0) {
            thumb_y += (int16_t)(((int32_t)sv->scroll_y * (track_h - thumb_h)) / max_scroll);
        }
        int16_t thumb_x    = sv->view_rect.x + sv->view_rect.w - bar_w - bar_margin;

        ui_rect_t thumb_rect = ui_rect_make(thumb_x, thumb_y, bar_w, thumb_h);
        ui_rect_t thumb_overlap;
        if (ui_rect_intersect(&tile->rect, &thumb_rect, &thumb_overlap)) {
            ui_draw_fill_rect(tile, &thumb_overlap, sv->color_scrollbar);
        }
    }
}
