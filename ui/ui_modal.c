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

    // ✨ 1. App 同款浅色系：与状态卡片、寄存器面板色调 100% 一致
    modal->color_bg      = 0xEF7D; // 暖白底板 (与你终端/烧录卡片完全一致)
    modal->color_border  = 0xC618; // 细腻温润的质感中灰边框
    modal->color_title   = 0x1A7B; // 深沉藏青色文字 (主视觉标题色)

    int16_t title_bar_h  = 42;     // 顶部留出 42px，开阔舒适
    int16_t bottom_bar_h = (btn_mode != UI_MODAL_BTN_NONE) ? 48 : 8;
    int16_t body_y       = modal->rect.y + title_bar_h;
    int16_t body_h       = modal->rect.h - title_bar_h - bottom_bar_h;

    // 内部滚动视口初始化
    ui_scrollview_init(&modal->sv_body, modal->rect.x + 4, body_y, w - 8, body_h, content_h, forward_scroll_draw);
    modal->sv_body.user_data       = modal;
    modal->sv_body.color_bg        = modal->color_bg;
    modal->sv_body.color_scrollbar = 0x3186; // 藏青细滚动条

    // ✨ 2. 右上角“X”大号圆角胶囊：长 44px、高 28px，大拇指随手一戳就能中！
    int16_t close_w = 44;
    int16_t close_h = 28;
    ui_btn_init(&modal->btn_close, modal->rect.x + w - close_w - 8, modal->rect.y + 7, close_w, close_h, "关闭", on_modal_close_btn_click);
    modal->btn_close.style        = UI_BTN_STYLE_ROUND; // 圆角胶囊
    modal->btn_close.font_size    = FONT_16;
    modal->btn_close.color_bg     = 0xCE79;             // 平时浅冷灰底
    modal->btn_close.color_active = 0xF800;             // 按下变亮红
    modal->btn_close.color_text   = 0x1A7B;             // 深藏青字
    modal->btn_close.user_data    = modal;

    // ✨ 3. 底部确定/取消按钮 (App 风格圆角按键)
    int16_t bot_y = modal->rect.y + modal->rect.h - 40;
    if (btn_mode == UI_MODAL_BTN_OK) {
        int16_t bw = 120;
        ui_btn_init(&modal->btn_confirm, modal->rect.x + (w - bw) / 2, bot_y, bw, 32, "确定", on_modal_confirm_btn_click);
        modal->btn_confirm.style        = UI_BTN_STYLE_ROUND;
        modal->btn_confirm.font_size    = FONT_16;
        modal->btn_confirm.color_bg     = 0x05E0; // 翡翠翠绿
        modal->btn_confirm.color_active = 0x03E0;
        modal->btn_confirm.color_text   = 0xFFFF;
        modal->btn_confirm.user_data    = modal;
    } else if (btn_mode == UI_MODAL_BTN_OK_CANCEL) {
        int16_t bw = (w - 36) / 2;
        ui_btn_init(&modal->btn_cancel, modal->rect.x + 12, bot_y, bw, 32, "取消", on_modal_close_btn_click);
        modal->btn_cancel.style        = UI_BTN_STYLE_ROUND;
        modal->btn_cancel.font_size    = FONT_16;
        modal->btn_cancel.color_bg     = 0xCE79; // 沉稳冷灰
        modal->btn_cancel.color_active = 0xA514;
        modal->btn_cancel.color_text   = 0x1A7B;
        modal->btn_cancel.user_data    = modal;

        ui_btn_init(&modal->btn_confirm, modal->rect.x + w - bw - 12, bot_y, bw, 32, "确定", on_modal_confirm_btn_click);
        modal->btn_confirm.style        = UI_BTN_STYLE_ROUND;
        modal->btn_confirm.font_size    = FONT_16;
        modal->btn_confirm.color_bg     = 0x05E0; // 翠绿
        modal->btn_confirm.color_active = 0x03E0;
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

/* ================= 3. 模态触控调度 (含 8px 触控外扩热区) ================= */

bool ui_modal_process_touch(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!modal || !modal->is_open) return false;

    bool need_refresh = false;

    // 1. 优先滚动视口防抖处理
    need_refresh |= ui_scrollview_process_touch(&modal->sv_body, is_touching, tx, ty, now_tick);

    // 2. ⚡ 非拖动状态下响应点击
    if (!modal->sv_body.is_dragged) {
        // ✨ 关闭按钮自带 8 像素向外扩散触控热区，大拇指按在按键外沿也能精准触发
        int16_t close_pad = 8;
        ui_rect_t close_hitbox = ui_rect_make(
            modal->btn_close.rect.x - close_pad,
            modal->btn_close.rect.y - close_pad,
            modal->btn_close.rect.w + close_pad * 2,
            modal->btn_close.rect.h + close_pad * 2
        );

        bool in_close = (tx >= close_hitbox.x && tx < (close_hitbox.x + close_hitbox.w) &&
                         ty >= close_hitbox.y && ty < (close_hitbox.y + close_hitbox.h));

        if (in_close) {
            need_refresh |= ui_btn_process_touch(&modal->btn_close, is_touching, modal->btn_close.rect.x + 4, modal->btn_close.rect.y + 4, now_tick);
        } else {
            if (!is_touching) {
                ui_btn_process_touch(&modal->btn_close, false, tx, ty, now_tick);
            }
        }

        // 底部按键
        if (modal->btn_mode == UI_MODAL_BTN_OK) {
            need_refresh |= ui_btn_process_touch(&modal->btn_confirm, is_touching, tx, ty, now_tick);
        } else if (modal->btn_mode == UI_MODAL_BTN_OK_CANCEL) {
            need_refresh |= ui_btn_process_touch(&modal->btn_cancel, is_touching, tx, ty, now_tick);
            need_refresh |= ui_btn_process_touch(&modal->btn_confirm, is_touching, tx, ty, now_tick);
        }

        // 内容区自定义条目交互
        if (modal->touch_content) {
            need_refresh |= modal->touch_content(modal, is_touching, tx, ty, now_tick, modal->user_data);
        }
    }

    return need_refresh;
}
void ui_modal_draw(ui_tile_t *tile, ui_modal_t *modal) {
    if (!tile || !modal || !modal->is_open) return;

    ui_rect_t overlap;
    // 切片相交快速裁剪：完全不相交的切片直接 0 开销跳过
    if (!ui_rect_intersect(&tile->rect, &modal->rect, &overlap)) {
        return;
    }

    // 图层 1: 暖白底板
    ui_draw_fill_rect(tile, &overlap, modal->color_bg);

    // 图层 2: 内容滚动视口
    ui_scrollview_draw(tile, &modal->sv_body);

    // 图层 3: 顶栏遮罩与标题修饰
    ui_rect_t header_rect = ui_rect_make(modal->rect.x + 1, modal->rect.y + 1, modal->rect.w - 2, 40);
    ui_rect_t hdr_ov;
    if (ui_rect_intersect(&tile->rect, &header_rect, &hdr_ov)) {
        ui_draw_fill_rect(tile, &hdr_ov, modal->color_bg);

        // 视觉锚点：左侧 3px 极客蓝竖向装饰条
        ui_rect_t tag_bar = ui_rect_make(modal->rect.x + 12, modal->rect.y + 12, 3, 18);
        ui_draw_fill_rect(tile, &tag_bar, 0x03DF);

        if (modal->title) {
            ui_draw_string(tile, modal->rect.x + 22, modal->rect.y + 13, modal->title, FONT_16, modal->color_title, modal->color_bg);
        }
        // 软灰细分隔线
        ui_draw_fast_hline(tile, modal->rect.x + 8, modal->rect.y + 40, modal->rect.w - 16, 0xCE59);
        ui_btn_draw(tile, &modal->btn_close);
    }

    // 图层 4: 底栏遮罩
    if (modal->btn_mode != UI_MODAL_BTN_NONE) {
        ui_rect_t footer_rect = ui_rect_make(modal->rect.x + 1, modal->rect.y + modal->rect.h - 46, modal->rect.w - 2, 45);
        ui_rect_t ftr_ov;
        if (ui_rect_intersect(&tile->rect, &footer_rect, &ftr_ov)) {
            ui_draw_fill_rect(tile, &ftr_ov, modal->color_bg);
            ui_draw_fast_hline(tile, modal->rect.x + 8, footer_rect.y, modal->rect.w - 16, 0xCE59);
            if (modal->btn_mode == UI_MODAL_BTN_OK) {
                ui_btn_draw(tile, &modal->btn_confirm);
            } else {
                ui_btn_draw(tile, &modal->btn_cancel);
                ui_btn_draw(tile, &modal->btn_confirm);
            }
        }
    }

    // 图层 5: 弹窗外框 (质感温润灰框)
    ui_draw_rect(tile, &modal->rect, modal->color_border);
}