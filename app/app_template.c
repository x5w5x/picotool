/*
 * @Author: 轩
 * @Date: 2026-08-23 10:49:28
 * @LastEditTime: 2026-08-23 11:23:42
 * @FilePath: \pico\app\app_template.c
 */
#include "app_template.h"
#include "ui_draw.h"
#include "lcd.h"

/* ================= 1. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t dirty_rect; // ⚠️ 必须保持在第 1 位！供 app_manager 提取局部脏矩形[cite: 19]
} app_template_data_t;

static app_template_data_t s_template_data;
static app_t               s_app_instance;

/* ================= 2. 生命周期与事件回调 ================= */

static void app_template_init(app_t *self) {
    self->user_data = &s_template_data;
    app_template_data_t *data = &s_template_data;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
}

static void app_template_enter(app_t *self) {
    (void)self;
}

static void app_template_exit(app_t *self) {
    (void)self;
}

static void app_template_update(app_t *self, uint32_t now_tick) {
    (void)self;
    (void)now_tick;
}

static bool app_template_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)self;
    (void)is_touching;
    (void)tx;
    (void)ty;
    (void)now_tick;
    return false;
}

/* ================= 3. 切片光栅化渲染 (纯净画布) ================= */

static void app_template_draw(app_t *self, ui_tile_t *tile) {
    (void)self;
    ui_rect_t overlap;

    // 工作区纯白底色 (Y: 32 ~ 320，避开顶部 32px 状态栏)
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }
}

/* ================= 4. 导出单例实体 ================= */

app_t *app_template_get_instance(void) {
    s_app_instance.id            = APP_ID_SETTINGS; // 对应 app_base.h 中的 app_id_t[cite: 23]
    s_app_instance.name          = "空白模板";
    s_app_instance.icon_symbol   = "APP";
    s_app_instance.icon_color    = 0x03DF;
    s_app_instance.init          = app_template_init;
    s_app_instance.enter         = app_template_enter;
    s_app_instance.exit          = app_template_exit;
    s_app_instance.update        = app_template_update;
    s_app_instance.process_touch = app_template_process_touch;
    s_app_instance.draw          = app_template_draw;
    return &s_app_instance;
}