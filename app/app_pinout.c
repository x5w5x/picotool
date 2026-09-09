#include "app_pinout.h"
#include "app_shared_ram.h" // ⚡ 引入共享内存头文件
#include "app_manager.h"
#include "ui_draw.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#define COLS_COUNT      10
#define TOTAL_PINS      20

typedef enum {
    PIN_TYPE_VBUS, // 5V 红
    PIN_TYPE_3V3,  // 3.3V 橙
    PIN_TYPE_GND,  // GND 黑
    PIN_TYPE_GPIO, // 数字 IO 蓝
    PIN_TYPE_ADC   // 模拟 ADC 绿
} pin_kind_t;

typedef struct {
    uint8_t     pin_num;   // 1 ~ 20
    const char *name;      // "VBUS", "GP4", "GND"
    const char *desc;      // 详细复用说明
    pin_kind_t  kind;
} pin_desc_t;

// 逆时针旋转 90 度后的上排 (偶数引脚: Pin 2, 4, 6 ... 20)
static const pin_desc_t s_top_row_pins[COLS_COUNT] = {
    { 2,  "VBUS", "USB 5V 电源输出 / 输入 (最大 500mA)",            PIN_TYPE_VBUS },
    { 4,  "3.3V",  "系统 3.3V 主电源输出 (最大 300mA)",             PIN_TYPE_3V3  },
    { 6,  "GND",  "系统电源地 (Ground)",                            PIN_TYPE_GND },
    { 8,  "GP3",  "数字IO | SWDIO / SPI0_TX / I2C1_SCL",            PIN_TYPE_GPIO },
    { 10, "GP5",  "数字IO | I2C0_SCL / UART1_RX / PWM4B",           PIN_TYPE_GPIO },
    { 12, "GP7",  "数字IO | SPI0_TX / I2C1_SCL / PWM6B",            PIN_TYPE_GPIO },
    { 14, "GP21", "数字IO | I2C0_SCL / UART1_RX / PWM4B",           PIN_TYPE_GPIO },
    { 16, "GND",  "系统电源地 (Ground)",                            PIN_TYPE_GND  },
    { 18, "GP27", "模拟IO | ADC1 模拟输入通道 / PWM10B",            PIN_TYPE_ADC  },
    { 20, "GND",  "系统电源地 (Ground)",                            PIN_TYPE_GND  },
};

// 逆时针旋转 90 度后的下排 (奇数引脚: Pin 1, 3, 5 ... 19)
static const pin_desc_t s_bot_row_pins[COLS_COUNT] = {
    { 1,  "VBUS", "USB 5V 电源输出 / 输入 (最大 500mA)",           PIN_TYPE_VBUS },
    { 3,  "3.3V",  "系统 3.3V 主电源输出 (最大 300mA)",            PIN_TYPE_3V3  },
    { 5,  "GND",  "系统电源地 (Ground)",                           PIN_TYPE_GND },
    { 7,  "GP2",  "数字IO | SWCLK / SPI0_SCK / I2C1_SDA",          PIN_TYPE_GPIO },
    { 9,  "GP4",  "数字IO | I2C0_SDA / UART1_TX / PWM4A",          PIN_TYPE_GPIO },
    { 11, "GP6",  "数字IO | SPI0_SCK / I2C1_SDA / PWM6A",          PIN_TYPE_GPIO },
    { 13, "GP20", "数字IO | I2C0_SDA / UART1_TX / PWM4A",          PIN_TYPE_GPIO },
    { 15, "GP22", "数字IO | SPI0_SCK / I2C1_SDA / PWM6A",          PIN_TYPE_GPIO },
    { 17, "GP26", "模拟IO | ADC0 模拟输入通道 / PWM10A",            PIN_TYPE_ADC  },
    { 19, "GP28", "模拟IO | ADC2 模拟输入通道 / PWM11A",            PIN_TYPE_ADC  },
};

typedef struct {
    ui_rect_t   dirty_rect;
    int8_t      selected_pin_num; // 1 ~ 20 (0 为未选中)
} app_pinout_data_t;

static app_t             s_app_pinout;

/* ================= 1. 引脚颜色配置 ================= */

static inline void get_pin_colors(pin_kind_t kind, bool is_selected, uint16_t *bg, uint16_t *border, uint16_t *text) {
    if (is_selected) {
        *bg     = 0xFFE0; // 选中高亮明黄色
        *border = 0xFFFF;
        *text   = 0x0000;
        return;
    }

    switch (kind) {
        case PIN_TYPE_VBUS: // 5V VBUS 鲜红色
            *bg     = 0xF800;
            *border = 0xFC10;
            *text   = 0xFFFF;
            break;

        case PIN_TYPE_3V3:  // 3.3V 橙红色
            *bg     = 0xFD00;
            *border = 0xFDE0;
            *text   = 0xFFFF;
            break;

        case PIN_TYPE_GND:  // GND 纯黑 + 银灰框
            *bg     = 0x0000;
            *border = 0x8410;
            *text   = 0xCE59;
            break;

        case PIN_TYPE_ADC:  // ADC 模拟 翠绿色
            *bg     = 0x05E0;
            *border = 0x27E8;
            *text   = 0xFFFF;
            break;

        case PIN_TYPE_GPIO: // 数字 GPIO 湖蓝色
        default:
            *bg     = 0x03DF;
            *border = 0x5DFB;
            *text   = 0xFFFF;
            break;
    }
}

/* ================= 2. 生命周期与触控 ================= */

static void app_pinout_init(app_t *self) {
    // ⚡ 仅绑定共享静态池指针
    self->user_data = g_static_app_ram;
}

static void app_pinout_enter(app_t *self) {
    // ⚡ 进入 App 时从 8KB 静态池就地初始化
    app_pinout_data_t *data = (app_pinout_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_pinout_data_t));

    data->dirty_rect        = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->selected_pin_num  = 0;
}

static void app_pinout_exit(app_t *self) {
    self->user_data = NULL;
}

static void app_pinout_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_pinout_data_t *data = (app_pinout_data_t *)self->user_data;
    if (data) data->dirty_rect = ui_rect_make(0, 0, 0, 0);
}

static bool app_pinout_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    app_pinout_data_t *data = (app_pinout_data_t *)self->user_data;
    if (!data) return false;

    if (!is_touching) return false;

    // 10 列水平触控检测 (X: 12 ~ 468)
    for (int col = 0; col < COLS_COUNT; col++) {
        int16_t x = 12 + col * 46;
        if (tx >= x && tx <= x + 42) {
            // 上排引脚触摸 (Y: 65 ~ 145 -> Pin 2, 4, 6 ... 20)
            if (ty >= 65 && ty <= 145) {
                data->selected_pin_num = s_top_row_pins[col].pin_num;
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                return true;
            }
            // 下排引脚触摸 (Y: 146 ~ 226 -> Pin 1, 3, 5 ... 19)
            if (ty >= 146 && ty <= 226) {
                data->selected_pin_num = s_bot_row_pins[col].pin_num;
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                return true;
            }
        }
    }

    return false;
}

/* ================= 3. 切片光栅化渲染 (横向 2x10 排针) ================= */

static void app_pinout_draw(app_t *self, ui_tile_t *tile) {
    app_pinout_data_t *data = (app_pinout_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    // 背景底色 (深邃科技黑蓝)
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x10A2);
    }

    char str_buf[64];

    // 1. 顶部引脚复用详情栏 (Y: 36 ~ 58)
    ui_rect_t info_bar = ui_rect_make(12, 36, 456, 22);
    if (ui_rect_intersect(&tile->rect, &info_bar, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x0000);
        ui_draw_rect(tile, &info_bar, 0x4208);

        if (data->selected_pin_num > 0) {
            const pin_desc_t *p = NULL;
            for (int i = 0; i < COLS_COUNT; i++) {
                if (s_top_row_pins[i].pin_num == data->selected_pin_num) { p = &s_top_row_pins[i]; break; }
                if (s_bot_row_pins[i].pin_num == data->selected_pin_num) { p = &s_bot_row_pins[i]; break; }
            }
            if (p) {
                snprintf(str_buf, sizeof(str_buf), "Pin %02u [%s]: %s", p->pin_num, p->name, p->desc);
                ui_draw_string(tile, 18, 40, str_buf, FONT_16, 0x07FF, 0x0000);
            }
        } else {
            ui_draw_string(tile, 18, 40, "H1 扩展排针 (水平布局) [轻触引脚可查看详细硬件复用定义]", FONT_16, 0xCE59, 0x0000);
        }
    }

    // 2. 中间 2x10 物理排针主体座 (Y: 114 ~ 176)
    ui_rect_t mid_socket = ui_rect_make(10, 114, 460, 62);
    if (ui_rect_intersect(&tile->rect, &mid_socket, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x2124); // 黑色塑料排针座
        ui_draw_rect(tile, &mid_socket, 0xCE59);  // 丝印白色外框
    }

    // 3. 绘制 10 列引脚与标签
    for (int col = 0; col < COLS_COUNT; col++) {
        int16_t x = 12 + col * 46;

        const pin_desc_t *p_top = &s_top_row_pins[col];
        const pin_desc_t *p_bot = &s_bot_row_pins[col];

        bool top_sel = (data->selected_pin_num == p_top->pin_num);
        bool bot_sel = (data->selected_pin_num == p_bot->pin_num);

        uint16_t bg_t, border_t, txt_t;
        uint16_t bg_b, border_b, txt_b;
        get_pin_colors(p_top->kind, top_sel, &bg_t, &border_t, &txt_t);
        get_pin_colors(p_bot->kind, bot_sel, &bg_b, &border_b, &txt_b);

        /* ----- A. 上排引脚 (偶数: Pin 2, 4, 6 ... 20) ----- */
        // 上标签
        ui_rect_t r_lbl_t = ui_rect_make(x, 68, 42, 42);
        if (ui_rect_intersect(&tile->rect, &r_lbl_t, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, bg_t);
            ui_draw_rect(tile, &r_lbl_t, border_t);

            // 居中绘制名称
            int16_t offset_x = (strlen(p_top->name) <= 3) ? 9 : 5;
            ui_draw_string(tile, x + offset_x, 81, p_top->name, FONT_16, txt_t, bg_t);
        }

        // 上排排针插孔与编号
        ui_rect_t r_hole_t = ui_rect_make(x + 3, 120, 36, 24);
        if (ui_rect_intersect(&tile->rect, &r_hole_t, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0x0000);
            ui_draw_rect(tile, &r_hole_t, top_sel ? 0xFFE0 : 0x632C);

            snprintf(str_buf, sizeof(str_buf), "%2u", p_top->pin_num);
            ui_draw_string(tile, x + 12, 124, str_buf, FONT_16, top_sel ? 0xFFE0 : 0xFFFF, 0x0000);
        }

        /* ----- B. 下排引脚 (奇数: Pin 1, 3, 5 ... 19) ----- */
        // 下排排针插孔与编号
        ui_rect_t r_hole_b = ui_rect_make(x + 3, 148, 36, 24);
        if (ui_rect_intersect(&tile->rect, &r_hole_b, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0x0000);
            ui_draw_rect(tile, &r_hole_b, bot_sel ? 0xFFE0 : 0x632C);

            // Pin 1 专属红色极性圆点标记 (1:1 还原原理图)
            if (p_bot->pin_num == 1) {
                ui_rect_t dot = ui_rect_make(x + 5, 156, 6, 6);
                ui_draw_fill_rect(tile, &dot, 0xF800);
            }

            snprintf(str_buf, sizeof(str_buf), "%2u", p_bot->pin_num);
            ui_draw_string(tile, x + 12, 152, str_buf, FONT_16, bot_sel ? 0xFFE0 : 0xFFFF, 0x0000);
        }

        // 下标签
        ui_rect_t r_lbl_b = ui_rect_make(x, 180, 42, 42);
        if (ui_rect_intersect(&tile->rect, &r_lbl_b, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, bg_b);
            ui_draw_rect(tile, &r_lbl_b, border_b);

            int16_t offset_x = (strlen(p_bot->name) <= 3) ? 9 : 5;
            ui_draw_string(tile, x + offset_x, 193, p_bot->name, FONT_16, txt_b, bg_b);
        }
    }

    // 4. 底部引脚类别颜色图例 (Y: 236 ~ 308)
    ui_rect_t legend_card = ui_rect_make(12, 236, 456, 72);
    if (ui_rect_intersect(&tile->rect, &legend_card, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0x0841);
        ui_draw_rect(tile, &legend_card, 0x2965);

        // 图例色块 1: 5V VBUS
        ui_rect_t b1 = ui_rect_make(24, 246, 16, 16); ui_draw_fill_rect(tile, &b1, 0xF800);
        ui_draw_string(tile, 46, 246, "5V(VBUS)", FONT_16, 0xFFFF, 0x0841);

        // 图例色块 2: 3.3V
        ui_rect_t b2 = ui_rect_make(136, 246, 16, 16); ui_draw_fill_rect(tile, &b2, 0xFD00);
        ui_draw_string(tile, 158, 246, "3.3V", FONT_16, 0xFFFF, 0x0841);

        // 图例色块 3: GND
        ui_rect_t b3 = ui_rect_make(216, 246, 16, 16); ui_draw_fill_rect(tile, &b3, 0x0000);
        ui_draw_rect(tile, &b3, 0x8410);
        ui_draw_string(tile, 238, 246, "GND(地)", FONT_16, 0xFFFF, 0x0841);

        // 图例色块 4: GPIO
        ui_rect_t b4 = ui_rect_make(316, 246, 16, 16); ui_draw_fill_rect(tile, &b4, 0x03DF);
        ui_draw_string(tile, 338, 246, "GPIO(数字)", FONT_16, 0xFFFF, 0x0841);

        // 图例色块 5: ADC
        ui_rect_t b5 = ui_rect_make(24, 276, 16, 16); ui_draw_fill_rect(tile, &b5, 0x05E0);
        ui_draw_string(tile, 46, 276, "ADC(模拟输入 GP26~28)", FONT_16, 0xFFFF, 0x0841);

        // 丝印型号
        ui_draw_string(tile, 248, 276, "HC-PZ254-2x10PWA (H1)", FONT_16, 0x07FF, 0x0841);
    }
}

/* ================= 4. 导出单例实体 ================= */

app_t *app_pinout_get_instance(void) {
    s_app_pinout.id            = APP_ID_CUSTOM_0 + 5;
    s_app_pinout.name          = "排针引脚";
    s_app_pinout.init          = app_pinout_init;
    s_app_pinout.enter         = app_pinout_enter;
    s_app_pinout.exit          = app_pinout_exit;
    s_app_pinout.update        = app_pinout_update;
    s_app_pinout.process_touch = app_pinout_process_touch;
    s_app_pinout.draw          = app_pinout_draw;
    return &s_app_pinout;
}