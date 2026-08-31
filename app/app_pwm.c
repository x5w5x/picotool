#include "app_pwm.h"
#include "app_shared_ram.h" // ⚡ 引入共享内存头文件
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "ui_stepper.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

// ⚡ 依据原理图 H1 排针配置：Pin 9 对应 GP4
#define PIN_PWM_OUT     4 

enum {
    TAB_PWM_SERVO   = 0, // 舵机专用测试
    TAB_PWM_GENERAL = 1, // 通用频率/占空比发生器
    TAB_PWM_PINOUT  = 2, // 接线与安全说明
    TAB_PWM_COUNT   = 3
};

static const char *s_pwm_tabs[] = {
    "舵机控制",
    "通用PWM",
    "接线说明"
};

typedef struct {
    ui_rect_t       dirty_rect;     // ⚠️ 必须放在第 1 位
    ui_tabview_t    tabview;        // 侧边栏
    uint8_t         last_tab_idx;   // 上次 Tab 索引

    uint            slice_num;
    uint            chan_num;

    // Tab 0: 舵机控制
    bool            servo_enabled;
    int32_t         servo_angle;    // 0 ~ 180 度
    int32_t         servo_pulse_us; // 500 ~ 2500 us
    bool            auto_sweep;     // 自动往复摆动
    int8_t          sweep_dir;      // 摆动方向 (+1 / -1)
    uint32_t        last_sweep_ms;  // 摆动节流计时

    ui_rect_t       servo_card;
    ui_switch_t     sw_servo_en;
    ui_stepper_t    stepper_angle;
    ui_stepper_t    stepper_pulse;
    ui_btn_t        btn_deg_0;
    ui_btn_t        btn_deg_45;
    ui_btn_t        btn_deg_90;
    ui_btn_t        btn_deg_135;
    ui_btn_t        btn_deg_180;
    ui_btn_t        btn_sweep_toggle;

    // Tab 1: 通用 PWM
    bool            gen_enabled;
    int32_t         gen_freq_hz;    // 1 Hz ~ 100000 Hz
    int32_t         gen_duty_pct;   // 0 ~ 100 %

    ui_rect_t       gen_card;
    ui_switch_t     sw_gen_en;
    ui_stepper_t    stepper_gen_freq;
    ui_stepper_t    stepper_gen_duty;
    ui_btn_t        btn_preset_1k;
    ui_btn_t        btn_preset_10k;
    ui_btn_t        btn_preset_50hz;
} app_pwm_data_t;

static app_t          s_app_pwm;

// ⚡ 防级联递归与重入锁
static bool           s_in_stepper_sync = false;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 1. ⚡ 动态自适应时钟底层 PWM 硬件驱动 ================= */

static void pwm_update_hardware_servo(app_pwm_data_t *data) {
    if (!data) return;
    if (!data->servo_enabled) {
        pwm_set_enabled(data->slice_num, false);
        return;
    }

    float div = (float)clock_get_hz(clk_sys) / 1000000.0f;
    if (div > 255.0f) div = 255.0f;
    if (div < 1.0f) div = 1.0f;

    pwm_set_clkdiv(data->slice_num, div);
    pwm_set_wrap(data->slice_num, 19999);
    pwm_set_chan_level(data->slice_num, data->chan_num, (uint16_t)data->servo_pulse_us);
    pwm_set_enabled(data->slice_num, true);
}

static void pwm_update_hardware_general(app_pwm_data_t *data) {
    if (!data) return;
    if (!data->gen_enabled) {
        pwm_set_enabled(data->slice_num, false);
        return;
    }

    uint32_t f_sys = clock_get_hz(clk_sys);
    uint32_t f_target = (uint32_t)data->gen_freq_hz;
    if (f_target < 1) f_target = 1;

    float div = (float)f_sys / ((float)f_target * 65535.0f);
    if (div < 1.0f) {
        div = 1.0f;
    } else if (div > 255.0f) {
        div = 255.0f;
    }

    uint32_t wrap = (uint32_t)((float)f_sys / (div * (float)f_target)) - 1;
    if (wrap > 65535) wrap = 65535;
    if (wrap < 1) wrap = 1;

    uint32_t level = ((wrap + 1) * (uint32_t)data->gen_duty_pct) / 100;

    pwm_set_clkdiv(data->slice_num, div);
    pwm_set_wrap(data->slice_num, (uint16_t)wrap);
    pwm_set_chan_level(data->slice_num, data->chan_num, (uint16_t)level);
    pwm_set_enabled(data->slice_num, true);
}

/* ================= 2. 交互回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;

    if (new_tab == TAB_PWM_SERVO) {
        data->gen_enabled = false;
        ui_switch_set_state(&data->sw_gen_en, false);
        pwm_update_hardware_servo(data);
    } else if (new_tab == TAB_PWM_GENERAL) {
        data->servo_enabled = false;
        data->auto_sweep = false;
        ui_switch_set_state(&data->sw_servo_en, false);
        data->btn_sweep_toggle.text = "自动摆动: 关";
        ui_btn_set_color(&data->btn_sweep_toggle, 0x03DF, 0x0275, 0xFFFF);
        pwm_update_hardware_general(data);
    } else {
        pwm_set_enabled(data->slice_num, false);
    }
}

static void on_servo_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->servo_enabled = state;
    if (!state) data->auto_sweep = false;
    pwm_update_hardware_servo(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_angle_stepper_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    if (s_in_stepper_sync) return;
    s_in_stepper_sync = true;

    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (data) {
        data->servo_angle = val;
        data->servo_pulse_us = 500 + (val * 2000 + 90) / 180;
        ui_stepper_set_val(&data->stepper_pulse, data->servo_pulse_us);

        pwm_update_hardware_servo(data);
        data->dirty_rect = data->tabview.content_rect;
    }
    s_in_stepper_sync = false;
}

static void on_pulse_stepper_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    if (s_in_stepper_sync) return;
    s_in_stepper_sync = true;

    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (data) {
        data->servo_pulse_us = val;
        data->servo_angle = ((val - 500) * 180 + 1000) / 2000;
        ui_stepper_set_val(&data->stepper_angle, data->servo_angle);

        pwm_update_hardware_servo(data);
        data->dirty_rect = data->tabview.content_rect;
    }
    s_in_stepper_sync = false;
}

static void on_quick_angle_click(ui_btn_t *btn, void *user_data) {
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->auto_sweep = false;

    if (btn == &data->btn_deg_0)        data->servo_angle = 0;
    else if (btn == &data->btn_deg_45)  data->servo_angle = 45;
    else if (btn == &data->btn_deg_90)  data->servo_angle = 90;
    else if (btn == &data->btn_deg_135) data->servo_angle = 135;
    else if (btn == &data->btn_deg_180) data->servo_angle = 180;

    data->servo_pulse_us = 500 + (data->servo_angle * 2000 + 90) / 180;

    s_in_stepper_sync = true;
    ui_stepper_set_val(&data->stepper_angle, data->servo_angle);
    ui_stepper_set_val(&data->stepper_pulse, data->servo_pulse_us);
    s_in_stepper_sync = false;

    pwm_update_hardware_servo(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_sweep_toggle_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->auto_sweep = !data->auto_sweep;

    if (data->auto_sweep) {
        if (!data->servo_enabled) {
            data->servo_enabled = true;
            ui_switch_set_state(&data->sw_servo_en, true);
        }
        data->btn_sweep_toggle.text = "自动摆动: 开";
        ui_btn_set_color(&data->btn_sweep_toggle, 0xF800, 0xB000, 0xFFFF);
    } else {
        data->btn_sweep_toggle.text = "自动摆动: 关";
        ui_btn_set_color(&data->btn_sweep_toggle, 0x03DF, 0x0275, 0xFFFF);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_gen_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->gen_enabled = state;
    pwm_update_hardware_general(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_gen_freq_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->gen_freq_hz = val;
    pwm_update_hardware_general(data);
    data->dirty_rect = data->gen_card;
}

static void on_gen_duty_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    data->gen_duty_pct = val;
    pwm_update_hardware_general(data);
    data->dirty_rect = data->gen_card;
}

static void on_preset_click(ui_btn_t *btn, void *user_data) {
    app_pwm_data_t *data = (app_pwm_data_t *)user_data;
    if (!data) return;
    if (btn == &data->btn_preset_1k) {
        data->gen_freq_hz = 1000;
        data->gen_duty_pct = 50;
    } else if (btn == &data->btn_preset_10k) {
        data->gen_freq_hz = 10000;
        data->gen_duty_pct = 50;
    } else if (btn == &data->btn_preset_50hz) {
        data->gen_freq_hz = 50;
        data->gen_duty_pct = 50;
    }

    ui_stepper_set_val(&data->stepper_gen_freq, data->gen_freq_hz);
    ui_stepper_set_val(&data->stepper_gen_duty, data->gen_duty_pct);
    pwm_update_hardware_general(data);
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= 3. 生命周期管理 ================= */

static void app_pwm_init(app_t *self) {
    // ⚡ 仅绑定共享静态池指针
    self->user_data = g_static_app_ram;
}

static void app_pwm_enter(app_t *self) {
    // ⚡ 进入 App 时从 8KB 静态池就地初始化
    app_pwm_data_t *data = (app_pwm_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_pwm_data_t));

    data->dirty_rect     = ui_rect_make(0, 0, 0, 0);
    data->last_tab_idx   = 0;
    data->slice_num      = pwm_gpio_to_slice_num(PIN_PWM_OUT);
    data->chan_num       = pwm_gpio_to_channel(PIN_PWM_OUT);

    data->servo_enabled  = false;
    data->servo_angle    = 90;
    data->servo_pulse_us = 1500;
    data->auto_sweep     = false;
    data->sweep_dir      = 1;

    data->gen_enabled    = false;
    data->gen_freq_hz    = 1000;
    data->gen_duty_pct   = 50;

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_pwm_tabs, TAB_PWM_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0: 舵机控制组件
    data->servo_card = ui_rect_make(120, 42, 350, 76);
    ui_switch_init(&data->sw_servo_en, 396, 48, 64, 32, false, on_servo_switch);
    data->sw_servo_en.user_data = data;

    ui_stepper_init(&data->stepper_angle, 120, 126, 170, 34, 0, 180, 90, 5, "度", on_angle_stepper_change);
    ui_stepper_set_prefix(&data->stepper_angle, "角度:");
    data->stepper_angle.user_data = data;

    ui_stepper_init(&data->stepper_pulse, 298, 126, 172, 34, 500, 2500, 1500, 50, "us", on_pulse_stepper_change);
    ui_stepper_set_prefix(&data->stepper_pulse, "脉宽:");
    data->stepper_pulse.user_data = data;

    ui_btn_init(&data->btn_deg_0,   120, 170, 65, 34, "0度",   on_quick_angle_click);
    data->btn_deg_0.user_data = data; data->btn_deg_0.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_deg_0, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_deg_45,  190, 170, 65, 34, "45度",  on_quick_angle_click);
    data->btn_deg_45.user_data = data; data->btn_deg_45.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_deg_45, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_deg_90,  260, 170, 70, 34, "90度中", on_quick_angle_click);
    data->btn_deg_90.user_data = data; data->btn_deg_90.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_deg_90, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_deg_135, 335, 170, 65, 34, "135度", on_quick_angle_click);
    data->btn_deg_135.user_data = data; data->btn_deg_135.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_deg_135, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_deg_180, 405, 170, 65, 34, "180度", on_quick_angle_click);
    data->btn_deg_180.user_data = data; data->btn_deg_180.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_deg_180, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_sweep_toggle, 120, 214, 350, 36, "自动摆动: 关", on_sweep_toggle_click);
    data->btn_sweep_toggle.user_data = data;
    data->btn_sweep_toggle.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_sweep_toggle, 0x03DF, 0x0275, 0xFFFF);

    // Tab 1: 通用 PWM 组件
    data->gen_card = ui_rect_make(120, 42, 350, 76);
    ui_switch_init(&data->sw_gen_en, 396, 48, 64, 32, false, on_gen_switch);
    data->sw_gen_en.user_data = data;

    ui_stepper_init(&data->stepper_gen_freq, 120, 130, 210, 36, 1, 100000, 1000, 100, "Hz", on_gen_freq_change);
    ui_stepper_set_prefix(&data->stepper_gen_freq, "频率:");
    data->stepper_gen_freq.user_data = data;

    ui_stepper_init(&data->stepper_gen_duty, 338, 130, 132, 36, 0, 100, 50, 5, "%", on_gen_duty_change);
    ui_stepper_set_prefix(&data->stepper_gen_duty, "占空:");
    data->stepper_gen_duty.user_data = data;

    ui_btn_init(&data->btn_preset_1k, 120, 180, 110, 36, "1kHz 50%", on_preset_click);
    data->btn_preset_1k.user_data = data; data->btn_preset_1k.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_preset_1k, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_preset_10k, 240, 180, 110, 36, "10kHz 50%", on_preset_click);
    data->btn_preset_10k.user_data = data; data->btn_preset_10k.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_preset_10k, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_preset_50hz, 360, 180, 110, 36, "50Hz 50%", on_preset_click);
    data->btn_preset_50hz.user_data = data; data->btn_preset_50hz.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_preset_50hz, 0x05E0, 0x03E0, 0xFFFF);

    gpio_set_function(PIN_PWM_OUT, GPIO_FUNC_PWM);
    data->last_tab_idx = data->tabview.active_tab_idx;

    if (data->tabview.active_tab_idx == TAB_PWM_SERVO) {
        pwm_update_hardware_servo(data);
    } else if (data->tabview.active_tab_idx == TAB_PWM_GENERAL) {
        pwm_update_hardware_general(data);
    }
}

static void app_pwm_exit(app_t *self) {
    app_pwm_data_t *data = (app_pwm_data_t *)self->user_data;
    if (data) {
        data->servo_enabled = false;
        data->gen_enabled = false;
        data->auto_sweep = false;

        pwm_set_enabled(data->slice_num, false);
        gpio_set_function(PIN_PWM_OUT, GPIO_FUNC_NULL);
        gpio_set_dir(PIN_PWM_OUT, GPIO_IN);
    }
    self->user_data = NULL;
}

/* ================= 4. 自动摆动步进更新 ================= */

static void app_pwm_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_pwm_data_t *data = (app_pwm_data_t *)self->user_data;
    if (!data) return;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 自动往复摆动 (每 20ms 步进 1 度)
    if (data->tabview.active_tab_idx == TAB_PWM_SERVO && data->auto_sweep && data->servo_enabled) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - data->last_sweep_ms >= 20) {
            data->last_sweep_ms = now_ms;

            data->servo_angle += data->sweep_dir;
            if (data->servo_angle >= 180) {
                data->servo_angle = 180;
                data->sweep_dir = -1;
            } else if (data->servo_angle <= 0) {
                data->servo_angle = 0;
                data->sweep_dir = 1;
            }

            data->servo_pulse_us = 500 + (data->servo_angle * 2000 + 90) / 180;

            s_in_stepper_sync = true;
            ui_stepper_set_val(&data->stepper_angle, data->servo_angle);
            ui_stepper_set_val(&data->stepper_pulse, data->servo_pulse_us);
            s_in_stepper_sync = false;

            pwm_update_hardware_servo(data);
            data->dirty_rect = data->tabview.content_rect;
        }
    }
}

/* ================= 5. 触控分发 ================= */

static bool app_pwm_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_pwm_data_t *data = (app_pwm_data_t *)self->user_data;
    if (!data) return false;

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    switch (data->tabview.active_tab_idx) {
        case TAB_PWM_SERVO: {
            if (ui_switch_process_touch(&data->sw_servo_en, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_servo_en));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_angle, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_angle));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_pulse, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_pulse));
                return true;
            }
            if (ui_btn_process_touch(&data->btn_deg_0, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_deg_45, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_deg_90, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_deg_135, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_deg_180, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_sweep_toggle, is_touching, tx, ty, now_tick)) return true;
            break;
        }

        case TAB_PWM_GENERAL: {
            if (ui_switch_process_touch(&data->sw_gen_en, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_gen_en));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_gen_freq, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_gen_freq));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_gen_duty, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_gen_duty));
                return true;
            }
            if (ui_btn_process_touch(&data->btn_preset_1k, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_preset_10k, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_preset_50hz, is_touching, tx, ty, now_tick)) return true;
            break;
        }
    }

    return (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* ================= 6. 切片光栅化渲染 ================= */

static void app_pwm_draw(app_t *self, ui_tile_t *tile) {
    app_pwm_data_t *data = (app_pwm_data_t *)self->user_data;
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

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case TAB_PWM_SERVO: {
            if (ui_rect_intersect(&tile->rect, &data->servo_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->servo_card, 0xC618);

                ui_draw_string(tile, 135, 50, "标准 RC 舵机 (50Hz / 20ms)", FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "当前角度: %ld 度  |  脉宽: %ld us", (long)data->servo_angle, (long)data->servo_pulse_us);
                ui_draw_string(tile, 135, 74, str_buf, FONT_16, 0x03DF, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "输出状态: %s (H1 Pin 9 / GP4)", data->servo_enabled ? "已开启" : "已待机");
                ui_draw_string(tile, 135, 96, str_buf, FONT_16, data->servo_enabled ? 0x05E0 : 0x8410, 0xEF7D);
            }

            ui_switch_draw(tile, &data->sw_servo_en);
            ui_stepper_draw(tile, &data->stepper_angle);
            ui_stepper_draw(tile, &data->stepper_pulse);

            ui_btn_draw(tile, &data->btn_deg_0);
            ui_btn_draw(tile, &data->btn_deg_45);
            ui_btn_draw(tile, &data->btn_deg_90);
            ui_btn_draw(tile, &data->btn_deg_135);
            ui_btn_draw(tile, &data->btn_deg_180);
            ui_btn_draw(tile, &data->btn_sweep_toggle);
            break;
        }

        case TAB_PWM_GENERAL: {
            if (ui_rect_intersect(&tile->rect, &data->gen_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->gen_card, 0x07FF);

                ui_draw_string(tile, 135, 50, "通用硬件 PWM 信号发生器", FONT_16, 0x07FF, 0x10A2);

                snprintf(str_buf, sizeof(str_buf), "频率: %ld Hz  |  占空比: %ld %%", (long)data->gen_freq_hz, (long)data->gen_duty_pct);
                ui_draw_string(tile, 135, 74, str_buf, FONT_16, 0x05E0, 0x10A2);

                snprintf(str_buf, sizeof(str_buf), "输出通道: [ H1 Pin 9 / GP4 ] 状态: %s", data->gen_enabled ? "ON" : "OFF");
                ui_draw_string(tile, 135, 96, str_buf, FONT_16, data->gen_enabled ? 0xFFFF : 0x8410, 0x10A2);
            }

            ui_switch_draw(tile, &data->sw_gen_en);
            ui_stepper_draw(tile, &data->stepper_gen_freq);
            ui_stepper_draw(tile, &data->stepper_gen_duty);

            ui_btn_draw(tile, &data->btn_preset_1k);
            ui_btn_draw(tile, &data->btn_preset_10k);
            ui_btn_draw(tile, &data->btn_preset_50hz);
            break;
        }

        case TAB_PWM_PINOUT: {
            ui_rect_t card = ui_rect_make(120, 42, 350, 240);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);

                ui_draw_string(tile, 135, 54, "H1 排针 舵机三线接线指南:", FONT_16, 0x03DF, 0xEF7D);
                ui_draw_string(tile, 135, 80, "Pin 9 (GP4)  -> 信号线 (橙色/黄色)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 106, "Pin 3 (GND)  -> 地线   (棕色/黑色)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 132, "Pin 1 (VBUS) -> 5V电源 (红色)", FONT_16, 0xF800, 0xEF7D);
                ui_draw_string(tile, 135, 168, "安全警告与提示:", FONT_16, 0xF800, 0xEF7D);
                ui_draw_string(tile, 135, 194, "大扭矩舵机建议使用外部 5V 独立供电!", FONT_16, 0xF800, 0xEF7D);
                ui_draw_string(tile, 135, 218, "外部供电必须与 Pico 共地 (接 H1 Pin 3)", FONT_16, 0x8410, 0xEF7D);
            }
            break;
        }
    }
}

/* ================= 7. 导出单例实体 ================= */

app_t *app_pwm_get_instance(void) {
    s_app_pwm.id            = APP_ID_CUSTOM_0 + 3;
    s_app_pwm.name          = "PWM发生";
    s_app_pwm.init          = app_pwm_init;
    s_app_pwm.enter         = app_pwm_enter;
    s_app_pwm.exit          = app_pwm_exit;
    s_app_pwm.update        = app_pwm_update;
    s_app_pwm.process_touch = app_pwm_process_touch;
    s_app_pwm.draw          = app_pwm_draw;
    return &s_app_pwm;
}