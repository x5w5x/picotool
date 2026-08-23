#include "ui_modal.h"
#include "ui_draw.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void on_modal_close_btn_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ui_modal_t *m = (ui_modal_t *)user_data;
    m->is_open = false;
    if (m->on_action) m->on_action(m, false, m->user_data);
}

static void on_modal_confirm_btn_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ui_modal_t *m = (ui_modal_t *)user_data;
    m->is_open = false;
    if (m->on_action) m->on_action(m, true, m->user_data);
}

static void forward_scroll_draw(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    ui_modal_t *m = (ui_modal_t *)user_data;
    if (m && m->draw_content) {
        m->draw_content(tile, viewport, scroll_y, m->user_data);
    }
}

void ui_modal_init(ui_modal_t *modal, int16_t screen_w, int16_t screen_h,
                   int16_t w, int16_t h, int16_t content_h,
                   const char *title, ui_modal_btn_mode_t btn_mode,
                   ui_modal_draw_content_cb_t draw_content,
                   ui_modal_touch_content_cb_t touch_content,
                   ui_modal_action_cb_t on_action) {
    if (!modal) return;

    modal->rect = ui_rect_make((screen_w - w) / 2, (screen_h - h) / 2, w, h);
    modal->is_open       = false;
    modal->title         = title;
    modal->btn_mode      = btn_mode;
    modal->draw_content  = draw_content;
    modal->touch_content = touch_content;
    modal->on_action     = on_action;
    modal->user_data     = NULL;

    modal->color_bg      = 0x10A2; // 深蓝黑实体底色
    modal->color_border  = 0x07FF; // 青蓝外框
    modal->color_title   = 0xFFE0; // 金黄标题

    int16_t title_bar_h  = 38;
    int16_t bottom_bar_h = (btn_mode != UI_MODAL_BTN_NONE) ? 46 : 6;
    int16_t body_y       = modal->rect.y + title_bar_h;
    int16_t body_h       = modal->rect.h - title_bar_h - bottom_bar_h;

    // 滚动视口初始化
    ui_scrollview_init(&modal->sv_body, modal->rect.x + 4, body_y, w - 8, body_h, content_h, forward_scroll_draw);
    modal->sv_body.user_data       = modal;
    modal->sv_body.color_bg        = modal->color_bg;
    modal->sv_body.color_scrollbar = 0x07FF;

    // 右上角关闭按钮
    ui_btn_init(&modal->btn_close, modal->rect.x + w - 28, modal->rect.y + 6, 22, 22, "X", on_modal_close_btn_click);
    modal->btn_close.style        = UI_BTN_STYLE_RECT;
    modal->btn_close.color_bg     = 0x3186;
    modal->btn_close.color_active = 0xF800;
    modal->btn_close.color_text   = 0xFFFF;
    modal->btn_close.user_data    = modal;

    // 底部按钮布局
    int16_t bot_y = modal->rect.y + modal->rect.h - 38;
    if (btn_mode == UI_MODAL_BTN_OK) {
        int16_t bw = 120;
        ui_btn_init(&modal->btn_confirm, modal->rect.x + (w - bw) / 2, bot_y, bw, 30, "确定", on_modal_confirm_btn_click);
        modal->btn_confirm.style        = UI_BTN_STYLE_RECT;
        modal->btn_confirm.color_bg     = 0x03E0;
        modal->btn_confirm.color_active = 0x07E0;
        modal->btn_confirm.color_text   = 0xFFFF;
        modal->btn_confirm.user_data    = modal;
    } else if (btn_mode == UI_MODAL_BTN_OK_CANCEL) {
        int16_t bw = (w - 36) / 2;
        ui_btn_init(&modal->btn_cancel, modal->rect.x + 12, bot_y, bw, 30, "取消", on_modal_close_btn_click);
        modal->btn_cancel.style        = UI_BTN_STYLE_RECT;
        modal->btn_cancel.color_bg     = 0x4208;
        modal->btn_cancel.color_active = 0xF800;
        modal->btn_cancel.color_text   = 0xFFFF;
        modal->btn_cancel.user_data    = modal;

        ui_btn_init(&modal->btn_confirm, modal->rect.x + w - bw - 12, bot_y, bw, 30, "应用生效", on_modal_confirm_btn_click);
        modal->btn_confirm.style        = UI_BTN_STYLE_RECT;
        modal->btn_confirm.color_bg     = 0x03E0;
        modal->btn_confirm.color_active = 0x07E0;
        modal->btn_confirm.color_text   = 0xFFFF;
        modal->btn_confirm.user_data    = modal;
    }
}

void ui_modal_show(ui_modal_t *modal, const char *title, void *user_data) {
    if (!modal) return;
    if (title) modal->title = title;
    modal->user_data        = user_data;
    modal->is_open          = true;
    modal->sv_body.scroll_y = 0;
}

void ui_modal_hide(ui_modal_t *modal) {
    if (modal) modal->is_open = false;
}

/* ================= 3. 模态触控调度 (防误触 + 滑动锁死) ================= */

bool ui_modal_process_touch(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!modal || !modal->is_open) return false;

    bool need_refresh = false;

    // 1. 优先将触摸事件分发给内部滚动视口 (内部已自带 8px 防抖与全局捕获锁)
    need_refresh |= ui_scrollview_process_touch(&modal->sv_body, is_touching, tx, ty, now_tick);

    // 2. ⚡ 核心保护：若用户正处于拖拽滑动状态，绝不触发固定按钮或子控件
    if (!modal->sv_body.is_dragged) {
        need_refresh |= ui_btn_process_touch(&modal->btn_close, is_touching, tx, ty, now_tick);
        if (modal->btn_mode == UI_MODAL_BTN_OK) {
            need_refresh |= ui_btn_process_touch(&modal->btn_confirm, is_touching, tx, ty, now_tick);
        } else if (modal->btn_mode == UI_MODAL_BTN_OK_CANCEL) {
            need_refresh |= ui_btn_process_touch(&modal->btn_cancel, is_touching, tx, ty, now_tick);
            need_refresh |= ui_btn_process_touch(&modal->btn_confirm, is_touching, tx, ty, now_tick);
        }

        // 分发给内容区自定义交互组件
        if (modal->touch_content) {
            need_refresh |= modal->touch_content(modal, is_touching, tx, ty, now_tick, modal->user_data);
        }
    }

    return need_refresh;
}

/* ================= 4. 切片光栅化渲染 (层级分明，零无用计算) ================= */

void ui_modal_draw(ui_tile_t *tile, ui_modal_t *modal) {
    if (!tile || !modal || !modal->is_open) return;

    ui_rect_t overlap;
    // 切片相交快速裁剪：完全不相交的切片直接 0 开销退出
    if (!ui_rect_intersect(&tile->rect, &modal->rect, &overlap)) {
        return;
    }

    // 图层 1: 擦除弹窗底板背景
    ui_draw_fill_rect(tile, &overlap, modal->color_bg);

    // 图层 2: 视口区域内容光栅化 (受 ui_draw_set_clip 保护)
    ui_scrollview_draw(tile, &modal->sv_body);

    // 图层 3: 顶层标题栏遮罩 (强行遮盖向上滑出视口的内容残像)
    ui_rect_t header_rect = ui_rect_make(modal->rect.x + 1, modal->rect.y + 1, (int16_t)(modal->rect.w - 2), 37);
    ui_rect_t hdr_ov;
    if (ui_rect_intersect(&tile->rect, &header_rect, &hdr_ov)) {
        ui_draw_fill_rect(tile, &hdr_ov, modal->color_bg);
        if (modal->title) {
            ui_draw_string(tile, modal->rect.x + 12, modal->rect.y + 11, modal->title, FONT_16, modal->color_title, modal->color_bg);
        }
        ui_draw_fast_hline(tile, modal->rect.x + 4, modal->rect.y + 37, modal->rect.w - 8, 0x4208);
        ui_btn_draw(tile, &modal->btn_close);
    }

    // 图层 4: 顶层底部操作栏遮罩 (强行遮盖向下滑出视口的内容残像)
    if (modal->btn_mode != UI_MODAL_BTN_NONE) {
        ui_rect_t footer_rect = ui_rect_make(modal->rect.x + 1, (int16_t)(modal->rect.y + modal->rect.h - 45), (int16_t)(modal->rect.w - 2), 44);
        ui_rect_t ftr_ov;
        if (ui_rect_intersect(&tile->rect, &footer_rect, &ftr_ov)) {
            ui_draw_fill_rect(tile, &ftr_ov, modal->color_bg);
            ui_draw_fast_hline(tile, modal->rect.x + 4, footer_rect.y, modal->rect.w - 8, 0x4208);
            if (modal->btn_mode == UI_MODAL_BTN_OK) {
                ui_btn_draw(tile, &modal->btn_confirm);
            } else {
                ui_btn_draw(tile, &modal->btn_cancel);
                ui_btn_draw(tile, &modal->btn_confirm);
            }
        }
    }

    // 图层 5: 弹窗最外层高亮发光边框
    ui_draw_rect(tile, &modal->rect, modal->color_border);
}