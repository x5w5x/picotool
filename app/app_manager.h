#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "app_base.h"
#include "ui_drawer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MAX_COUNT 16

typedef struct {
    app_t          *apps[APP_MAX_COUNT];
    uint8_t         app_count;
    int8_t          current_idx;    // -1 为主界面，>=0 为具体 App
    bool            is_home;

    ui_drawer_t     drawer;

    int16_t         edge_down_x;
    int16_t         edge_down_y;
    bool            edge_touch_down;
    uint8_t         edge_candidate;
    bool            gesture_consumed;
    bool            needs_full_redraw;   // ⚡ switch_to / return_home 后置 true,通知主循环整屏重画
} app_manager_t;

void app_manager_init(app_manager_t *mgr);
bool app_manager_register(app_manager_t *mgr, app_t *app);
bool app_manager_switch_to(app_manager_t *mgr, uint8_t app_idx);
bool app_manager_return_home(app_manager_t *mgr);

void app_manager_open_drawer(app_manager_t *mgr);
void app_manager_close_drawer(app_manager_t *mgr);
void app_manager_toggle_drawer(app_manager_t *mgr);
bool app_manager_is_drawer_open(const app_manager_t *mgr);

app_t *app_manager_get_current(const app_manager_t *mgr);
void app_manager_update(app_manager_t *mgr, uint32_t now_tick);

// ⚡ 带精确脏矩形捕获的触控分发：out_dirty 返回需要重绘的局部区域，is_full_frame 返回是否发生整页切换
bool app_manager_process_touch_dirty(app_manager_t *mgr, bool is_touching, int16_t tx, int16_t ty, 
                                     uint32_t now_tick, ui_rect_t *out_dirty, bool *is_full_frame);

void app_manager_draw(app_manager_t *mgr, ui_tile_t *tile);
extern app_manager_t g_app_mgr;
#ifdef __cplusplus
}
#endif

#endif // APP_MANAGER_H