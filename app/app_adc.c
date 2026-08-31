#include "app_adc.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_tabview.h"
#include "ui_waveform.h"
#include "ui_stepper.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

#define ADC_WAVE_SAMPLES    350 // 示波器帧点数 (与 350px 视口严格 1:1)

static const char *s_adc_tabs[] = {
    "实时监测",
    "波形示波",
    "极值统计",
    "引脚说明"
};
#define TAB_COUNT (sizeof(s_adc_tabs) / sizeof(s_adc_tabs[0]))

// ADC 通道定义
enum {
    ADC_CH_0    = 0, // GP26
    ADC_CH_1    = 1, // GP27
    ADC_CH_2    = 2, // GP28
    ADC_CH_TEMP = 4  // 内部芯片温度
};

typedef struct {
    uint16_t raw[5];
    float    voltage[5];
    float    chip_temp_c;

    float    v_min;
    float    v_max;
    float    v_avg;
    float    v_pp;
    uint32_t sample_cnt;
} adc_state_t;

/* ================= 1. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t          dirty_rect;       // ⚠️ 必须放在第 1 位
    ui_tabview_t       tabview;          // 侧边栏
    uint8_t            last_tab_idx;     // 上次 Tab 索引

    adc_state_t        state;
    uint32_t           last_ui_frame_ms; // 30 FPS 刷新节流

    // ⚡ DMA 高速采样架构
    int                dma_chan;
    uint16_t           dma_raw_buf[ADC_WAVE_SAMPLES] __attribute__((aligned(4)));

    // Tab 0: 仪表卡片
    ui_rect_t          monitor_cards[4];

    // Tab 1: 模拟示波器与步进微调器
    uint8_t            active_wave_ch;   // 当前示波通道 (0, 1, 2, 4)
    bool               wave_running;     // 运行/暂停
    int32_t            cur_freq_khz;     // 采样率 (默认 250 kHz)
    int32_t            cur_gain;         // 放大倍率 (1 ~ 8x)
    int16_t            analog_buf[ADC_WAVE_SAMPLES]; // 渲染缓冲区
    ui_waveform_t      waveform;
    ui_btn_t           btn_wave_toggle;  // 运行/暂停
    ui_stepper_t       stepper_freq;     // 采样率步进器
    ui_stepper_t       stepper_gain;     // 增益步进器
    ui_btn_t           btn_wave_ch0;     // CH0
    ui_btn_t           btn_wave_ch1;     // CH1
    ui_btn_t           btn_wave_ch2;     // CH2
    ui_btn_t           btn_wave_temp;    // Temp

    // Tab 2: 统计看板
    ui_rect_t          stats_card;
    ui_btn_t           btn_reset_stats;
} app_adc_data_t;

static app_t s_app_adc;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 2. 安全 DMA 流控与采集管理 ================= */

static void adc_stop_all(app_adc_data_t *data) {
    adc_run(false);
    if (data && data->dma_chan >= 0) {
        dma_channel_abort(data->dma_chan);
    }
    adc_fifo_drain();
}

static void adc_trigger_one_dma_frame(app_adc_data_t *data) {
    if (!data || data->dma_chan < 0) return;
    adc_stop_all(data);

    adc_select_input(data->active_wave_ch);

    // 计算分频: 48MHz / (1 + div) = target_hz
    uint32_t target_hz = (uint32_t)data->cur_freq_khz * 1000UL;
    if (target_hz < 1000UL) target_hz = 1000UL;
    if (target_hz >= 500000UL) {
        adc_set_clkdiv(0.0f);
        data->waveform.sample_rate_hz = 500000;
    } else {
        float div = (48000000.0f / (float)target_hz) - 1.0f;
        if (div < 0.0f) div = 0.0f;
        adc_set_clkdiv(div);
        data->waveform.sample_rate_hz = target_hz;
    }

    adc_fifo_setup(true, true, 1, false, false);

    // 装载并启动单次 350 点传输
    dma_channel_set_read_addr(data->dma_chan, &adc_hw->fifo, false);
    dma_channel_set_write_addr(data->dma_chan, data->dma_raw_buf, false);
    dma_channel_set_trans_count(data->dma_chan, ADC_WAVE_SAMPLES, true);

    adc_run(true);
}

// Tab 0 安全的多通道轮询
static void adc_sample_sync_tab0(app_adc_data_t *data) {
    if (!data) return;
    adc_stop_all(data);
    adc_fifo_setup(false, false, 0, false, false);

    for (int ch = 0; ch <= 2; ch++) {
        adc_select_input(ch);
        uint32_t acc = 0;
        for (int i = 0; i < 8; i++) acc += adc_read();
        data->state.raw[ch] = (uint16_t)(acc >> 3);
        data->state.voltage[ch] = (float)data->state.raw[ch] * 3.3f / 4095.0f;
    }

    adc_select_input(ADC_CH_TEMP);
    uint32_t temp_acc = 0;
    for (int i = 0; i < 8; i++) temp_acc += adc_read();
    data->state.raw[ADC_CH_TEMP] = (uint16_t)(temp_acc >> 3);
    float temp_v = (float)data->state.raw[ADC_CH_TEMP] * 3.3f / 4095.0f;
    data->state.voltage[ADC_CH_TEMP] = temp_v;
    data->state.chip_temp_c = 27.0f - (temp_v - 0.706f) / 0.001721f;
}

/* ================= 3. 交互回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (!data) return;

    if (new_tab == 1 || new_tab == 2) {
        if (data->wave_running) {
            adc_trigger_one_dma_frame(data);
        }
    } else {
        adc_stop_all(data);
    }
}

static void on_freq_stepper_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (!data) return;
    data->cur_freq_khz = val;
    if (data->wave_running && (data->tabview.active_tab_idx == 1 || data->tabview.active_tab_idx == 2)) {
        adc_trigger_one_dma_frame(data);
    }
}

static void on_gain_stepper_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (data) {
        data->cur_gain = val;
    }
}

static void on_wave_toggle_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (!data) return;
    data->wave_running = !data->wave_running;

    if (data->wave_running) {
        data->btn_wave_toggle.text = "暂停";
        ui_btn_set_color(&data->btn_wave_toggle, 0xF800, 0xB000, 0xFFFF);
        adc_trigger_one_dma_frame(data);
    } else {
        adc_stop_all(data);
        data->btn_wave_toggle.text = "运行";
        ui_btn_set_color(&data->btn_wave_toggle, 0x05E0, 0x03E0, 0xFFFF);
    }
    ui_rect_t b_dirty = ui_btn_get_dirty_rect(&data->btn_wave_toggle);
    data->dirty_rect = rect_union_box(data->dirty_rect, b_dirty);
}

static void on_wave_ch_select(ui_btn_t *btn, void *user_data) {
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (!data) return;

    if (btn == &data->btn_wave_ch0) data->active_wave_ch = ADC_CH_0;
    else if (btn == &data->btn_wave_ch1) data->active_wave_ch = ADC_CH_1;
    else if (btn == &data->btn_wave_ch2) data->active_wave_ch = ADC_CH_2;
    else if (btn == &data->btn_wave_temp) data->active_wave_ch = ADC_CH_TEMP;

    memset(data->analog_buf, 0, sizeof(data->analog_buf));
    ui_waveform_bind_analog(&data->waveform, data->analog_buf, ADC_WAVE_SAMPLES);
    data->state.sample_cnt = 0;

    if (data->wave_running && (data->tabview.active_tab_idx == 1 || data->tabview.active_tab_idx == 2)) {
        adc_trigger_one_dma_frame(data);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_reset_stats_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_adc_data_t *data = (app_adc_data_t *)user_data;
    if (data) {
        data->state.sample_cnt = 0;
        data->dirty_rect = data->stats_card;
    }
}

/* ================= 4. 生命周期管理 ================= */

static void app_adc_init(app_t *self) {
    // ⚡ 仅绑定共享静态池指针
    self->user_data = g_static_app_ram;
}

static void app_adc_enter(app_t *self) {
    // ⚡ 进入 App 时从 8KB 静态池就地初始化
    app_adc_data_t *data = (app_adc_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_adc_data_t));

    data->dirty_rect     = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx   = 0;
    data->active_wave_ch = ADC_CH_0;
    data->wave_running   = true;
    data->cur_freq_khz   = 250; // 默认 250 kHz
    data->cur_gain       = 1;   // 默认 1x
    data->dma_chan       = -1;

    // 1. 初始化 ADC 硬件
    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);
    adc_gpio_init(28);
    adc_set_temp_sensor_enabled(true);

    // 2. 动态申请 DMA 通道
    data->dma_chan = dma_claim_unused_channel(true);
    if (data->dma_chan >= 0) {
        dma_channel_config cfg = dma_channel_get_default_config(data->dma_chan);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
        channel_config_set_read_increment(&cfg, false);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, DREQ_ADC);

        dma_channel_configure(
            data->dma_chan,
            &cfg,
            data->dma_raw_buf,
            &adc_hw->fifo,
            ADC_WAVE_SAMPLES,
            false
        );
    }

    // 3. 侧边栏初始化
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_adc_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // 4. Tab 0 仪表卡片
    data->monitor_cards[0] = ui_rect_make(120, 42,  350, 56);
    data->monitor_cards[1] = ui_rect_make(120, 104, 350, 56);
    data->monitor_cards[2] = ui_rect_make(120, 166, 350, 56);
    data->monitor_cards[3] = ui_rect_make(120, 228, 350, 56);

    // 5. Tab 1: 示波器波形画布
    ui_waveform_init(&data->waveform, 118, 36, 354, 156, UI_WAVE_MODE_ANALOG_1CH, 250000);
    data->waveform.color_bg   = 0x0000;
    data->waveform.color_grid = 0x2104;
    ui_waveform_bind_analog(&data->waveform, data->analog_buf, ADC_WAVE_SAMPLES);

    // 控制行 1: 运行按钮 + 采样率步进器 + 增益步进器
    ui_btn_init(&data->btn_wave_toggle, 118, 198, 50, 32, "暂停", on_wave_toggle_click);
    data->btn_wave_toggle.user_data = data; 
    data->btn_wave_toggle.style = UI_BTN_STYLE_ROUND;
    data->btn_wave_toggle.font_size = FONT_16;
    ui_btn_set_color(&data->btn_wave_toggle, 0xF800, 0xB000, 0xFFFF);

    ui_stepper_init(&data->stepper_freq, 172, 198, 160, 32, 1, 500, 250, 10, "kHz", on_freq_stepper_change);
    ui_stepper_set_prefix(&data->stepper_freq, "Fs:");
    data->stepper_freq.user_data = data;

    ui_stepper_init(&data->stepper_gain, 336, 198, 136, 32, 1, 8, 1, 1, "x", on_gain_stepper_change);
    ui_stepper_set_prefix(&data->stepper_gain, "增益:");
    data->stepper_gain.user_data = data;

    // 控制行 2: 4 通道切换按钮
    ui_btn_init(&data->btn_wave_ch0, 118, 236, 82, 32, "CH0/GP26", on_wave_ch_select);
    data->btn_wave_ch0.user_data = data; data->btn_wave_ch0.style = UI_BTN_STYLE_ROUND;
    data->btn_wave_ch0.font_size = FONT_16;
    ui_btn_set_color(&data->btn_wave_ch0, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_ch1, 206, 236, 82, 32, "CH1/GP27", on_wave_ch_select);
    data->btn_wave_ch1.user_data = data; data->btn_wave_ch1.style = UI_BTN_STYLE_ROUND;
    data->btn_wave_ch1.font_size = FONT_16;
    ui_btn_set_color(&data->btn_wave_ch1, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_ch2, 294, 236, 82, 32, "CH2/GP28", on_wave_ch_select);
    data->btn_wave_ch2.user_data = data; data->btn_wave_ch2.style = UI_BTN_STYLE_ROUND;
    data->btn_wave_ch2.font_size = FONT_16;
    ui_btn_set_color(&data->btn_wave_ch2, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_temp, 382, 236, 90, 32, "内部温度", on_wave_ch_select);
    data->btn_wave_temp.user_data = data; data->btn_wave_temp.style = UI_BTN_STYLE_ROUND;
    data->btn_wave_temp.font_size = FONT_16;
    ui_btn_set_color(&data->btn_wave_temp, 0xFA00, 0xC800, 0xFFFF);

    // 6. Tab 2 统计看板
    data->stats_card = ui_rect_make(120, 42, 350, 180);
    ui_btn_init(&data->btn_reset_stats, 220, 235, 150, 42, "重置统计极值", on_reset_stats_click);
    data->btn_reset_stats.user_data = data; data->btn_reset_stats.style = UI_BTN_STYLE_ROUND;
    data->btn_reset_stats.font_size = FONT_20;
    ui_btn_set_color(&data->btn_reset_stats, 0xBF5E, 0x03DF, 0x1A7B);

    // 7. 触发首帧采样
    if (data->wave_running && (data->tabview.active_tab_idx == 1 || data->tabview.active_tab_idx == 2)) {
        adc_trigger_one_dma_frame(data);
    }
}

static void app_adc_exit(app_t *self) {
    app_adc_data_t *data = (app_adc_data_t *)self->user_data;
    if (data) {
        adc_stop_all(data);
        adc_fifo_setup(false, false, 0, false, false);
        if (data->dma_chan >= 0) {
            dma_channel_unclaim(data->dma_chan);
            data->dma_chan = -1;
        }
    }
    self->user_data = NULL;
}

/* ================= 5. 30 FPS 节流帧处理 ================= */

static void app_adc_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_adc_data_t *data = (app_adc_data_t *)self->user_data;
    if (!data) return;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (now_ms - data->last_ui_frame_ms < 33) {
        return;
    }
    data->last_ui_frame_ms = now_ms;

    // 示波器页 / 统计页：DMA 帧抓取处理
    if (data->tabview.active_tab_idx == 1 || data->tabview.active_tab_idx == 2) {
        if (data->wave_running && data->dma_chan >= 0) {
            if (!dma_channel_is_busy(data->dma_chan)) {
                adc_run(false);

                uint16_t raw_min = 4095, raw_max = 0;
                uint32_t raw_sum = 0;
                float gain = (float)data->cur_gain;

                for (int i = 0; i < ADC_WAVE_SAMPLES; i++) {
                    uint16_t r = data->dma_raw_buf[i];
                    if (r < raw_min) raw_min = r;
                    if (r > raw_max) raw_max = r;
                    raw_sum += r;

                    int16_t wave_val;
                    if (data->active_wave_ch == ADC_CH_TEMP) {
                        float v = (float)r * 3.3f / 4095.0f;
                        float temp_c = 27.0f - (v - 0.706f) / 0.001721f;
                        float norm = ((temp_c - 40.0f) / 40.0f) * gain;
                        if (norm < -1.0f) norm = -1.0f;
                        if (norm > 1.0f) norm = 1.0f;
                        wave_val = (int16_t)(norm * 70.0f);
                    } else {
                        float norm = ((float)r / 4095.0f - 0.5f) * gain;
                        if (norm < -0.5f) norm = -0.5f;
                        if (norm > 0.5f) norm = 0.5f;
                        wave_val = (int16_t)(norm * 140.0f);
                    }
                    data->analog_buf[i] = wave_val;
                }

                uint16_t raw_avg = (uint16_t)(raw_sum / ADC_WAVE_SAMPLES);
                data->state.raw[data->active_wave_ch] = raw_avg;
                data->state.voltage[data->active_wave_ch] = (float)raw_avg * 3.3f / 4095.0f;

                float f_min = (float)raw_min * 3.3f / 4095.0f;
                float f_max = (float)raw_max * 3.3f / 4095.0f;
                float f_avg = (float)raw_avg * 3.3f / 4095.0f;

                if (data->state.sample_cnt == 0) {
                    data->state.v_min = f_min;
                    data->state.v_max = f_max;
                    data->state.v_avg = f_avg;
                } else {
                    if (f_min < data->state.v_min) data->state.v_min = f_min;
                    if (f_max > data->state.v_max) data->state.v_max = f_max;
                    data->state.v_avg = (data->state.v_avg * 0.9f) + (f_avg * 0.1f);
                }
                data->state.v_pp = data->state.v_max - data->state.v_min;
                data->state.sample_cnt += ADC_WAVE_SAMPLES;

                ui_waveform_bind_analog(&data->waveform, data->analog_buf, ADC_WAVE_SAMPLES);

                // 触发下一帧 DMA 抓取
                adc_trigger_one_dma_frame(data);

                if (data->tabview.active_tab_idx == 1) {
                    data->dirty_rect = ui_waveform_get_dirty_rect(&data->waveform);
                } else {
                    data->dirty_rect = data->stats_card;
                }
            }
        }
    }
    // 仪表页 (Tab 0)
    else if (data->tabview.active_tab_idx == 0) {
        adc_sample_sync_tab0(data);
        data->dirty_rect = data->tabview.content_rect;
    }
}

/* ================= 6. 触控分发 ================= */

static bool app_adc_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_adc_data_t *data = (app_adc_data_t *)self->user_data;
    if (!data) return false;

    // 1. 侧边栏触控
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    // 2. 当前 Tab 控件事件
    switch (data->tabview.active_tab_idx) {
        case 1: { // 示波器页
            if (ui_stepper_process_touch(&data->stepper_freq, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_freq));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_gain, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_gain));
                return true;
            }

            if (ui_waveform_process_touch(&data->waveform, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = ui_waveform_get_dirty_rect(&data->waveform);
                return true;
            }
            if (ui_btn_process_touch(&data->btn_wave_toggle, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_ch0, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_ch1, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_ch2, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_temp, is_touching, tx, ty, now_tick)) return true;
            break;
        }

        case 2: { // 统计页
            if (ui_btn_process_touch(&data->btn_reset_stats, is_touching, tx, ty, now_tick)) {
                ui_rect_t b_dirty = ui_btn_get_dirty_rect(&data->btn_reset_stats);
                data->dirty_rect = rect_union_box(data->dirty_rect, b_dirty);
                return true;
            }
            break;
        }
    }

    return (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* ================= 7. 切片光栅化渲染 ================= */

static void app_adc_draw(app_t *self, ui_tile_t *tile) {
    app_adc_data_t *data = (app_adc_data_t *)self->user_data;
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
        case 0: { // ===== Tab 0: 多路电压仪表 =====
            const char *labels[3] = {"ADC0 (GP26):", "ADC1 (GP27):", "ADC2 (GP28):"};
            const uint16_t bar_colors[3] = {0x03DF, 0x05E0, 0xFA00};

            for (int i = 0; i < 3; i++) {
                if (ui_rect_intersect(&tile->rect, &data->monitor_cards[i], &overlap)) {
                    ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                    ui_draw_rect(tile, &data->monitor_cards[i], 0xC618);

                    snprintf(str_buf, sizeof(str_buf), "%s  %.3f V  (Raw: %u)",
                             labels[i], data->state.voltage[i], data->state.raw[i]);
                    ui_draw_string(tile, 130, data->monitor_cards[i].y + 8, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    ui_rect_t bar_bg = ui_rect_make(130, data->monitor_cards[i].y + 32, 330, 14);
                    ui_draw_fill_rect(tile, &bar_bg, 0xC618);
                    
                    int16_t fill_w = (int16_t)((float)bar_bg.w * (data->state.voltage[i] / 3.3f));
                    if (fill_w > bar_bg.w) fill_w = bar_bg.w;
                    if (fill_w > 0) {
                        ui_rect_t bar_fill = ui_rect_make(130, data->monitor_cards[i].y + 32, fill_w, 14);
                        ui_draw_fill_rect(tile, &bar_fill, bar_colors[i]);
                    }
                }
            }

            if (ui_rect_intersect(&tile->rect, &data->monitor_cards[3], &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->monitor_cards[3], 0x07FF);

                snprintf(str_buf, sizeof(str_buf), "内置温度: %.1f °C (电压: %.3fV)",
                         data->state.chip_temp_c, data->state.voltage[ADC_CH_TEMP]);
                ui_draw_string(tile, 130, data->monitor_cards[3].y + 8, str_buf, FONT_16, 0x07FF, 0x10A2);

                ui_rect_t bar_bg = ui_rect_make(130, data->monitor_cards[3].y + 32, 330, 14);
                ui_draw_fill_rect(tile, &bar_bg, 0x2104);
                int16_t fill_w = (int16_t)((float)bar_bg.w * (data->state.chip_temp_c / 80.0f));
                if (fill_w > bar_bg.w) fill_w = bar_bg.w;
                if (fill_w > 0) {
                    ui_rect_t bar_fill = ui_rect_make(130, data->monitor_cards[3].y + 32, fill_w, 14);
                    ui_draw_fill_rect(tile, &bar_fill, 0xF800);
                }
            }
            break;
        }

        case 1: { // ===== Tab 1: 模拟示波器 =====
            ui_waveform_draw(tile, &data->waveform);

            int16_t info_y = 42;
            if ((info_y + 16) > tile->rect.y && info_y < (tile->rect.y + tile->rect.h)) {
                const char *ch_names[] = {"CH0", "CH1", "CH2", "", "温度"};
                if (data->active_wave_ch == ADC_CH_TEMP) {
                    snprintf(str_buf, sizeof(str_buf), "%s: %.1f °C", ch_names[data->active_wave_ch], data->state.chip_temp_c);
                } else {
                    snprintf(str_buf, sizeof(str_buf), "%s: %.3f V", ch_names[data->active_wave_ch], data->state.voltage[data->active_wave_ch]);
                }
                ui_draw_string(tile, 126, info_y, str_buf, FONT_16, 0x07E0, 0x0000);
            }

            ui_btn_draw(tile, &data->btn_wave_toggle);
            ui_stepper_draw(tile, &data->stepper_freq);
            ui_stepper_draw(tile, &data->stepper_gain);

            ui_btn_draw(tile, &data->btn_wave_ch0);
            ui_btn_draw(tile, &data->btn_wave_ch1);
            ui_btn_draw(tile, &data->btn_wave_ch2);
            ui_btn_draw(tile, &data->btn_wave_temp);
            break;
        }

        case 2: { // ===== Tab 2: 极值统计 =====
            if (ui_rect_intersect(&tile->rect, &data->stats_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->stats_card, 0xC618);

                const char *ch_names[] = {"ADC0 (GP26)", "ADC1 (GP27)", "ADC2 (GP28)", "", "内置温度传感器"};
                snprintf(str_buf, sizeof(str_buf), "当前统计通道: %s", ch_names[data->active_wave_ch]);
                ui_draw_string(tile, 140, 54, str_buf, FONT_20, 0x03DF, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "实时电压 (Vcur): %.3f V", data->state.voltage[data->active_wave_ch]);
                ui_draw_string(tile, 140, 84, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "峰峰值 (Vpp):   %.3f V", data->state.v_pp);
                ui_draw_string(tile, 140, 110, str_buf, FONT_16, 0xF800, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "最大值 (Vmax):  %.3f V", data->state.v_max);
                ui_draw_string(tile, 140, 136, str_buf, FONT_16, 0x05E0, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "最小值 (Vmin):  %.3f V  平均: %.3f V", data->state.v_min, data->state.v_avg);
                ui_draw_string(tile, 140, 162, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "累计采集: %lu 点 (DMA 连续)", data->state.sample_cnt);
                ui_draw_string(tile, 140, 188, str_buf, FONT_16, 0x8410, 0xEF7D);
            }
            ui_btn_draw(tile, &data->btn_reset_stats);
            break;
        }

        case 3: { // ===== Tab 3: 引脚说明 =====
            ui_rect_t card = ui_rect_make(120, 42, 350, 240);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);

                ui_draw_string(tile, 135, 54, "RP2040 硬件 DMA-ADC 配置:", FONT_16, 0x03DF, 0xEF7D);
                ui_draw_string(tile, 135, 80, "Pin 31 (GPIO 26) -> ADC 通道 0", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 106, "Pin 32 (GPIO 27) -> ADC 通道 1", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 132, "Pin 34 (GPIO 28) -> ADC 通道 2", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 158, "ADC4 (内部通道) -> 芯片温度传感器", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 188, "采样率范围: 1 ~ 500 kS/s (默认 250kS/s)", FONT_16, 0x05E0, 0xEF7D);
                ui_draw_string(tile, 135, 214, "量程范围: 0.0V ~ 3.3V (禁止超压输入!)", FONT_16, 0xF800, 0xEF7D);
            }
            break;
        }
    }
}

/* ================= 8. 导出单例实体 ================= */

app_t *app_adc_get_instance(void) {
    s_app_adc.id            = APP_ID_CUSTOM_0 + 2;
    s_app_adc.name          = "ADC采样";
    s_app_adc.init          = app_adc_init;
    s_app_adc.enter         = app_adc_enter;
    s_app_adc.exit          = app_adc_exit;
    s_app_adc.update        = app_adc_update;
    s_app_adc.process_touch = app_adc_process_touch;
    s_app_adc.draw          = app_adc_draw;
    return &s_app_adc;
}