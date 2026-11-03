/*
 * 电平触发逻辑分析仪 (PIO + DMA)
 *
 *   CH0 ~ CH5  ->  GP2 ~ GP7  (板上排针, 3.3V 电平, 禁止超压)
 *   PIO: 1 条 IN PINS,6 指令 + autopush, 每点 1 个系统时钟/div
 *   DMA: 从 PIO RX FIFO 以 32bit 搬 1024 words (=4096 样本字节流)
 *   触发: 软件轮询等待指定通道达到高/低电平, 命中瞬间启动 DMA,
 *         因此波形左边缘即触发时刻; "立即"模式则不做等待。
 */
#include "app_la.h"
#include "app_shared_ram.h"
#include "pio_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_waveform.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LA_PIN_BASE          2
#define LA_CH_COUNT         6
#define LA_SAMPLES          4096
#define LA_PROG_NAME        "app_la_cap"
#define LA_ARM_TIMEOUT_MS  5000

/* 采样率档位 (Hz) */
static const uint32_t k_fs_table[] = {
    10000, 50000, 100000, 250000, 500000,
    1000000, 2000000, 5000000, 10000000, 20000000
};
#define LA_FS_COUNT (sizeof(k_fs_table) / sizeof(k_fs_table[0]))

/* 每像素采样点档位 */
static const uint16_t k_spp_table[] = { 1, 2, 4, 8 };
#define LA_SPP_COUNT (sizeof(k_spp_table) / sizeof(k_spp_table[0]))

/* 触发源编码: 0=立即; 1..12 = CH0高/CH0低/CH1高/.../CH5低 */
#define LA_TRIG_COUNT 13

typedef enum {
    LA_TRIG_NONE = 0,
    LA_TRIG_HIGH,
    LA_TRIG_LOW
} la_trig_t;

typedef enum {
    LA_ST_IDLE = 0,
    LA_ST_ARMED,       /* 等待电平触发 */
    LA_ST_CAPTURING,   /* DMA 采集中 */
    LA_ST_DONE,
    LA_ST_TIMEOUT,
    LA_ST_ERROR
} la_state_t;

/* ================= App 私有数据 (放入共享静态池) ================= */
/* ⚠️ dirty_rect 必须是第 1 个成员: app_manager_process_touch_dirty 直接
 * 把 user_data 强转为 ui_rect_t* 取局部脏矩形 */
typedef struct {
    ui_rect_t           dirty_rect;

    ui_waveform_t       waveform;
    ui_btn_t            btn_run;
    ui_btn_t            btn_trig;
    ui_btn_t            btn_fs;
    ui_btn_t            btn_zoom;

    ui_rect_t           status_rect;
    ui_rect_t           live_rect;

    char                trig_label[20];
    char                fs_label[20];
    char                zoom_label[20];
    char                status[80];
    char                live_str[64];

    uint8_t             cap_bytes[LA_SAMPLES] __attribute__((aligned(4)));
    uint32_t           *cap_words;   /* 别名: DMA 按 word 写, 字节序即连续采样流 */

    /* 硬件资源 */
    pio_manager_t      *mgr;
    PIO                 pio;
    uint8_t             pio_idx;
    uint8_t             sm;
    uint                prog_offset;
    int                 dma_chan;
    dma_channel_config  dma_cfg;
    pio_sm_config       sm_cfg;
    bool                hw_ok;
    bool                sm_claimed;
    bool                prog_loaded;

    /* 采集状态 */
    la_state_t          state;
    la_trig_t           trig_mode;
    uint8_t             trig_ch;
    uint8_t             trig_idx;
    uint8_t             fs_idx;
    uint8_t             spp_idx;
    uint8_t             live_bits;
    uint32_t            arm_start_ms;
    uint32_t            last_ui_ms;
} app_la_data_t;

static app_t s_app_la;

/* 运行时构造的 PIO 程序:
 * [0] wait 占位 (本 App 为软件轮询触发, 不执行)
 * [1..4] in pins,8 —— 每 4 样本 autopush 1 word。必须配右移
 *        (sm_config_set_in_shift(..., true, true, 32)), ISR 才是
 *        s0|s1<<8|s2<<16|s3<<24, 小端落盘 = [s0,s1,s2,s3] 时间正序;
 *        用左移会得到 [s3,s2,s1,s0], 波形全是假毛刺且解码全乱。
 * RP2040 的 PIO RX FIFO 只允许 32bit 访问, 禁止 8bit DMA 读 FIFO。 */
static uint16_t s_la_instr[5];
static struct pio_program s_la_program = {
    .instructions = s_la_instr,
    .length       = 5,
    .origin       = -1,
};

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static void fmt_fs(uint32_t hz, char *buf, size_t n) {
    if (hz >= 1000000UL) snprintf(buf, n, "%luM", (unsigned long)(hz / 1000000UL));
    else                 snprintf(buf, n, "%luk", (unsigned long)(hz / 1000UL));
}

static float la_calc_div(uint32_t fs) {
    uint32_t sys = clock_get_hz(clk_sys);
    if (sys == 0) sys = 240000000UL;
    float div = (float)sys / (float)fs;
    if (div < 1.0f) div = 1.0f;
    if (div > 65535.0f) div = 65535.0f;
    return div;
}

static void la_fail(app_la_data_t *d, const char *msg) {
    snprintf(d->status, sizeof(d->status), "错误: %s", msg);
    d->state  = LA_ST_ERROR;
    d->hw_ok  = false;
}

static void la_set_run_btn(app_la_data_t *d, bool running) {
    d->btn_run.text = running ? "停止" : "开始采集";
    if (running) ui_btn_set_color(&d->btn_run, 0xF800, 0xB000, 0xFFFF);
    else         ui_btn_set_color(&d->btn_run, 0x05E0, 0x03E0, 0xFFFF);
}

static void la_update_labels(app_la_data_t *d) {
    if (d->trig_idx == 0) {
        snprintf(d->trig_label, sizeof(d->trig_label), "T:立即");
    } else {
        uint8_t v = d->trig_idx - 1;
        snprintf(d->trig_label, sizeof(d->trig_label), "T:CH%d%s",
                 v / 2, (v % 2 == 0) ? "高" : "低");
    }
    char fs_txt[16];
    fmt_fs(k_fs_table[d->fs_idx], fs_txt, sizeof(fs_txt));
    snprintf(d->fs_label, sizeof(d->fs_label), "Fs:%s", fs_txt);
    snprintf(d->zoom_label, sizeof(d->zoom_label), "Z:%lux",
             (unsigned long)k_spp_table[d->spp_idx]);
}

static void la_apply_trig(app_la_data_t *d) {
    if (d->trig_idx == 0) {
        d->trig_mode = LA_TRIG_NONE;
        d->trig_ch   = 0;
    } else {
        uint8_t v = d->trig_idx - 1;
        d->trig_ch   = v / 2;
        d->trig_mode = (v % 2 == 0) ? LA_TRIG_HIGH : LA_TRIG_LOW;
    }
}

/* ================= 硬件资源申请 / 释放 ================= */

static void la_stop_sm(app_la_data_t *d) {
    if (d->pio) pio_sm_set_enabled(d->pio, d->sm, false);
    if (d->dma_chan >= 0) {
        dma_channel_abort(d->dma_chan);
        uint32_t guard = 0;
        while (dma_channel_is_busy(d->dma_chan) && guard++ < 100000UL) {
            tight_loop_contents();
        }
    }
    if (d->pio) pio_sm_clear_fifos(d->pio, d->sm);
}

static void la_hw_release(app_la_data_t *d) {
    la_stop_sm(d);
    if (d->dma_chan >= 0) {
        dma_channel_unclaim(d->dma_chan);
        d->dma_chan = -1;
    }
    if (d->mgr) {
        if (d->prog_loaded) {
            pio_manager_release_program(d->mgr, d->pio_idx, LA_PROG_NAME);
            d->prog_loaded = false;
        }
        if (d->sm_claimed) {
            pio_manager_release_sm(d->mgr, d->pio_idx, d->sm);
            d->sm_claimed = false;
        }
    }
    for (uint pin = LA_PIN_BASE; pin < LA_PIN_BASE + LA_CH_COUNT; pin++) {
        gpio_init(pin); /* 恢复 SIO 输入, 交还给其它 App */
    }
    d->hw_ok = false;
}

static void la_hw_init(app_la_data_t *d) {
    d->mgr = pio_manager_get_default();
    if (!d->mgr) { la_fail(d, "PIO 管理器不可用"); return; }

    int sm_id = pio_manager_claim_sm_from(d->mgr, 0, LA_PROG_NAME);
    if (sm_id < 0) sm_id = pio_manager_claim_sm_from(d->mgr, 1, LA_PROG_NAME);
    if (sm_id < 0) { la_fail(d, "PIO 状态机已占满"); return; }
    d->sm_claimed = true;
    d->pio_idx    = (uint8_t)(sm_id / PIO_SM_COUNT);
    d->sm         = (uint8_t)(sm_id % PIO_SM_COUNT);
    d->pio        = pio_manager_get_hardware(d->mgr, d->pio_idx);
    if (!d->pio) { la_fail(d, "PIO 硬件为空"); la_hw_release(d); return; }

    /* in pins,8 ×4 —— 1 word 装 4 样本 (bit6/7 为 GP8/9 串扰, 显示只用 bit0~5) */
    s_la_instr[0] = (uint16_t)(pio_instr_bits_wait | (1u << 5));
    for (int i = 1; i < 5; i++)
        s_la_instr[i] = (uint16_t)pio_encode_in(pio_pins, 8);
    int off = pio_manager_load_program(d->mgr, d->pio_idx, LA_PROG_NAME, &s_la_program);
    if (off < 0) { la_fail(d, "PIO 程序加载失败"); la_hw_release(d); return; }
    d->prog_offset = (uint)off;
    d->prog_loaded  = true;

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg, (uint)off + 1, (uint)off + 4);   /* 循环 in 区 */
    sm_config_set_in_pins(&cfg, LA_PIN_BASE);
    /* 右移 + autopush 32: 4 次 IN 后 ISR = s0|s1<<8|s2<<16|s3<<24,
       小端写入内存正好是 [s0,s1,s2,s3] 的时间正序。
       用左移会得到 [s3,s2,s1,s0], 波形全是 1~3 点的假毛刺、解码全乱。 */
    sm_config_set_in_shift(&cfg, true, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);          /* RX FIFO 深度 8 */
    sm_config_set_clkdiv(&cfg, la_calc_div(k_fs_table[d->fs_idx]));
    d->sm_cfg = cfg;

    for (uint pin = LA_PIN_BASE; pin < LA_PIN_BASE + LA_CH_COUNT; pin++) {
        pio_gpio_init(d->pio, pin);
    }
    pio_sm_set_enabled(d->pio, d->sm, false);
    pio_sm_init(d->pio, d->sm, (uint)off, &cfg);
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, LA_PIN_BASE, LA_CH_COUNT, false);
    pio_sm_clear_fifos(d->pio, d->sm);

    d->dma_chan = dma_claim_unused_channel(false);
    if (d->dma_chan < 0) { la_fail(d, "DMA 通道不足"); la_hw_release(d); return; }

    dma_channel_config dc = dma_channel_get_default_config((uint)d->dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(d->pio, d->sm, false));
    d->dma_cfg = dc;

    d->cap_words = (uint32_t *)d->cap_bytes;
    d->hw_ok = true;
}

/* 触发命中后启动一帧 DMA 采集 (触发点 = 第 0 点) */
static void la_start_capture(app_la_data_t *d) {
    if (!d->hw_ok) return;

    sm_config_set_clkdiv(&d->sm_cfg, la_calc_div(k_fs_table[d->fs_idx]));
    pio_sm_set_enabled(d->pio, d->sm, false);
    pio_sm_init(d->pio, d->sm, d->prog_offset + 1, &d->sm_cfg);  /* PC 直指 in 循环 */
    pio_sm_set_consecutive_pindirs(d->pio, d->sm, LA_PIN_BASE, LA_CH_COUNT, false);
    pio_sm_clear_fifos(d->pio, d->sm);

    /* DMA 先就绪, 再放开 SM —— 采样点从触发瞬间起不丢 */
    dma_channel_configure((uint)d->dma_chan, &d->dma_cfg,
                          d->cap_words,
                          (const void *)(uintptr_t)&d->pio->rxf[d->sm],
                          LA_SAMPLES / 4, true);
    pio_sm_set_enabled(d->pio, d->sm, true);
    d->state = LA_ST_CAPTURING;
}

/* ================= 交互回调 ================= */

static void on_run_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_la_data_t *d = (app_la_data_t *)user_data;
    if (!d) return;

    if (d->state == LA_ST_ARMED || d->state == LA_ST_CAPTURING) {
        la_stop_sm(d);
        d->state = LA_ST_IDLE;
        la_set_run_btn(d, false);
        snprintf(d->status, sizeof(d->status), "已停止, 按[开始采集]重新开始");
    } else {
        if (!d->hw_ok) {
            snprintf(d->status, sizeof(d->status), "硬件未就绪, 无法采集");
        } else if (d->trig_mode == LA_TRIG_NONE) {
            la_start_capture(d);
            snprintf(d->status, sizeof(d->status), "采集中(立即) %s ...", d->fs_label);
            la_set_run_btn(d, true);
        } else {
            d->arm_start_ms = to_ms_since_boot(get_absolute_time());
            d->state = LA_ST_ARMED;
            snprintf(d->status, sizeof(d->status), "等待 %s 电平触发 ...", d->trig_label);
            la_set_run_btn(d, true);
        }
    }
    d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_run.rect);
    d->dirty_rect = rect_union_box(d->dirty_rect, d->status_rect);
}

static void on_trig_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_la_data_t *d = (app_la_data_t *)user_data;
    if (!d) return;
    d->trig_idx = (uint8_t)((d->trig_idx + 1) % LA_TRIG_COUNT);
    la_apply_trig(d);
    la_update_labels(d);
    d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_trig.rect);
}

static void on_fs_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_la_data_t *d = (app_la_data_t *)user_data;
    if (!d) return;
    /* 采集中改采样率会导致 clkdiv 与 dT 换算不一致, 先中止当前帧 */
    bool interrupted = (d->state == LA_ST_ARMED || d->state == LA_ST_CAPTURING);
    if (interrupted) {
        la_stop_sm(d);
        d->state = LA_ST_IDLE;
        la_set_run_btn(d, false);
    }
    d->fs_idx = (uint8_t)((d->fs_idx + 1) % LA_FS_COUNT);
    d->waveform.sample_rate_hz = k_fs_table[d->fs_idx];
    la_update_labels(d);
    if (interrupted) {
        snprintf(d->status, sizeof(d->status), "采样率已改, 按[开始采集]重新采集");
        d->dirty_rect = rect_union_box(d->dirty_rect, d->status_rect);
        d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_run.rect);
    }
    d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_fs.rect);
}

static void on_zoom_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_la_data_t *d = (app_la_data_t *)user_data;
    if (!d) return;
    d->spp_idx = (uint8_t)((d->spp_idx + 1) % LA_SPP_COUNT);
    uint16_t spp = k_spp_table[d->spp_idx];
    ui_waveform_set_scale(&d->waveform, spp, LA_CH_COUNT);
    uint32_t span = (uint32_t)spp * (uint32_t)d->waveform.rect.w;
    uint32_t max_off = (LA_SAMPLES > span) ? (LA_SAMPLES - span) : 0;
    if (d->waveform.view_offset > max_off) d->waveform.view_offset = max_off;
    la_update_labels(d);
    d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_zoom.rect);
    d->dirty_rect = rect_union_box(d->dirty_rect, d->waveform.rect);
}

/* ================= 生命周期 ================= */

static void app_la_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_la_enter(app_t *self) {
    app_la_data_t *d = (app_la_data_t *)g_static_app_ram;
    self->user_data = d;
    memset(d, 0, sizeof(app_la_data_t));

    d->dirty_rect   = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    d->dma_chan     = -1;
    d->state        = LA_ST_IDLE;
    d->trig_idx     = 0;               /* 默认"立即", 首次使用即可看到波形 */
    d->fs_idx       = 2;               /* 100 kS/s */
    d->spp_idx      = 0;               /* 1 点/像素 */
    la_apply_trig(d);
    la_update_labels(d);

    d->status_rect = ui_rect_make(8, 214, 464, 22);
    d->live_rect   = ui_rect_make(8, 238, 464, 20);
    snprintf(d->status, sizeof(d->status), "就绪 | 选触发源后按[开始采集]");

    /* 波形画布: 6 通道数字时序 */
    ui_waveform_init(&d->waveform, 8, 40, 464, 170,
                     UI_WAVE_MODE_DIGITAL_8CH, k_fs_table[d->fs_idx]);
    d->waveform.color_bg   = 0x0000;
    d->waveform.color_grid = 0x2104;
    ui_waveform_set_scale(&d->waveform, 1, LA_CH_COUNT);
    ui_waveform_bind_digital(&d->waveform, d->cap_bytes, LA_SAMPLES);

    /* 控制行 */
    ui_btn_init(&d->btn_run,   8, 262,  92, 34, "开始采集", on_run_click);
    ui_btn_init(&d->btn_trig, 104, 262, 132, 34, d->trig_label, on_trig_click);
    ui_btn_init(&d->btn_fs,   240, 262, 100, 34, d->fs_label,   on_fs_click);
    ui_btn_init(&d->btn_zoom, 344, 262, 128, 34, d->zoom_label, on_zoom_click);

    d->btn_run.user_data  = d;
    d->btn_trig.user_data = d;
    d->btn_fs.user_data   = d;
    d->btn_zoom.user_data = d;
    d->btn_run.style = d->btn_trig.style = UI_BTN_STYLE_ROUND;
    d->btn_fs.style  = d->btn_zoom.style = UI_BTN_STYLE_ROUND;
    d->btn_run.font_size = d->btn_trig.font_size = FONT_16;
    d->btn_fs.font_size  = d->btn_zoom.font_size  = FONT_16;
    la_set_run_btn(d, false);
    ui_btn_set_color(&d->btn_trig, 0x1A7B, 0x105F, 0xFFFF);
    ui_btn_set_color(&d->btn_fs,   0x03DF, 0x0275, 0xFFFF);
    ui_btn_set_color(&d->btn_zoom, 0x8410, 0x5288, 0xFFFF);

    /* 硬件 */
    la_hw_init(d);
}

static void app_la_exit(app_t *self) {
    app_la_data_t *d = (app_la_data_t *)self->user_data;
    if (d) la_hw_release(d);
    self->user_data = NULL;
}

/* ================= 帧更新 / 电平触发状态机 ================= */

static void app_la_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_la_data_t *d = (app_la_data_t *)self->user_data;
    if (!d) return;

    d->dirty_rect = ui_rect_make(0, 0, 0, 0);

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - d->last_ui_ms < 33) return;
    d->last_ui_ms = now_ms;

    /* 实时电平监视 (即使未采集也能确认接线) */
    uint8_t live = (uint8_t)((gpio_get_all() >> LA_PIN_BASE) & ((1u << LA_CH_COUNT) - 1));
    if (live != d->live_bits) {
        d->live_bits = live;
        d->dirty_rect = rect_union_box(d->dirty_rect, d->live_rect);
    }

    switch (d->state) {
        case LA_ST_ARMED: {
            uint32_t mask = 1u << (LA_PIN_BASE + d->trig_ch);
            bool high = (gpio_get_all() & mask) != 0;
            bool want = (d->trig_mode == LA_TRIG_HIGH);
            if (high == want) {
                la_start_capture(d);
                snprintf(d->status, sizeof(d->status),
                         "已触发 %s ! 采集中 %s ...", d->trig_label, d->fs_label);
            } else if (now_ms - d->arm_start_ms > LA_ARM_TIMEOUT_MS) {
                d->state = LA_ST_TIMEOUT;
                la_set_run_btn(d, false);
                snprintf(d->status, sizeof(d->status),
                         "等待超时(5s): 未检测到 %s", d->trig_label);
            } else {
                snprintf(d->status, sizeof(d->status),
                         "等待 %s ... %lus",
                         d->trig_label,
                         (unsigned long)((now_ms - d->arm_start_ms) / 1000UL));
            }
            d->dirty_rect = rect_union_box(d->dirty_rect, d->status_rect);
            d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_run.rect);
            break;
        }

        case LA_ST_CAPTURING: {
            if (!dma_channel_is_busy((uint)d->dma_chan)) {
                la_stop_sm(d);
                d->waveform.sample_rate_hz = k_fs_table[d->fs_idx];
                ui_waveform_bind_digital(&d->waveform, d->cap_bytes, LA_SAMPLES);
                d->waveform.view_offset = 0;    /* 左边缘 = 触发时刻 */
                d->state = LA_ST_DONE;
                la_set_run_btn(d, false);
                snprintf(d->status, sizeof(d->status),
                         "完成 %u点 @ %s | 拖动平移/游标测dT",
                         (unsigned)LA_SAMPLES, d->fs_label);
                d->dirty_rect = rect_union_box(d->dirty_rect, d->waveform.rect);
                d->dirty_rect = rect_union_box(d->dirty_rect, d->status_rect);
                d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_run.rect);
            }
            break;
        }

        default:
            break;
    }
}

/* ================= 触控分发 ================= */

static bool app_la_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty,
                                uint32_t now_tick) {
    app_la_data_t *d = (app_la_data_t *)self->user_data;
    if (!d) return false;

    if (ui_waveform_process_touch(&d->waveform, is_touching, tx, ty, now_tick)) {
        d->dirty_rect = rect_union_box(d->dirty_rect, d->waveform.rect);
    }
    if (ui_btn_process_touch(&d->btn_run, is_touching, tx, ty, now_tick)) {
        d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_run.rect);
    }
    if (ui_btn_process_touch(&d->btn_trig, is_touching, tx, ty, now_tick)) {
        d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_trig.rect);
    }
    if (ui_btn_process_touch(&d->btn_fs, is_touching, tx, ty, now_tick)) {
        d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_fs.rect);
    }
    if (ui_btn_process_touch(&d->btn_zoom, is_touching, tx, ty, now_tick)) {
        d->dirty_rect = rect_union_box(d->dirty_rect, d->btn_zoom.rect);
    }

    return (d->dirty_rect.w > 0 && d->dirty_rect.h > 0);
}

/* ================= 光栅化渲染 ================= */

static void app_la_draw(app_t *self, ui_tile_t *tile) {
    app_la_data_t *d = (app_la_data_t *)self->user_data;
    if (!d) return;
    ui_rect_t ov;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xFFFF);
    }

    /* 波形视口 */
    ui_waveform_draw(tile, &d->waveform);

    /* 状态条 */
    if (ui_rect_intersect(&tile->rect, &d->status_rect, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xEF7D);
    }
    if ((d->status_rect.y + 16) > tile->rect.y &&
        d->status_rect.y < (tile->rect.y + tile->rect.h)) {
        ui_draw_string(tile, 12, d->status_rect.y + 2, d->status,
                       FONT_16, 0x1A7B, 0xEF7D);
    }

    /* 实时电平 */
    if (ui_rect_intersect(&tile->rect, &d->live_rect, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xFFFF);
    }
    if ((d->live_rect.y + 16) > tile->rect.y &&
        d->live_rect.y < (tile->rect.y + tile->rect.h)) {
        snprintf(d->live_str, sizeof(d->live_str),
                 "实时 D0~D5: %d %d %d %d %d %d",
                 (d->live_bits >> 0) & 1, (d->live_bits >> 1) & 1,
                 (d->live_bits >> 2) & 1, (d->live_bits >> 3) & 1,
                 (d->live_bits >> 4) & 1, (d->live_bits >> 5) & 1);
        ui_draw_string(tile, 12, d->live_rect.y + 2, d->live_str,
                       FONT_16, 0x03DF, 0xFFFF);
    }

    /* 控制按钮 */
    ui_btn_draw(tile, &d->btn_run);
    ui_btn_draw(tile, &d->btn_trig);
    ui_btn_draw(tile, &d->btn_fs);
    ui_btn_draw(tile, &d->btn_zoom);

    /* 底部说明 */
    int16_t tip_y = 302;
    if ((tip_y + 16) > tile->rect.y && tip_y < (tile->rect.y + tile->rect.h)) {
        ui_draw_string(tile, 10, tip_y,
                       "CH0~5=GP2~7 (3.3V) | 跳线接3.3V/GND可自测",
                       FONT_16, 0x8410, 0xFFFF);
    }
}

/* ================= 导出单例 ================= */

app_t *app_la_get_instance(void) {
    s_app_la.id            = APP_ID_LOGIC_LA;
    s_app_la.name          = "逻辑分析";
    s_app_la.icon_symbol   = "LA";
    s_app_la.icon_color   = 0x1A7B;
    s_app_la.init          = app_la_init;
    s_app_la.enter         = app_la_enter;
    s_app_la.exit          = app_la_exit;
    s_app_la.update        = app_la_update;
    s_app_la.process_touch = app_la_process_touch;
    s_app_la.draw          = app_la_draw;
    return &s_app_la;
}
