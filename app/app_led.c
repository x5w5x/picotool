#include "app_led.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "lcd.h"
#include <stdio.h>
#include <string.h>

/* ================= 1. 标签定义与 App 私有数据结构体 ================= */

static const char *s_tab_names[] = {
    "开关控制",
    "工作模式",
    "硬件信息"
};
#define TAB_COUNT (sizeof(s_tab_names) / sizeof(s_tab_names[0]))

typedef struct {
    ui_rect_t     dirty_rect;     // ⚠️ 必须保持在第 1 位！供 app_manager 提取局部脏矩形
    ui_tabview_t  tabview;        // 侧边栏 TabView 容器
    uint8_t       last_tab_idx;   // 用于检测 Tab 切换标记全区重绘

    // --- Tab 0: 开关控制组件 ---
    bool          sw_state;       // 软件开关状态
    ui_switch_t   sw_led;         // 滑动开关
    ui_btn_t      btn_toggle;     // 翻转按键
    ui_rect_t     status_card;    // 状态指示卡片

    // --- Tab 1: 模式选择 ---
    uint8_t       work_mode;      // 0: 常亮, 1: 闪烁, 2: 呼吸
} app_led_data_t;

static app_t          s_app_led;

/* ================= 2. 交互回调函数 ================= */

static void on_tab_change(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab; (void)new_tab; (void)user_data;
}

static void on_switch_change(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_led_data_t *data = (app_led_data_t *)user_data;
    if (data) data->sw_state = state;
}

static void on_btn_toggle_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_led_data_t *data = (app_led_data_t *)user_data;
    if (data) {
        data->sw_state = !data->sw_state;
        ui_switch_set_state(&data->sw_led, data->sw_state);
    }
}

/* ================= 3. 生命周期与事件回调 ================= */

static void app_led_init(app_t *self) {
    // ⚡ 仅绑定共享静态池指针
    self->user_data = g_static_app_ram;
}

static void app_led_enter(app_t *self) {
    // ⚡ 进入 App 时从 8KB 静态池就地初始化
    app_led_data_t *data = (app_led_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_led_data_t));

    data->dirty_rect   = ui_rect_make(0, 0, 0, 0);
    data->sw_state     = false;
    data->work_mode    = 0;
    data->last_tab_idx = 0;

    // 1. 初始化 TabView (Y: 32 ~ 320, 侧边栏宽 110px, 单项高 48px)
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32, 
                    110, 48, s_tab_names, TAB_COUNT, 0, on_tab_change);
    data->tabview.user_data = data;

    // 2. 右侧工作区 (X: 110 ~ 480) 内部控件布局
    data->status_card = ui_rect_make(125, 48, 340, 70);

    ui_switch_init(&data->sw_led, 385, 140, 64, 32, false, on_switch_change);
    data->sw_led.user_data = data;

    ui_btn_init(&data->btn_toggle, 175, 200, 240, 42, "翻转状态", on_btn_toggle_click);
    data->btn_toggle.user_data     = data;
    data->btn_toggle.style         = UI_BTN_STYLE_ROUND;
    data->btn_toggle.font_size     = FONT_20;
    ui_btn_set_color(&data->btn_toggle, 0xBF5E, 0x03DF, 0x1A7B);
}

static void app_led_exit(app_t *self) {
    self->user_data = NULL;
}

static void app_led_update(app_t *self, uint32_t now_tick) {
    (void)self; (void)now_tick;
}

/* ================= 4. 触控状态机分发与局部切片裁剪 ================= */

static bool app_led_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_led_data_t *data = (app_led_data_t *)self->user_data;
    if (!data) return false;

    // 1. 优先分发给 TabView 侧边栏
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    // 2. 仅在 Tab 0 (开关控制) 分发右侧工作区控件触控
    if (data->tabview.active_tab_idx == 0) {
        bool sw_changed  = ui_switch_process_touch(&data->sw_led, is_touching, tx, ty, now_tick);
        bool btn_changed = ui_btn_process_touch(&data->btn_toggle, is_touching, tx, ty, now_tick);

        if (sw_changed) {
            ui_rect_t c_sw = ui_switch_get_dirty_rect(&data->sw_led);
            ui_rect_union(&c_sw, &data->status_card, &data->dirty_rect);
            return true;
        }

        if (btn_changed) {
            data->dirty_rect = ui_btn_get_dirty_rect(&data->btn_toggle);
            return true;
        }
    }

    return false;
}

/* ================= 5. 切片光栅化合成 ================= */

static void app_led_draw(app_t *self, ui_tile_t *tile) {
    app_led_data_t *data = (app_led_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    switch (data->tabview.active_tab_idx) {
        case 0: 
        {
            if (ui_rect_intersect(&tile->rect, &data->status_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->status_card, 0xC618);

                ui_draw_string(tile, 140, 58, "当前通道状态", FONT_16, 0x1A7B, 0xEF7D);

                uint16_t dot_col = data->sw_state ? 0x05E0 : 0x8410;
                ui_draw_fill_circle(tile, 148, 92, 6, dot_col);

                const char *st_str = data->sw_state ? "输出开启 (HIGH / ON)" : "输出关闭 (LOW / OFF)";
                ui_draw_string(tile, 162, 84, st_str, FONT_16, data->sw_state ? 0x05E0 : 0x4208, 0xEF7D);
            }

            int16_t lbl_y = 146;
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 130, lbl_y, "滑动开关主控:", FONT_20, 0x1A7B, 0xFFFF);
            }

            ui_switch_draw(tile, &data->sw_led);
            ui_btn_draw(tile, &data->btn_toggle);
            break;
        }

        case 1: 
        {
            int16_t text_y = 60;
            if ((text_y + 24) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "模式选择面板", FONT_24, 0x1A7B, 0xFFFF);
            }
            text_y = 100;
            if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "模式 1: GPIO 直推模式", FONT_16, 0x4208, 0xFFFF);
            }
            text_y = 130;
            if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "模式 2: PWM 渐变呼吸", FONT_16, 0x4208, 0xFFFF);
            }
            break;
        }

        case 2: 
        {
            int16_t text_y = 60;
            if ((text_y + 24) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "硬件拓扑参数", FONT_24, 0x1A7B, 0xFFFF);
            }
            text_y = 100;
            if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "控制引脚: PICO_DEFAULT_LED", FONT_16, 0x4208, 0xFFFF);
            }
            text_y = 130;
            if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 135, text_y, "驱动模式: 推挽输出 (Push-Pull)", FONT_16, 0x4208, 0xFFFF);
            }
            break;
        }
    }
}

/* ================= 6. 导出单例实体 ================= */

app_t *app_led_get_instance(void) {
    s_app_led.id            = APP_ID_GPIO_MATRIX;
    s_app_led.name          = "LED控制器";
    s_app_led.init          = app_led_init;
    s_app_led.enter         = app_led_enter;
    s_app_led.exit          = app_led_exit;
    s_app_led.update        = app_led_update;
    s_app_led.process_touch = app_led_process_touch;
    s_app_led.draw          = app_led_draw;
    return &s_app_led;
}