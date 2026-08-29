#include "app_manager.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lcd.h"
#define EDGE_ZONE_LEFT_PX   50
#define EDGE_ZONE_RIGHT_PX  430
#define SWIPE_THRESHOLD_PX  40
#define SWIPE_MAX_DEVIATION 60
#define DRAWER_WIDTH        140

static void on_drawer_app_select(uint8_t app_idx, void *user_data) {
    app_manager_t *mgr = (app_manager_t *)user_data;
    if (!mgr) return;
    app_manager_switch_to(mgr, app_idx);
}

static void draw_home_dashboard(ui_tile_t *tile, const app_manager_t *mgr) {
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &body, &overlap)) return;

    ui_draw_fill_rect(tile, &overlap, 0x0947);

    // 顶部横幅
    ui_rect_t banner_rect = ui_rect_make(12, 40, 456, 54);
    if (ui_rect_intersect(&tile->rect, &banner_rect, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x11A9);
        ui_draw_rect(tile, &banner_rect, 0x07FF);

        ui_rect_t chip_box = ui_rect_make(20, 47, 44, 40);
        ui_draw_fill_rect(tile, &chip_box, 0x03E0);
        ui_draw_rect(tile, &chip_box, 0x07E0);
        ui_draw_string(tile, 24, 53, "RP2040", FONT_16, 0xFFFF, 0x03E0);
        ui_draw_string(tile, 28, 69, "WORK",   FONT_16, 0x07FF, 0x03E0);

        ui_draw_string(tile, 74, 48, "EMBEDDED WORKSTATION v2.0", FONT_16, 0xFFE0, 0x11A9);
        ui_draw_string(tile, 74, 70, "多功能分析仪 / 示波器 / 离线烧录终端", FONT_16, 0xFFFF, 0x11A9);
    }

    // 监视看板
    ui_rect_t monitor_rect = ui_rect_make(148, 102, 320, 206);
    if (ui_rect_intersect(&tile->rect, &monitor_rect, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x11E9);
        ui_draw_rect(tile, &monitor_rect, 0x07FF);

        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 10, "【核心系统监视】", FONT_16, YELLOW, 0x11E9);
        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 36, "运行模式: 裸机极速流水线 (Bare-Metal)", FONT_16, 0x07FF, 0x11E9);

        char app_cnt_buf[32];
        snprintf(app_cnt_buf, sizeof(app_cnt_buf), "已载入独立应用: %u 个 App 就绪", mgr->app_count);
        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 62, app_cnt_buf, FONT_16, 0x07E0, 0x11E9);

        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 88, "SRAM 内存: 264 KB 正常", FONT_16, WHITE, 0x11E9);
        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 114, "Flash 存储: 8 MB (SPI 总线就绪)", FONT_16, WHITE, 0x11E9);
        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 140, "局部切片裁剪推流: 开启 (60 FPS)", FONT_16, 0x07E0, 0x11E9);
        ui_draw_string(tile, monitor_rect.x + 12, monitor_rect.y + 166, "向右滑动呼出抽屉菜单以进入应用", FONT_16, 0xFD20, 0x11E9);
    }

    // 手势说明
    ui_rect_t guide_rect = ui_rect_make(12, 102, 128, 206);
    if (ui_rect_intersect(&tile->rect, &guide_rect, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x1188);
        ui_draw_rect(tile, &guide_rect, 0x39E7);

        ui_draw_string(tile, guide_rect.x + 8, guide_rect.y + 12, "【手势说明】", FONT_16, YELLOW, 0x1188);
        ui_draw_string(tile, guide_rect.x + 8, guide_rect.y + 40, "左边右滑:", FONT_16, 0x07FF, 0x1188);
        ui_draw_string(tile, guide_rect.x + 8, guide_rect.y + 64, "呼出抽屉菜单", FONT_16, WHITE, 0x1188);

        ui_draw_string(tile, guide_rect.x + 8, guide_rect.y + 106, "右边左滑:", FONT_16, 0x07FF, 0x1188);
        ui_draw_string(tile, guide_rect.x + 8, guide_rect.y + 130, "退出到主界面", FONT_16, WHITE, 0x1188);
    }
}

void app_manager_init(app_manager_t *mgr) {
    if (!mgr) return;
    mgr->app_count        = 0;
    mgr->current_idx      = -1;
    mgr->is_home          = true;
    mgr->edge_touch_down  = false;
    mgr->edge_candidate   = 0;
    mgr->gesture_consumed = false;
    mgr->needs_full_redraw = false;

    for (uint8_t i = 0; i < APP_MAX_COUNT; i++) {
        mgr->apps[i] = NULL;
    }

    ui_drawer_init(&mgr->drawer, 32, 288, DRAWER_WIDTH, on_drawer_app_select, mgr);
}

bool app_manager_register(app_manager_t *mgr, app_t *app) {
    if (!mgr || !app || mgr->app_count >= APP_MAX_COUNT) return false;
    if (app->init) app->init(app);
    mgr->apps[mgr->app_count++] = app;
    ui_drawer_register_app(&mgr->drawer, app);
    return true;
}

bool app_manager_switch_to(app_manager_t *mgr, uint8_t app_idx) {
    if (!mgr || app_idx >= mgr->app_count) return false;

    if (!mgr->is_home) {
        app_t *old_app = app_manager_get_current(mgr);
        if (old_app && old_app->exit) old_app->exit(old_app);
    }

    mgr->is_home     = false;
    mgr->current_idx = (int8_t)app_idx;
    mgr->drawer.selected_idx = (int8_t)app_idx;
    /* ⚡ 切到新 app 时强制下一帧整屏重画 — 否则上一次触摸事件的旧
     * dirty_rect(比如 app_sd 的文件浏览器区域)会限制新 app 的首次渲染 */
    mgr->needs_full_redraw = true;

    app_t *new_app = mgr->apps[app_idx];
    if (new_app && new_app->enter) new_app->enter(new_app);
    return true;
}

bool app_manager_return_home(app_manager_t *mgr) {
    if (!mgr || mgr->is_home) return false;
    app_t *cur = app_manager_get_current(mgr);
    if (cur && cur->exit) cur->exit(cur);

    mgr->is_home     = true;
    mgr->current_idx = -1;
    mgr->drawer.selected_idx = -1;
    /* ⚡ 主动退出当前 app(loader 卸载 plugin / drawer 右滑回主界面等)后,
     * 必须强制下一帧整屏重画,否则 home_dashboard 不会显示,屏冻在
     * 最后一次 plugin UI 上 */
    mgr->needs_full_redraw = true;
    return true;
}

void app_manager_open_drawer(app_manager_t *mgr) {
    if (mgr) ui_drawer_open(&mgr->drawer);
}

void app_manager_close_drawer(app_manager_t *mgr) {
    if (mgr) ui_drawer_close(&mgr->drawer);
}

void app_manager_toggle_drawer(app_manager_t *mgr) {
    if (mgr) ui_drawer_toggle(&mgr->drawer);
}

bool app_manager_is_drawer_open(const app_manager_t *mgr) {
    return mgr ? mgr->drawer.is_open : false;
}

app_t *app_manager_get_current(const app_manager_t *mgr) {
    if (!mgr || mgr->is_home || mgr->current_idx < 0 || mgr->current_idx >= (int8_t)mgr->app_count) {
        return NULL;
    }
    return mgr->apps[mgr->current_idx];
}

void app_manager_update(app_manager_t *mgr, uint32_t now_tick) {
    if (!mgr || mgr->is_home) return;
    app_t *cur = app_manager_get_current(mgr);
    if (cur && cur->update) cur->update(cur, now_tick);
}

/* ================= ⚡ 脏矩形捕获分发核心 ================= */

bool app_manager_process_touch_dirty(app_manager_t *mgr, bool is_touching, int16_t tx, int16_t ty, 
                                     uint32_t now_tick, ui_rect_t *out_dirty, bool *is_full_frame) {
    if (!mgr || !out_dirty || !is_full_frame) return false;
    *is_full_frame = false;

    // 1. 抽屉菜单处于打开状态
    if (mgr->drawer.is_open) {
        bool drawer_changed = ui_drawer_process_touch(&mgr->drawer, is_touching, tx, ty, now_tick);
        if (drawer_changed) {
            // 抽屉滑动时仅推流抽屉所在的 140px 区域
            *out_dirty = mgr->drawer.rect;
            // 若点击项触发了换页，则需要全屏刷新
            if (!mgr->drawer.is_open) {
                *is_full_frame = true;
            }
            return true;
        }
        return false;
    }

    // 2. 边缘手势状态机
    if (is_touching) {
        if (!mgr->edge_touch_down) {
            mgr->edge_touch_down  = true;
            mgr->edge_down_x       = tx;
            mgr->edge_down_y       = ty;
            mgr->gesture_consumed  = false;

            if (tx <= EDGE_ZONE_LEFT_PX) {
                mgr->edge_candidate = 1;
            } else if (tx >= EDGE_ZONE_RIGHT_PX) {
                mgr->edge_candidate = 2;
            } else {
                mgr->edge_candidate = 0;
            }
        } else {
            if (!mgr->gesture_consumed) {
                int16_t dx = tx - mgr->edge_down_x;
                int16_t dy = abs(ty - mgr->edge_down_y);

                // 右滑呼出抽屉
                if (mgr->edge_candidate == 1 && dx >= SWIPE_THRESHOLD_PX && dy <= SWIPE_MAX_DEVIATION) {
                    mgr->gesture_consumed = true;
                    ui_drawer_open(&mgr->drawer);
                    *out_dirty = mgr->drawer.rect; // 仅推流抽屉区域
                    return true;
                }

                // 左滑退出到主界面
                if (mgr->edge_candidate == 2 && dx <= -SWIPE_THRESHOLD_PX && dy <= SWIPE_MAX_DEVIATION) {
                    mgr->gesture_consumed = true;
                    if (!mgr->is_home) {
                        app_manager_return_home(mgr);
                        *is_full_frame = true; // 页面切换才全刷
                        return true;
                    }
                    return false;
                }
            }
        }

        if (mgr->gesture_consumed) return false;

        // 3. 非边缘手势：分发给当前 App
        if (!mgr->is_home) {
            app_t *cur = app_manager_get_current(mgr);
            if (cur && cur->process_touch) {
                // ⚡ App 内部处理，并返回 App 私有的局部脏矩形
                bool redraw = cur->process_touch(cur, is_touching, tx, ty, now_tick);
                if (redraw) {
                    // 若 App 挂载了 user_data 且记录了局部脏矩形，则提取该脏矩形
                    *out_dirty = *(ui_rect_t *)cur->user_data;
                    return true;
                }
            }
        }
        return false;
    } else {
        mgr->edge_touch_down  = false;
        mgr->edge_candidate   = 0;
        bool was_consumed     = mgr->gesture_consumed;
        mgr->gesture_consumed = false;

        if (was_consumed) return false;

        if (!mgr->is_home) {
            app_t *cur = app_manager_get_current(mgr);
            if (cur && cur->process_touch) {
                bool redraw = cur->process_touch(cur, false, tx, ty, now_tick);
                if (redraw) {
                    *out_dirty = *(ui_rect_t *)cur->user_data;
                    return true;
                }
            }
        }
    }

    return false;
}

void app_manager_draw(app_manager_t *mgr, ui_tile_t *tile) {
    if (!mgr) return;

    if (mgr->is_home) {
        draw_home_dashboard(tile, mgr);
    } else {
        app_t *cur = app_manager_get_current(mgr);
        if (cur && cur->draw) cur->draw(cur, tile);
    }

    if (mgr->drawer.is_open) {
        ui_drawer_draw(tile, &mgr->drawer);
    }
}