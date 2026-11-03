/*
 * LA over USB CDC —— 实现, 协议说明见 la_usb.h
 *
 * 资源占用 (首个合法命令到达时懒申请, 断电前不释放):
 *   - PIO 状态机 ×1 + 5 条指令 (pio_manager)
 *   - DMA 通道 ×2 (A 主采, B 单次模式当下水道 / 连续模式乒乓)
 *   - DMA_IRQ_0 (audio_i2s 用的是 IRQ_1, 无冲突)
 *   - RAM 0x2003C000 ~ 0x20040000 (32KB, 链接脚本 .la_pool_sec)
 */
#include "la_usb.h"
#include "pio_manager.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include <string.h>

/* ================= 参数 ================= */
#define LA_PIN_BASE       4u   /* GP4~GP7 = CH0~CH3; GP2/GP3 留给 DAPLink SWD */
#define LA_CH_COUNT       4u
#define LA_DEPTH_SAMPLES  32768u
#define LA_DEPTH_WORDS    (LA_DEPTH_SAMPLES / 4u)   /* 8192 words */
#define LA_HALF_SAMPLES   (LA_DEPTH_SAMPLES / 2u)   /* 16384 */
#define LA_HALF_WORDS     (LA_DEPTH_WORDS / 2u)     /* 4096  */
#define LA_PROG_NAME      "la_usb_cap"
#define LA_READ_MAX       1024u                     /* 单帧 READ 上限 */

/* 协议命令号 (与 TangNano LA 一致) */
enum {
    LA_CMD_SET_DIV  = 0x01,
    LA_CMD_SET_TRIG = 0x02,
    LA_CMD_ARM      = 0x03,
    LA_CMD_STOP     = 0x04,
    LA_CMD_STATUS   = 0x05,
    LA_CMD_READ     = 0x06,
    LA_CMD_IDENT    = 0x07,
    LA_CMD_SET_MODE = 0x08,
    LA_CMD_SET_RLE  = 0x0A,   /* Pico 扩展: RLE 压缩使能 */
};

typedef enum {
    LA_PHASE_IDLE = 0,
    LA_PHASE_WAIT_TRIG,   /* SM 停在 wait, 等触发电平 */
    LA_PHASE_CAPTURING,
    LA_PHASE_DONE,
} la_phase_t;

/* ================= 32KB 采样缓冲 =================
 * 直接复用 plugin RAM 池 (0x20038000, 32KB): 引擎活跃期间
 * app_loader 拒绝加载 plugin, 空闲 60 秒后 la_hw_release 归还。 */
extern uint8_t *app_loader_ram_pool(void);
#define LA_BUF ((uint32_t *)app_loader_ram_pool())

/* ================= PIO 程序 =================
 * [0] wait gpio <pol> <pin>   ← ARM 时按触发源覆写 instr_mem
 * [1..4] in pins, 8           ← wrap [1..4], 每 4 样本 autopush 1 word
 * 立即模式: SM 初始 PC = off+1, 永不执行 wait。
 */
static uint16_t s_la_prog_ins[5];
static const struct pio_program s_la_prog = {
    .instructions = s_la_prog_ins,
    .length       = 5,
    .origin       = -1,
};

/* ================= 运行状态 ================= */
static pio_manager_t *s_mgr;
static PIO     s_pio;
static uint8_t s_pio_idx, s_sm;
static uint    s_prog_off;
static int     s_dma_a = -1, s_dma_b = -1;
static bool    s_hw_open = false;

static volatile la_phase_t s_phase = LA_PHASE_IDLE;
static volatile uint8_t   s_seq    = 0;      /* 连续模式半区序号 */
static volatile uint8_t   s_ready  = 0;      /* bit0=半区0可读 bit1=半区1可读 */
static volatile bool      s_cont   = false;

static uint32_t s_cfg_div  = 240;            /* 240MHz/240 = 1 MHz 默认 */
static uint8_t  s_trig_mode = 0;             /* 0立即 1电平 2上升 3下降 */
static uint8_t  s_trig_ch   = 0;
static bool     s_trig_pol  = true;          /* true=等高电平 */
static uint32_t s_scratch;                   /* 下水道丢弃目标 */

/* ---- RLE 压缩状态 ----
 * 采样值只用低 4 位 (CH0~3 = GP4~7)。压缩在采样缓冲**原地**进行:
 *   0x00~0x7F  字面量: 1 字节 = 1 样本
 *   0x80~0xFF  游程:   低 7 位 = count-1 (1~127), 后跟 1 字节值
 * 每条游程记录花 2 字节省 count-1 字节 (count>=2), 字面量 1:1,
 * 所以输出 <= 输入 (最坏情况逐样本翻转时不膨胀), 原地压缩不会越界。 */
static bool     s_rle = false;
static bool     s_shot_comp = false;         /* 单次缓冲已压缩 */
static uint32_t s_shot_clen = 0;             /* 单次压缩后字节数 */
static uint32_t s_half_clen[2] = {0, 0};     /* 连续模式两半区压缩后字节数 */
static volatile uint8_t s_comp_seq = 0;      /* 已压缩到的半区序号 */

/* ================= CRC8 (poly 0x07, init 0) ================= */
static uint8_t la_crc8(uint8_t c, const uint8_t *d, uint32_t n) {
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    return c;
}

/* ================= DMA_IRQ_0 中断 (定义在下方) ================= */
static void __not_in_flash_func(la_dma_irq_handler)(void);
static void la_stop_capture(void);

/* ================= CDC 发送 ================= */
static uint8_t s_tx[5 + LA_READ_MAX];

static void la_cdc_write(const uint8_t *buf, uint32_t len) {
    uint32_t t0 = time_us_32();
    while (len > 0) {
        if (!tud_ready()) return;
        uint32_t w = tud_cdc_n_write(0, buf, len);
        buf += w;
        len -= w;
        if (w) tud_cdc_n_write_flush(0);
        if (len && time_us_32() - t0 > 200000u) return;   /* 200ms 兜底 */
    }
}

static void la_send(uint8_t cmd, const uint8_t *pay, uint16_t len) {
    s_tx[0] = 0x5A;
    s_tx[1] = cmd;
    s_tx[2] = (uint8_t)(len & 0xFF);
    s_tx[3] = (uint8_t)(len >> 8);
    if (len) memcpy(s_tx + 4, pay, len);
    s_tx[4 + len] = la_crc8(0, s_tx + 1, (uint32_t)(3 + len));
    la_cdc_write(s_tx, (uint32_t)(5 + len));
}

/* ================= 采集启停 ================= */
static void la_hw_open(void) {
    if (s_hw_open) return;
    s_la_prog_ins[0] = (uint16_t)(pio_instr_bits_wait | (1u << 5) | 0u); /* wait 1 gpio0 占位 */
    for (int i = 1; i < 5; i++)
        s_la_prog_ins[i] = (uint16_t)pio_encode_in(pio_pins, 8);

    s_mgr = pio_manager_get_default();
    if (!s_mgr) return;

    int sm_id = pio_manager_claim_sm_from(s_mgr, 0, LA_PROG_NAME);
    if (sm_id < 0) sm_id = pio_manager_claim_sm_from(s_mgr, 1, LA_PROG_NAME);
    if (sm_id < 0) return;
    s_pio_idx = (uint8_t)(sm_id / PIO_SM_COUNT);
    s_sm      = (uint8_t)(sm_id % PIO_SM_COUNT);
    s_pio     = pio_manager_get_hardware(s_mgr, s_pio_idx);
    if (!s_pio) goto fail;

    {
        int off = pio_manager_load_program(s_mgr, s_pio_idx, LA_PROG_NAME, &s_la_prog);
        if (off < 0) goto fail;
        s_prog_off = (uint)off;
    }

    for (uint pin = LA_PIN_BASE; pin < LA_PIN_BASE + LA_CH_COUNT; pin++)
        pio_gpio_init(s_pio, pin);

    s_dma_a = dma_claim_unused_channel(false);
    s_dma_b = dma_claim_unused_channel(false);
    if (s_dma_a < 0 || s_dma_b < 0) goto fail;

    irq_set_exclusive_handler(DMA_IRQ_0, la_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    s_hw_open = true;
    return;

fail:
    if (s_dma_a >= 0) { dma_channel_unclaim((uint)s_dma_a); s_dma_a = -1; }
    if (s_dma_b >= 0) { dma_channel_unclaim((uint)s_dma_b); s_dma_b = -1; }
    if (s_mgr) {
        pio_manager_release_program(s_mgr, s_pio_idx, LA_PROG_NAME);
        pio_manager_release_sm(s_mgr, s_pio_idx, s_sm);
    }
    s_hw_open = false;
}

/* 空闲超时释放: 停止状态 60 秒无任何命令 → 归还全部硬件与 RAM 池,
   让 >16KB 的 plugin 恢复可加载 (不需要重新上电)。 */
static void la_hw_release(void) {
    if (!s_hw_open) return;
    la_stop_capture();
    irq_set_enabled(DMA_IRQ_0, false);
    irq_remove_handler(DMA_IRQ_0, la_dma_irq_handler);
    if (s_dma_a >= 0) { dma_channel_unclaim((uint)s_dma_a); s_dma_a = -1; }
    if (s_dma_b >= 0) { dma_channel_unclaim((uint)s_dma_b); s_dma_b = -1; }
    if (s_mgr) {
        pio_manager_release_program(s_mgr, s_pio_idx, LA_PROG_NAME);
        pio_manager_release_sm(s_mgr, s_pio_idx, s_sm);
    }
    s_hw_open = false;
}

/* DMA_IRQ_0 中断: 乒乓半区完成 (定义在 la_hw_open 之前使用, 用前向声明解决) */
static void __not_in_flash_func(la_dma_irq_handler)(void);

bool la_usb_pool_active(void) {
    return s_hw_open;
}

/* ================= ARM / STOP ================= */
static void la_stop_capture(void) {
    if (!s_hw_open) return;
    pio_sm_set_enabled(s_pio, s_sm, false);
    if (s_dma_a >= 0) dma_channel_abort((uint)s_dma_a);
    if (s_dma_b >= 0) dma_channel_abort((uint)s_dma_b);
    dma_channel_set_irq0_enabled((uint)s_dma_a, false);
    dma_channel_set_irq0_enabled((uint)s_dma_b, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    s_phase = LA_PHASE_IDLE;
}

static void la_arm(void) {
    if (!s_hw_open) return;

    la_stop_capture();
    s_seq = 0;
    s_ready = 0;

    float clkdiv = (float)s_cfg_div;
    if (clkdiv < 1.0f) clkdiv = 1.0f;
    if (clkdiv > 65535.0f) clkdiv = 65535.0f;

    /* 触发源覆写 wait 指令; 立即模式 PC 直接指到 in 循环 */
    uint entry_pc = s_prog_off;
    if (s_trig_mode != 0) {
        uint pin = LA_PIN_BASE + s_trig_ch;
        s_pio->instr_mem[s_prog_off] =
            (uint16_t)(pio_instr_bits_wait | (s_trig_pol ? (1u << 5) : 0u) | pin);
        s_phase = LA_PHASE_WAIT_TRIG;
    } else {
        entry_pc = s_prog_off + 1;
        s_phase = LA_PHASE_CAPTURING;
    }

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg, s_prog_off + 1, s_prog_off + 4);
    sm_config_set_in_pins(&cfg, (uint)LA_PIN_BASE);
    /* 右移 + autopush 32: 4 次 IN 后 ISR = s0|s1<<8|s2<<16|s3<<24,
       小端写入内存正好是 [s0,s1,s2,s3] 的时间正序。
       (左移会得到 [s3,s2,s1,s0], 波形全是 1~3 点的假毛刺) */
    sm_config_set_in_shift(&cfg, true, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&cfg, clkdiv);

    pio_sm_init(s_pio, s_sm, entry_pc, &cfg);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, (uint)LA_PIN_BASE, LA_CH_COUNT, false);

    uint dreq = pio_get_dreq(s_pio, s_sm, false);

    if (s_cont) {
        /* 乒乓: A 写半区0 → chain 触发 B 写半区1 → chain 触发 A(已由 IRQ 重配) */
        dma_channel_config ca = dma_channel_get_default_config((uint)s_dma_a);
        channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
        channel_config_set_read_increment(&ca, false);
        channel_config_set_write_increment(&ca, true);
        channel_config_set_dreq(&ca, dreq);
        channel_config_set_chain_to(&ca, (uint)s_dma_b);
        dma_channel_configure((uint)s_dma_a, &ca,
                              LA_BUF, &s_pio->rxf[s_sm], LA_HALF_WORDS, false);

        dma_channel_config cb = dma_channel_get_default_config((uint)s_dma_b);
        channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
        channel_config_set_read_increment(&cb, false);
        channel_config_set_write_increment(&cb, true);
        channel_config_set_dreq(&cb, dreq);
        channel_config_set_chain_to(&cb, (uint)s_dma_a);
        dma_channel_configure((uint)s_dma_b, &cb,
                              LA_BUF + LA_HALF_WORDS, &s_pio->rxf[s_sm], LA_HALF_WORDS, false);

        dma_channel_set_irq0_enabled((uint)s_dma_a, true);
        dma_channel_set_irq0_enabled((uint)s_dma_b, true);

        pio_sm_set_enabled(s_pio, s_sm, true);
        dma_channel_start((uint)s_dma_a);
    } else {
        /* 单次: A 采满全深度; 完成后 chain 触发 B 把后续 FIFO 数据
         * 丢进 s_scratch (写不递增), 防止 chain 触发 count=0 通道产生
         * 65536 风暴写穿 RAM。IRQ 里只认 A 完成。 */
        dma_channel_config ca = dma_channel_get_default_config((uint)s_dma_a);
        channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
        channel_config_set_read_increment(&ca, false);
        channel_config_set_write_increment(&ca, true);
        channel_config_set_dreq(&ca, dreq);
        channel_config_set_chain_to(&ca, (uint)s_dma_b);
        dma_channel_configure((uint)s_dma_a, &ca,
                              LA_BUF, &s_pio->rxf[s_sm], LA_DEPTH_WORDS, false);

        dma_channel_config cb = dma_channel_get_default_config((uint)s_dma_b);
        channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
        channel_config_set_read_increment(&cb, false);
        channel_config_set_write_increment(&cb, false);       /* 固定写 s_scratch */
        channel_config_set_dreq(&cb, dreq);
        channel_config_set_chain_to(&cb, (uint)s_dma_b);      /* 自循环丢数据 */
        dma_channel_configure((uint)s_dma_b, &cb,
                              &s_scratch, &s_pio->rxf[s_sm], 1, false);

        dma_channel_set_irq0_enabled((uint)s_dma_a, true);    /* B 的 IRQ 不开 */
        dma_channel_set_irq0_enabled((uint)s_dma_b, false);

        pio_sm_set_enabled(s_pio, s_sm, true);
        dma_channel_start((uint)s_dma_a);
    }
}

/* 单次模式当前 valid 样本数 (实时进度) */
static uint32_t la_valid_now(void) {
    if (!s_hw_open || s_dma_a < 0) return 0;
    uint32_t remain = dma_channel_hw_addr((uint)s_dma_a)->transfer_count;
    uint32_t done_words = (LA_DEPTH_WORDS > remain) ? (LA_DEPTH_WORDS - remain) : 0;
    uint32_t samples = done_words * 4u;
    return (samples > LA_DEPTH_SAMPLES) ? LA_DEPTH_SAMPLES : samples;
}

/* RLE 压缩 (原地)。返回压缩后字节数, 恒 <= n。 */
static uint32_t la_rle_compress(uint8_t *buf, uint32_t n) {
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint8_t v = (uint8_t)(buf[i] & 0x0Fu);
        uint32_t run = 1;
        while (run < 128u && i + run < n && (buf[i + run] & 0x0Fu) == v) run++;
        if (run >= 2u) {
            buf[o++] = (uint8_t)(0x80u | (run - 1u));
            buf[o++] = v;
            i += run;
        } else {
            buf[o++] = v;
            i++;
        }
    }
    return o;
}

/* 重置压缩状态 (ARM / 模式切换 / RLE 开关时调用) */
static void la_rle_reset(void) {
    s_shot_comp = false;
    s_shot_clen = 0;
    s_half_clen[0] = s_half_clen[1] = 0;
    s_comp_seq = 0;
}

/* ================= 命令执行 ================= */
static uint32_t s_last_cmd_ms;

static void la_exec_cmd(uint8_t cmd, const uint8_t *pl, uint8_t len) {
    uint8_t ack = cmd;
    uint8_t rsp[12];
    s_last_cmd_ms = to_ms_since_boot(get_absolute_time());

    switch (cmd) {
        case LA_CMD_IDENT:
            rsp[0] = 'P'; rsp[1] = 'I'; rsp[2] = 'C'; rsp[3] = 'O';
            rsp[4] = 0x03;                 /* ver: 3 = 支持 RLE (0x0A 命令) */
            rsp[5] = (uint8_t)LA_CH_COUNT; /* 通道数 */
            la_send(LA_CMD_IDENT, rsp, 6);
            return;

        case LA_CMD_SET_DIV: {
            if (len >= 4) {
                uint32_t div = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8)
                             | ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24);
                if (div < 1) div = 1;
                if (div > 65535) div = 65535;   /* PIO clkdiv 上限 → 最低约 3.66 kHz */
                s_cfg_div = div;
            }
            break;
        }

        case LA_CMD_SET_TRIG: {
            if (len >= 3) {
                uint8_t mask = pl[0], value = pl[1], mode = pl[2];
                if (mask == 0 || mode == 0) {
                    s_trig_mode = 0;
                } else {
                    s_trig_mode = (mode > 3) ? 1 : mode;
                    s_trig_ch = 0;
                    for (uint8_t b = 0; b < LA_CH_COUNT; b++) {
                        if (mask & (1u << b)) { s_trig_ch = b; break; }
                    }
                    if (s_trig_mode == 1) s_trig_pol = (value & mask) != 0;
                    else if (s_trig_mode == 2) s_trig_pol = true;   /* 上升沿 ≈ 等待变高 */
                    else s_trig_pol = false;                        /* 下降沿 ≈ 等待变低 */
                }
            }
            break;
        }

        case LA_CMD_ARM:
            la_hw_open();
            if (s_hw_open) {
                la_rle_reset();
                la_arm();
            } else {
                ack = 0xEE;
            }
            break;

        case LA_CMD_STOP:
            /* 单次中途停止: 先趁 DMA 还在跑读出 valid 并压缩, 再停 */
            if (!s_cont && s_rle && s_hw_open && s_phase == LA_PHASE_CAPTURING) {
                uint32_t v = la_valid_now() & ~3u;
                if (v) {
                    s_shot_clen = la_rle_compress((uint8_t *)LA_BUF, v);
                    s_shot_comp = true;
                }
            }
            la_stop_capture();
            break;

        case LA_CMD_STATUS: {
            uint32_t valid, clen = 0;
            if (s_cont) {
                valid = (s_phase == LA_PHASE_IDLE) ? 0 : LA_DEPTH_SAMPLES;
                if (s_rle && s_seq)
                    clen = s_half_clen[(uint32_t)(s_seq - 1) & 1u];
            } else {
                if (s_phase == LA_PHASE_DONE) {
                    if (s_rle && !s_shot_comp) {
                        s_shot_clen = la_rle_compress((uint8_t *)LA_BUF, LA_DEPTH_SAMPLES);
                        s_shot_comp = true;
                    }
                    valid = s_rle ? s_shot_clen : LA_DEPTH_SAMPLES;
                } else if (s_phase == LA_PHASE_IDLE && s_rle && s_shot_comp) {
                    valid = s_shot_clen;        /* STOP 早停后 */
                } else {
                    valid = la_valid_now();     /* 采集中: 未压缩的原始进度 */
                }
            }
            uint16_t trig = 0;   /* Pico 全 post-trigger, 触发点固定 0 */
            rsp[0] = (uint8_t)s_phase;
            rsp[1] = (uint8_t)(valid & 0xFF);
            rsp[2] = (uint8_t)(valid >> 8);
            rsp[3] = (uint8_t)(trig & 0xFF);
            rsp[4] = (uint8_t)(trig >> 8);
            rsp[5] = s_cont ? 1 : 0;
            rsp[6] = s_ready;
            rsp[7] = s_seq;
            rsp[8] = s_rle ? 1u : 0u;           /* flags: bit0 = RLE 生效 */
            rsp[9] = (uint8_t)(clen & 0xFF);    /* 连续模式: 最后完成半区的压缩长度 */
            rsp[10] = (uint8_t)(clen >> 8);
            la_send(LA_CMD_STATUS, rsp, 11);
            return;
        }

        case LA_CMD_READ: {
            if (!s_hw_open || len < 4) { ack = 0xEE; break; }
            uint32_t off = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8);
            uint32_t n   = (uint32_t)pl[2] | ((uint32_t)pl[3] << 8);
            if (n > LA_READ_MAX) n = LA_READ_MAX;
            /* 有效数据上界: RLE 时为压缩流长度, 否则全深度 */
            uint32_t limit = LA_DEPTH_SAMPLES;
            if (s_rle) {
                if (s_cont) {
                    uint32_t h = (off < LA_HALF_SAMPLES) ? 0u : 1u;
                    limit = h * LA_HALF_SAMPLES + s_half_clen[h];
                } else {
                    limit = s_shot_clen;
                }
            }
            if (off + n > limit) {
                n = (off < limit) ? (limit - off) : 0;
            }
            if (n > 0) {
                la_send(LA_CMD_READ, (const uint8_t *)LA_BUF + off, (uint16_t)n);
                /* 连续模式: 半区被读走后清 ready 位 */
                if (s_cont && n > 0) {
                    uint8_t bit = (off < LA_HALF_SAMPLES) ? 1u : 2u;
                    s_ready &= (uint8_t)~bit;
                }
            } else {
                la_send(LA_CMD_READ, (const uint8_t *)"", 0);
            }
            return;
        }

        case LA_CMD_SET_MODE:
            if (len >= 1) {
                la_stop_capture();
                s_cont = (pl[0] & 1u) != 0;
                la_rle_reset();      /* 单次/连续的缓冲语义不同, 作废旧压缩状态 */
            }
            break;

        case LA_CMD_SET_RLE:
            if (len >= 1) {
                bool en = (pl[0] & 1u) != 0;
                /* 状态不变时不停止采集: 上位机改采样率等操作会重发 pushConfig,
                 * 若每次都 stop 会把进行中的采集静默杀掉 */
                if (en != s_rle) {
                    la_stop_capture();
                    s_rle = en;
                    la_rle_reset();
                }
            }
            break;

        default:
            ack = 0xEE;
            break;
    }

    la_send(cmd, &ack, 1);
}

/* ================= 下行帧解析状态机 ================= */
enum { P_HUNT, P_CMD, P_LEN, P_PAY, P_CRC };

static uint8_t p_st = P_HUNT, p_cmd, p_len, p_idx, p_crc;
static uint8_t p_buf[8];

static void la_feed_byte(uint8_t b) {
    switch (p_st) {
        case P_HUNT:
            if (b == 0xA5) p_st = P_CMD;
            break;
        case P_CMD:
            p_cmd = b;
            p_crc = la_crc8(0, &b, 1);
            p_st  = P_LEN;
            break;
        case P_LEN:
            p_len = b;
            p_crc = la_crc8(p_crc, &b, 1);
            p_idx = 0;
            p_st  = (b == 0) ? P_CRC : P_PAY;
            break;
        case P_PAY:
            if (p_idx < 8) p_buf[p_idx] = b;
            p_crc = la_crc8(p_crc, &b, 1);
            p_idx++;
            if (p_idx >= p_len) p_st = P_CRC;
            break;
        case P_CRC:
            p_st = P_HUNT;
            if (b == p_crc && p_len <= 8) {
                la_exec_cmd(p_cmd, p_buf, p_len);
            }
            break;
        default:
            p_st = P_HUNT;
            break;
    }
}

/* ================= 主循环轮询入口 ================= */
void la_usb_poll(void) {
    /* 连续模式 + RLE: 半区一完成就在主循环里压缩 (写指针恒 <= 读指针,
     * 压缩的是刚完成的半区, DMA 正在写另一半区, 互不干扰)。
     * 只压最新半区: 上位机没跟上 (delta>1) 时旧半区反正已被覆盖, 直接放弃。 */
    if (s_cont && s_rle && s_hw_open && s_phase == LA_PHASE_CAPTURING) {
        uint8_t s = s_seq;
        uint8_t d = (uint8_t)(s - s_comp_seq);
        if (d == 1u) {
            uint32_t h = (uint32_t)(s - 1) & 1u;
            s_half_clen[h] = la_rle_compress((uint8_t *)LA_BUF + h * LA_HALF_SAMPLES,
                                             LA_HALF_SAMPLES);
            s_comp_seq = s;
        } else if (d > 1u) {
            s_half_clen[(uint32_t)(s - 1) & 1u] = 0;   /* 溢出: 本半区放弃 */
            s_comp_seq = s;
        }
    }

    if (s_hw_open && s_phase != LA_PHASE_WAIT_TRIG && s_phase != LA_PHASE_CAPTURING) {
        /* 非采集中 60 秒无命令 → 归还 RAM 池与硬件, plugin 恢复可加载 */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - s_last_cmd_ms > 60000u) la_hw_release();
    }

    if (!tud_ready()) return;

    uint32_t navail = tud_cdc_n_available(0);
    if (navail == 0) return;

    uint8_t rbuf[64];
    while (navail > 0) {
        uint32_t n = tud_cdc_n_read(0, rbuf, sizeof(rbuf));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) la_feed_byte(rbuf[i]);
        navail = tud_cdc_n_available(0);
    }
}

/* ================= DMA_IRQ_0 ================= */
static void __not_in_flash_func(la_dma_irq_handler)(void) {
    if (!s_hw_open) {
        dma_hw->ints0 = dma_hw->intr;   /* 清掉全部, 防止风暴 */
        return;
    }

    uint32_t intr = dma_hw->intr;

    if (intr & (1u << s_dma_a)) {
        if (s_cont) {
            /* 半区0 完成: chain 已经把 B 拉起来了, 这里只需把 A 重配好
               (count 回满 + 写地址回半区0), 等 B 完成时被 chain 再拉起 */
            dma_channel_set_write_addr((uint)s_dma_a, LA_BUF, false);
            dma_channel_set_trans_count((uint)s_dma_a, LA_HALF_WORDS, false);
            s_seq++;
            s_ready |= 1u;
        } else {
            /* 单次: 全深度采完。B 已被 chain 拉去当下水道, 果断停掉 */
            dma_channel_abort((uint)s_dma_b);
            dma_channel_set_irq0_enabled((uint)s_dma_a, false);
            pio_sm_set_enabled(s_pio, s_sm, false);
            s_phase = LA_PHASE_DONE;
        }
    }

    if (s_cont && (intr & (1u << s_dma_b))) {
        dma_channel_set_write_addr((uint)s_dma_b, LA_BUF + LA_HALF_WORDS, false);
        dma_channel_set_trans_count((uint)s_dma_b, LA_HALF_WORDS, false);
        s_seq++;
        s_ready |= 2u;
    }

    dma_hw->ints0 = intr;   /* 清本次中断 */
}
