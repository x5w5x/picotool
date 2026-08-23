#ifndef UI_MODAL_H
#define UI_MODAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "ui_btn.h"
#include "ui_scrollview.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_modal ui_modal_t;

typedef enum {
    UI_MODAL_BTN_NONE = 0,
    UI_MODAL_BTN_OK,
    UI_MODAL_BTN_OK_CANCEL
} ui_modal_btn_mode_t;

typedef void (*ui_modal_draw_content_cb_t)(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data);
typedef bool (*ui_modal_touch_content_cb_t)(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data);
typedef void (*ui_modal_action_cb_t)(ui_modal_t *modal, bool is_confirm, void *user_data);

struct ui_modal {
    ui_rect_t                   rect;           // 弹窗窗口包围盒
    bool                        is_open;        // 显隐状态
    const char                 *title;          // 标题文本

    ui_scrollview_t             sv_body;        // 内部滚动视口

    ui_modal_btn_mode_t         btn_mode;
    ui_btn_t                    btn_close;      // 右上角关闭按钮
    ui_btn_t                    btn_cancel;     // 取消按钮
    ui_btn_t                    btn_confirm;    // 确定按钮

    uint16_t                    color_bg;       // 弹窗底色
    uint16_t                    color_border;   // 边框色
    uint16_t                    color_title;    // 标题文本色

    ui_modal_draw_content_cb_t  draw_content;
    ui_modal_touch_content_cb_t touch_content;
    ui_modal_action_cb_t        on_action;
    void                       *user_data;
};

void ui_modal_init(ui_modal_t *modal, int16_t screen_w, int16_t screen_h,
                   int16_t w, int16_t h, int16_t content_h,
                   const char *title, ui_modal_btn_mode_t btn_mode,
                   ui_modal_draw_content_cb_t draw_content,
                   ui_modal_touch_content_cb_t touch_content,
                   ui_modal_action_cb_t on_action);

void ui_modal_show(ui_modal_t *modal, const char *title, void *user_data);
void ui_modal_hide(ui_modal_t *modal);

bool ui_modal_process_touch(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
void ui_modal_draw(ui_tile_t *tile, ui_modal_t *modal);

#ifdef __cplusplus
}
#endif

#endif // UI_MODAL_H