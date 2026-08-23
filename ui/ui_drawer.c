// #include "ui_drawer.h"
// #include "ui_draw.h"
// #include <stdio.h>
// #include <string.h>
// #include <stdlib.h>

// #define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

// static void drawer_scroll_content_draw(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
//     ui_drawer_t *d = (ui_drawer_t *)user_data;
//     if (!d || !tile || !viewport) return;

//     int16_t base_y = viewport->y - scroll_y;

//     for (uint8_t i = 0; i < d->app_count; i++) {
//         int16_t item_y = base_y + i * 42 + 3;
//         ui_rect_t item_rect = ui_rect_make((int16_t)(viewport->x + 4), item_y, (int16_t)(viewport->w - 8), 36);

//         // 1. 视口裁剪
//         ui_rect_t item_vp;
//         if (!ui_rect_intersect(&item_rect, viewport, &item_vp)) continue;

//         // 2. 切片显存裁剪
//         ui_rect_t item_tile;
//         if (!ui_rect_intersect(&item_vp, &tile->rect, &item_tile)) continue;

//         app_t *app = d->apps[i];
//         if (!app) continue;

//         bool is_sel = (d->selected_idx == (int8_t)i);

//         // ⚡ 固定底色：统一采用 0x10A2 暗底色，消除滑动闪烁
//         uint16_t ibg = 0x10A2;
//         ui_draw_fill_rect(tile, &item_tile, ibg);
//         ui_draw_rect(tile, &item_vp, 0x2126);

//         // 3. 文字与图标视口边界安全守卫
//         int16_t text_y = item_rect.y + 10;
//         if (text_y >= viewport->y && (text_y + 16) <= (viewport->y + viewport->h)) {
//             // 图标徽章
//             ui_rect_t badge = ui_rect_make((int16_t)(item_rect.x + 4), (int16_t)(item_rect.y + 5), 32, 26);
//             ui_rect_t badge_tile;
//             if (ui_rect_intersect(&badge, &item_tile, &badge_tile)) {
//                 ui_draw_fill_rect(tile, &badge_tile, app->icon_color);
//                 int16_t sym_w = (int16_t)strlen(app->icon_symbol) * 8;
//                 int16_t sx = (int16_t)(badge.x + (badge.w - sym_w) / 2);
//                 ui_draw_string(tile, sx, (int16_t)(badge.y + 5), app->icon_symbol, FONT_16, 0x0000, app->icon_color);
//             }

//             // 应用文字标题 (当前选中的应用青色高亮，其他为白色)
//             ui_draw_string(tile, (int16_t)(item_rect.x + 40), text_y, app->name, FONT_16, is_sel ? 0x07FF : 0xFFFF, ibg);
//         }
//     }
// }

// void ui_drawer_init(ui_drawer_t *d, int16_t y, int16_t h, int16_t w, ui_drawer_cb_t on_select, void *user_data) {
//     if (!d) return;

//     // 抽屉高度固定为 204px (36px 顶栏 + 168px 内部视口，容纳 4 个完整选项)
//     int16_t exact_drawer_h = 204;
//     d->rect = ui_rect_make(0, y, w, exact_drawer_h);
//     d->is_open = false;

//     d->app_count      = 0;
//     d->selected_idx   = 0;
//     d->pressed_idx    = -1;
//     d->on_select      = on_select;
//     d->user_data      = user_data;
//     d->is_touch_down  = false;
//     d->is_dragged     = false;

//     for (uint8_t i = 0; i < UI_DRAWER_MAX_APPS; i++) {
//         d->apps[i] = NULL;
//     }

//     ui_scrollview_init(&d->sv_body, 0, (int16_t)(y + 36), w, 168, 400, drawer_scroll_content_draw);
//     d->sv_body.user_data       = d;
//     d->sv_body.color_bg        = 0x1084;
//     d->sv_body.color_scrollbar = 0x07FF;
// }

// bool ui_drawer_register_app(ui_drawer_t *d, app_t *app) {
//     if (!d || !app || d->app_count >= UI_DRAWER_MAX_APPS) return false;
//     d->apps[d->app_count++] = app;
//     d->sv_body.content_h = d->app_count * 42 + 6;
//     return true;
// }

// void ui_drawer_open(ui_drawer_t *d) {
//     if (!d) return;
//     d->is_open = true;
//     d->pressed_idx = -1;
//     d->is_touch_down = false;
//     d->is_dragged = false;
// }

// void ui_drawer_close(ui_drawer_t *d) {
//     if (!d) return;
//     d->is_open = false;
//     d->pressed_idx = -1;
//     d->is_touch_down = false;
//     d->is_dragged = false;
// }

// void ui_drawer_toggle(ui_drawer_t *d) {
//     if (!d) return;
//     if (d->is_open) ui_drawer_close(d);
//     else ui_drawer_open(d);
// }

// bool ui_drawer_process_touch(ui_drawer_t *d, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
//     if (!d || !d->is_open || d->app_count == 0) return false;

//     // 1. 点击抽屉外部空白区域 -> 收起抽屉
//     if (is_touching && (tx > d->rect.w || ty < d->rect.y || ty > (d->rect.y + d->rect.h))) {
//         ui_drawer_close(d);
//         return true;
//     }

//     bool refresh = false;

//     // 2. 触控状态机调度
//     if (is_touching) {
//         if (!d->is_touch_down) {
//             // A. 手指按下 (Touch Down): 仅记录坐标起点
//             d->is_touch_down = true;
//             d->is_dragged    = false;
//             d->down_x        = tx;
//             d->down_y        = ty;
//         } else {
//             // B. 手指移动 (Touch Move): 超过 8px 判定为滑动
//             int16_t dy = abs(ty - d->down_y);
//             int16_t dx = abs(tx - d->down_x);

//             if (dy >= TOUCH_SLOP_PX || dx >= TOUCH_SLOP_PX) {
//                 d->is_dragged = true;
//             }
//         }

//         // C. 将触控事件送入 scrollview 计算位移
//         refresh |= ui_scrollview_process_touch(&d->sv_body, true, tx, ty, now_tick);

//     } else {
//         // D. ⚡ 手指松开 (Touch Up): 唯一允许跳转判定的时机
//         if (d->is_touch_down) {
//             d->is_touch_down = false;

//             refresh |= ui_scrollview_process_touch(&d->sv_body, false, tx, ty, now_tick);

//             // 仅在完全没有滑动的纯点击时触发跳转
//             if (!d->is_dragged) {
//                 int16_t rel_y = ty - d->sv_body.view_rect.y + d->sv_body.scroll_y;
//                 if (rel_y >= 0 && tx <= d->rect.w && ty >= d->sv_body.view_rect.y && ty < (d->sv_body.view_rect.y + d->sv_body.view_rect.h)) {
//                     uint8_t sel = (uint8_t)(rel_y / 42);
//                     if (sel < d->app_count) {
//                         d->selected_idx = (int8_t)sel;
//                         ui_drawer_close(d);
//                         if (d->on_select) {
//                             d->on_select(sel, d->user_data);
//                         }
//                         return true;
//                     }
//                 }
//             }

//             d->is_dragged = false;
//         }
//     }

//     return refresh;
// }

// void ui_drawer_draw(ui_tile_t *tile, const ui_drawer_t *d) {
//     if (!tile || !d || !d->is_open) return;

//     // 图层 1: 底板
//     ui_rect_t panel_ov;
//     if (!ui_rect_intersect(&tile->rect, &d->rect, &panel_ov)) return;
//     ui_draw_fill_rect(tile, &panel_ov, 0x1084);

//     // 图层 2: 滚动列表项
//     ui_scrollview_draw(tile, &d->sv_body);

//     // 图层 3: 标头遮罩 (盖住向上滑出的残留文字)
//     ui_rect_t hdr_rect = ui_rect_make(0, d->rect.y, d->rect.w, 36);
//     ui_rect_t hdr_ov;
//     if (ui_rect_intersect(&tile->rect, &hdr_rect, &hdr_ov)) {
//         ui_draw_fill_rect(tile, &hdr_ov, 0x18C3);
//         ui_rect_t hline = ui_rect_make(0, (int16_t)(d->rect.y + 35), d->rect.w, 1);
//         ui_rect_t hline_ov;
//         if (ui_rect_intersect(&tile->rect, &hline, &hline_ov)) {
//             ui_draw_fill_rect(tile, &hline_ov, 0x07FF);
//         }
//         ui_draw_string(tile, 38, (int16_t)(d->rect.y + 10), "功能清单", FONT_16, 0x07FF, 0x18C3);
//     }

//     // 图层 4: 右侧发光分割线
//     ui_rect_t vline_rect = ui_rect_make((int16_t)(d->rect.w - 1), d->rect.y, 1, d->rect.h);
//     ui_rect_t vline_ov;
//     if (ui_rect_intersect(&tile->rect, &vline_rect, &vline_ov)) {
//         ui_draw_fill_rect(tile, &vline_ov, 0x07FF);
//     }
// }

#include "ui_drawer.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

static void drawer_scroll_content_draw(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    ui_drawer_t *d = (ui_drawer_t *)user_data;
    if (!d || !tile || !viewport) return;

    int16_t base_y = viewport->y - scroll_y;

    for (uint8_t i = 0; i < d->app_count; i++) {
        int16_t item_y = base_y + i * 42 + 3;
        ui_rect_t item_rect = ui_rect_make((int16_t)(viewport->x + 4), item_y, (int16_t)(viewport->w - 8), 36);

        // 1. 视口裁剪
        ui_rect_t item_vp;
        if (!ui_rect_intersect(&item_rect, viewport, &item_vp)) continue;

        // 2. 切片显存裁剪
        ui_rect_t item_tile;
        if (!ui_rect_intersect(&item_vp, &tile->rect, &item_tile)) continue;

        app_t *app = d->apps[i];
        if (!app) continue;

        bool is_sel = (d->selected_idx == (int8_t)i);

        // ⚡ 选项背景：使用圆角矩形（胶囊形状），半径 = 高度/2 = 18
        // uint16_t ibg = 0xBF5E;
    uint16_t ibg = 0xFFFF;
        // 3. 文字与图标视口边界安全守卫
        int16_t text_y = item_rect.y + 10;
        if (text_y >= viewport->y && (text_y + 16) <= (viewport->y + viewport->h)) {
            // 应用文字标题：统一黑色
            ui_draw_string(tile, (int16_t)(item_rect.x + 14), text_y, app->name, FONT_20, 0xC618, ibg);
        }
    }
}

void ui_drawer_init(ui_drawer_t *d, int16_t y, int16_t h, int16_t w, ui_drawer_cb_t on_select, void *user_data) {
    if (!d) return;

    int16_t exact_drawer_h = 204;
    d->rect = ui_rect_make(0, y, w, exact_drawer_h);
    d->is_open = false;

    d->app_count      = 0;
    d->selected_idx   = 0;
    d->pressed_idx    = -1;
    d->on_select      = on_select;
    d->user_data      = user_data;
    d->is_touch_down  = false;
    d->is_dragged     = false;

    for (uint8_t i = 0; i < UI_DRAWER_MAX_APPS; i++) {
        d->apps[i] = NULL;
    }

    ui_scrollview_init(&d->sv_body, 0, (int16_t)(y + 36), w, 168, 400, drawer_scroll_content_draw);
    d->sv_body.user_data       = d;
    d->sv_body.color_bg        = 0xFFFF;   // 滚动视口背景白色
    d->sv_body.color_scrollbar = 0x0000;   // 滚动条黑色
}

bool ui_drawer_register_app(ui_drawer_t *d, app_t *app) {
    if (!d || !app || d->app_count >= UI_DRAWER_MAX_APPS) return false;
    d->apps[d->app_count++] = app;
    d->sv_body.content_h = d->app_count * 42 + 6;
    return true;
}

void ui_drawer_open(ui_drawer_t *d) {
    if (!d) return;
    d->is_open = true;
    d->pressed_idx = -1;
    d->is_touch_down = false;
    d->is_dragged = false;
}

void ui_drawer_close(ui_drawer_t *d) {
    if (!d) return;
    d->is_open = false;
    d->pressed_idx = -1;
    d->is_touch_down = false;
    d->is_dragged = false;
}

void ui_drawer_toggle(ui_drawer_t *d) {
    if (!d) return;
    if (d->is_open) ui_drawer_close(d);
    else ui_drawer_open(d);
}

bool ui_drawer_process_touch(ui_drawer_t *d, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!d || !d->is_open || d->app_count == 0) return false;

    if (is_touching && (tx > d->rect.w || ty < d->rect.y || ty > (d->rect.y + d->rect.h))) {
        ui_drawer_close(d);
        return true;
    }

    bool refresh = false;

    if (is_touching) {
        if (!d->is_touch_down) {
            d->is_touch_down = true;
            d->is_dragged    = false;
            d->down_x        = tx;
            d->down_y        = ty;
        } else {
            int16_t dy = abs(ty - d->down_y);
            int16_t dx = abs(tx - d->down_x);
            if (dy >= TOUCH_SLOP_PX || dx >= TOUCH_SLOP_PX) {
                d->is_dragged = true;
            }
        }
        refresh |= ui_scrollview_process_touch(&d->sv_body, true, tx, ty, now_tick);
    } else {
        if (d->is_touch_down) {
            d->is_touch_down = false;
            refresh |= ui_scrollview_process_touch(&d->sv_body, false, tx, ty, now_tick);

            if (!d->is_dragged) {
                int16_t rel_y = ty - d->sv_body.view_rect.y + d->sv_body.scroll_y;
                if (rel_y >= 0 && tx <= d->rect.w && ty >= d->sv_body.view_rect.y && ty < (d->sv_body.view_rect.y + d->sv_body.view_rect.h)) {
                    uint8_t sel = (uint8_t)(rel_y / 42);
                    if (sel < d->app_count) {
                        d->selected_idx = (int8_t)sel;
                        ui_drawer_close(d);
                        if (d->on_select) {
                            d->on_select(sel, d->user_data);
                        }
                        return true;
                    }
                }
            }
            d->is_dragged = false;
        }
    }

    return refresh;
}

void ui_drawer_draw(ui_tile_t *tile, const ui_drawer_t *d) {
    if (!tile || !d || !d->is_open) return;

    // 图层 1: 底板（白色矩形，保持直角）
    ui_rect_t panel_ov;
    if (!ui_rect_intersect(&tile->rect, &d->rect, &panel_ov)) return;
    ui_draw_fill_rect(tile, &panel_ov, 0xFFFF);

    // 图层 2: 滚动列表项（内部选项为圆角矩形）
    ui_scrollview_draw(tile, &d->sv_body);

    // 图层 3: 标头遮罩（白色背景，黑色文字和分割线）
    ui_rect_t hdr_rect = ui_rect_make(0, d->rect.y, d->rect.w, 36);
    ui_rect_t hdr_ov;
    if (ui_rect_intersect(&tile->rect, &hdr_rect, &hdr_ov)) {
        ui_draw_fill_rect(tile, &hdr_ov, 0xFFFF);
        ui_rect_t hline = ui_rect_make(0, (int16_t)(d->rect.y + 35), d->rect.w, 1);
        ui_rect_t hline_ov;
        if (ui_rect_intersect(&tile->rect, &hline, &hline_ov)) {
            ui_draw_fill_rect(tile, &hline_ov, 0x0000);
        }
        ui_draw_string(tile, 42, (int16_t)(d->rect.y + 10), "功能", FONT_24, 0xBF5E, 0xFFFF);
    }

    // 图层 4: 右侧分割线（黑色）
    ui_rect_t vline_rect = ui_rect_make((int16_t)(d->rect.w - 1), d->rect.y, 1, d->rect.h);
    ui_rect_t vline_ov;
    if (ui_rect_intersect(&tile->rect, &vline_rect, &vline_ov)) {
        ui_draw_fill_rect(tile, &vline_ov, 0x0000);
    }

    // 图层 5: 顶部和底部边界线（黑色）
    ui_rect_t top_line = ui_rect_make(d->rect.x, d->rect.y, d->rect.w, 1);
    ui_rect_t bottom_line = ui_rect_make(d->rect.x, (int16_t)(d->rect.y + d->rect.h - 1), d->rect.w, 1);
    ui_rect_t top_ov, bottom_ov;
    if (ui_rect_intersect(&tile->rect, &top_line, &top_ov)) {
        ui_draw_fill_rect(tile, &top_ov, 0x0000);
    }
    if (ui_rect_intersect(&tile->rect, &bottom_line, &bottom_ov)) {
        ui_draw_fill_rect(tile, &bottom_ov, 0x0000);
    }
}