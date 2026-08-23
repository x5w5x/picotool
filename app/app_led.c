#include "app_led.h"
#include "app_manager.h"

#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "lcd.h"

typedef struct {
    ui_rect_t   dirty_rect;  // 必须放在第 1 位，供 app_manager 提取局部脏矩形
    bool        sw_state;    // 纯软件开关状态
    ui_switch_t sw_led;      // 开关控件
    ui_btn_t    btn_toggle;  // 翻转按钮
    ui_rect_t   status_card; // 状态卡片区域
} app_led_data_t;

static app_led_data_t s_led_data;
static app_t          s_app_led;

static void on_switch_change(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_led_data_t *data = (app_led_data_t *)user_data;
    data->sw_state = state; // 仅更新软件状态，不操作硬件
}

static void on_btn_toggle_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_led_data_t *data = (app_led_data_t *)user_data;
    data->sw_state = !data->sw_state;
    ui_switch_set_state(&data->sw_led, data->sw_state); // 同步更新 Switch 控件显示
}

static void app_led_init(app_t *self) {
    self->user_data = &s_led_data;
    app_led_data_t *data = &s_led_data;

    data->sw_state    = false;
    data->status_card = ui_rect_make(20, 50, 440, 76);
    data->dirty_rect  = ui_rect_make(0, 0, 0, 0);

    // 1. 初始化纯软件 Switch 开关
    ui_switch_init(&data->sw_led, 350, 152, 64, 32, false, on_switch_change);
    data->sw_led.user_data = data;

    // 2. 初始化点按翻转按钮
    ui_btn_init(&data->btn_toggle, 130, 218, 220, 44, "翻转软件状态", on_btn_toggle_click);
    data->btn_toggle.user_data   = data;
    data->btn_toggle.color_bg    = 0x18C3;
    data->btn_toggle.color_active= 0x07E0;
}

static void app_led_enter(app_t *self) {
    (void)self;
}

static void app_led_exit(app_t *self) {
    (void)self;
}

static void app_led_update(app_t *self, uint32_t now_tick) {
    (void)self; (void)now_tick;
}

static bool app_led_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_led_data_t *data = (app_led_data_t *)self->user_data;
    bool sw_changed  = ui_switch_process_touch(&data->sw_led, is_touching, tx, ty, now_tick);
    bool btn_changed = ui_btn_process_touch(&data->btn_toggle, is_touching, tx, ty, now_tick);

    if (sw_changed) {
        // 1. 若是开关动作：仅刷新开关自身的小矩形 + 紧邻它的状态指示区
        ui_rect_t c_sw = ui_switch_get_dirty_rect(&data->sw_led);
        ui_rect_t c_status = ui_rect_make(30, 80, 300, 32);
        ui_rect_union(&c_sw, &c_status, &data->dirty_rect);
        return true;
    }
    
    if (btn_changed) {
        // 2. 若是底部按钮动作：仅刷新按钮自身矩形 (绝不和顶部做跨区域并集！高度仅 44px，完美命中单个切片)
        data->dirty_rect = data->btn_toggle.rect;
        return true;
    }

    return false;
}

static void app_led_draw(app_t *self, ui_tile_t *tile) {
    app_led_data_t *data = (app_led_data_t *)self->user_data;
    ui_rect_t overlap;

    // 工作区背景
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x0947);
    }

    // 状态显示卡片
    if (ui_rect_intersect(&tile->rect, &data->status_card, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x11A9);
        ui_rect_t card_rect = data->status_card;
        ui_draw_rect(tile, &card_rect, 0x07FF);

        ui_draw_string(tile, 35, 60, "【纯软件开关状态指示】", FONT_16, YELLOW, 0x11A9);

        uint16_t dot_color = data->sw_state ? 0x07E0 : 0x4208;
        ui_draw_fill_circle(tile, 45, 96, 7, dot_color);
        ui_draw_circle(tile, 45, 96, 7, WHITE);

        const char *state_str = data->sw_state ? "状态: 开启 (TRUE / HIGH)" : "状态: 关闭 (FALSE / LOW)";
        ui_draw_string(tile, 62, 88, state_str, FONT_16, data->sw_state ? 0x07E0 : WHITE, 0x11A9);
    }

    // 控件提示
    ui_rect_t label_rect = ui_rect_make(40, 158, 220, 20);
    if (ui_rect_intersect(&tile->rect, &label_rect, &overlap)) {
        ui_draw_string(tile, 40, 158, "滑动开关控制状态:", FONT_16, WHITE, 0x0947);
    }

    ui_switch_draw(tile, &data->sw_led);
    ui_btn_draw(tile, &data->btn_toggle);

    // 底部提示
    ui_rect_t tip_rect = ui_rect_make(0, 288, LCD_WIDTH, 32);
    if (ui_rect_intersect(&tile->rect, &tip_rect, &overlap)) {
        ui_draw_string(tile, 20, 292, "屏幕左边缘向右滑: 菜单 | 右边缘向左滑: 退出", FONT_16, 0x07FF, 0x0947);
    }
}

app_t *app_led_get_instance(void) {
    s_app_led.id            = APP_ID_GPIO_MATRIX;
    s_app_led.name          = "led控制器";
    s_app_led.icon_color    = 0x07E0;
    s_app_led.init          = app_led_init;
    s_app_led.enter         = app_led_enter;
    s_app_led.exit          = app_led_exit;
    s_app_led.update        = app_led_update;
    s_app_led.process_touch = app_led_process_touch;
    s_app_led.draw          = app_led_draw;
    return &s_app_led;
}