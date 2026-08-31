/*
 * @Author: 轩
 * @Date: 2026-08-31 13:24:12
 * @LastEditTime: 2026-08-31 13:51:12
 * @FilePath: \pico\app\app_template.c
 */
#include "app_template.h"
#include "app_shared_ram.h" // ⚡ 引入共享内存头文件
#include "ui_draw.h"
#include "ui_btn.h"
#include "lcd.h"
#include <stdio.h>
#include <string.h>

/* ================= 1. 组件与外设交互回调 ================= */

static void on_hello_btn_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    (void)user_data;
    printf("hello\r\n");
}

/* ================= 2. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t dirty_rect; // ⚠️ 必须保持在第 1 位！供 app_manager 提取局部脏矩形
    ui_btn_t  btn_hello;  // 业务控件实体
} app_template_data_t;

static app_t               s_app_instance;

/* ================= 3. 生命周期与事件回调 ================= */

static void app_template_init(app_t *self) {
    // ⚡ 仅绑定共享静态池指针
    self->user_data = g_static_app_ram;
}

static void app_template_enter(app_t *self) {
    // ⚡ 进入 App 时从 8KB 静态池就地初始化
    app_template_data_t *data = (app_template_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_template_data_t));

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 控件初始化 (X: 190, Y: 130, 宽: 75, 高: 36)
    ui_btn_init(&data->btn_hello, 190, 130, 75, 36, "你好", on_hello_btn_click);
    data->btn_hello.style     = UI_BTN_STYLE_ROUND;
    data->btn_hello.font_size = FONT_20;
    ui_btn_set_color(&data->btn_hello, 0xBF5E, 0xC618, 0x1A7B);
    data->btn_hello.user_data = data;
}

static void app_template_exit(app_t *self) {
    self->user_data = NULL;
}

static void app_template_update(app_t *self, uint32_t now_tick) {
    (void)self;
    (void)now_tick;
}

static bool app_template_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (!data) return false;

    if (ui_btn_process_touch(&data->btn_hello, is_touching, tx, ty, now_tick)) {
        data->dirty_rect = ui_btn_get_dirty_rect(&data->btn_hello);
        return true;
    }
    return false;
}

/* ================= 4. 切片光栅化渲染 ================= */

static void app_template_draw(app_t *self, ui_tile_t *tile) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_btn_draw(tile, &data->btn_hello);
}

/* ================= 5. 导出单例实体 ================= */

app_t *app_template_get_instance(void) {
    s_app_instance.id            = APP_ID_SETTINGS;
    s_app_instance.name          = "空白模板";
    s_app_instance.init          = app_template_init;
    s_app_instance.enter         = app_template_enter;
    s_app_instance.exit          = app_template_exit;
    s_app_instance.update        = app_template_update;
    s_app_instance.process_touch = app_template_process_touch;
    s_app_instance.draw          = app_template_draw;
    return &s_app_instance;
}