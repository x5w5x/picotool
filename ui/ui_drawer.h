#ifndef UI_DRAWER_H
#define UI_DRAWER_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "ui_scrollview.h"
#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_DRAWER_MAX_APPS 12

typedef void (*ui_drawer_cb_t)(uint8_t app_idx, void *user_data);

typedef struct {
    ui_rect_t       rect;           // 抽屉整体物理包围盒
    bool            is_open;        // 打开/收起状态
    int8_t          selected_idx;   // 当前选中高亮索引
    int8_t          pressed_idx;    // 临时按压态索引
    uint8_t         app_count;      // 注册应用数
    app_t*          apps[UI_DRAWER_MAX_APPS];
    ui_scrollview_t sv_body;        // 内部滚动视口
    ui_drawer_cb_t  on_select;
    void*           user_data;

    // ⚡ 触控手势防抖与防误触状态机
    int16_t         down_x;
    int16_t         down_y;
    bool            is_touch_down;
    bool            is_dragged;     // 是否已确认为滑动手势 (超过 8px)
} ui_drawer_t;

void ui_drawer_init(ui_drawer_t *d, int16_t y, int16_t h, int16_t w, ui_drawer_cb_t on_select, void *user_data);
bool ui_drawer_register_app(ui_drawer_t *d, app_t *app);
void ui_drawer_open(ui_drawer_t *d);
void ui_drawer_close(ui_drawer_t *d);
void ui_drawer_toggle(ui_drawer_t *d);

bool ui_drawer_process_touch(ui_drawer_t *d, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
void ui_drawer_draw(ui_tile_t *tile, const ui_drawer_t *d);

#ifdef __cplusplus
}
#endif

#endif // UI_DRAWER_H