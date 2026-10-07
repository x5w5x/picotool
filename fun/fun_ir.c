#include "fun_ir.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define IR_TIMEOUT_US       40000 // 40ms 持续静默判定整帧结束
#define IR_MIN_PULSE_US     80    // 滤除小于 80us 的光电高频毛刺噪声

static uint8_t              s_rx_pin = IR_DEFAULT_RX_PIN;
static uint8_t              s_tx_pin = IR_DEFAULT_TX_PIN;
static uint                 s_tx_pwm_slice = 0;

static volatile bool        s_rx_active = false;
static volatile bool        s_packet_ready = false;
static volatile uint32_t    s_live_edge_count = 0;

/* ⚡ 外部共享内存指针 (零静态全局 BSS 内存占用) */
static uint16_t            *s_pulse_buf = NULL;
static ir_packet_t         *s_captured_packet = NULL;

/* ================= 1. 硬件探针与诊断接口 ================= */
bool fun_ir_get_pin_level(void) {
    return gpio_get(s_rx_pin);
}

uint32_t fun_ir_get_edge_count(void) {
    return s_live_edge_count;
}

/* ================= 2. Core 1 独占微秒高精度轮询 ================= */
static void core1_ir_worker(void) {
    while (s_rx_active) {
        if (!s_pulse_buf || !s_captured_packet) break;

        // A. 等待空闲高电平 (VS1838B 默认上拉为 1)
        if (gpio_get(s_rx_pin) == 0) {
            uint32_t wait_high = time_us_32();
            while (gpio_get(s_rx_pin) == 0 && s_rx_active) {
                if (time_us_32() - wait_high > 50000) break;
                tight_loop_contents();
            }
        }

        // B. 等待红外前导码下降沿 (变 0 表示捕获到载波)
        while (gpio_get(s_rx_pin) == 1 && s_rx_active) {
            tight_loop_contents();
        }
        if (!s_rx_active) break;

        // C. 开始微秒差值测宽
        uint32_t last_time = time_us_32();
        bool last_state = 0;
        uint16_t count = 0;

        while (s_rx_active) {
            uint32_t now = time_us_32();
            bool cur_state = gpio_get(s_rx_pin);

            if (cur_state != last_state) {
                uint32_t dur = now - last_time;
                last_time = now;
                last_state = cur_state;
                s_live_edge_count++;

                if (dur >= IR_MIN_PULSE_US) {
                    if (count < IR_MAX_PULSES) {
                        s_pulse_buf[count++] = (dur > 65535) ? 65535 : (uint16_t)dur;
                    }
                }
            } else {
                // 超时判定：静默超过 40ms，整包接收完毕
                if (now - last_time > IR_TIMEOUT_US) {
                    if (count >= 4) {
                        s_captured_packet->pulse_count = count;
                        memcpy(s_captured_packet->pulses, (const void *)s_pulse_buf, count * sizeof(uint16_t));
                        s_captured_packet->protocol = IR_PROTO_RAW;
                        fun_ir_decode_nec(s_captured_packet);
                        s_packet_ready = true;
                    }
                    break;
                }
            }
        }
    }

    while (true) tight_loop_contents();
}

/* ================= 3. 驱动底层初始化与销毁 ================= */
bool fun_ir_init(uint8_t rx_pin, uint8_t tx_pin) {
    s_rx_pin = rx_pin;
    s_tx_pin = tx_pin;

    // RX 配置
    gpio_init(s_rx_pin);
    gpio_set_dir(s_rx_pin, GPIO_IN);
    gpio_pull_up(s_rx_pin);

    // TX 配置 (38kHz PWM 载波，33% 占空比)
    gpio_set_function(s_tx_pin, GPIO_FUNC_PWM);
    s_tx_pwm_slice = pwm_gpio_to_slice_num(s_tx_pin);

    pwm_config cfg = pwm_get_default_config();
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t wrap = (sys_clk / 38000) - 1;
    pwm_config_set_wrap(&cfg, wrap);
    pwm_init(s_tx_pwm_slice, &cfg, false);

    pwm_set_gpio_level(s_tx_pin, wrap / 3);
    pwm_set_enabled(s_tx_pwm_slice, false);

    return true;
}

void fun_ir_deinit(void) {
    fun_ir_rx_stop();
    pwm_set_enabled(s_tx_pwm_slice, false);
    gpio_init(s_tx_pin);
    gpio_put(s_tx_pin, 0);
}

void fun_ir_rx_start(uint16_t *sample_buf, ir_packet_t *pkt_out) {
    s_pulse_buf = sample_buf;
    s_captured_packet = pkt_out;
    s_packet_ready = false;
    s_live_edge_count = 0;
    s_rx_active = true;

    multicore_reset_core1();
    multicore_launch_core1(core1_ir_worker);
}

void fun_ir_rx_stop(void) {
    s_rx_active = false;
    multicore_reset_core1();
    s_pulse_buf = NULL;
    s_captured_packet = NULL;
}

/* ================= 4. 解码、轮询与 38kHz 重放 ================= */
bool fun_ir_decode_nec(ir_packet_t *packet) {
    if (!packet || packet->pulse_count < 67) return false;

    // NEC 前导码判定 (Mark ~9000us, Space ~4500us，允许 20% 容差)
    if (packet->pulses[0] < 6800 || packet->pulses[0] > 11000) return false;
    if (packet->pulses[1] < 3400 || packet->pulses[1] > 5600) return false;

    uint32_t raw_data = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t space = packet->pulses[2 + i * 2 + 1];
        if (space > 1100) {
            raw_data |= (1UL << i);
        }
    }

    uint8_t addr     = (raw_data >> 0) & 0xFF;
    uint8_t addr_inv = (raw_data >> 8) & 0xFF;
    uint8_t cmd      = (raw_data >> 16) & 0xFF;
    uint8_t cmd_inv  = (raw_data >> 24) & 0xFF;

    packet->nec_addr  = addr;
    packet->nec_cmd   = cmd;
    packet->nec_valid = (cmd == (uint8_t)(~cmd_inv));
    packet->protocol  = IR_PROTO_NEC;

    return packet->nec_valid;
}

bool fun_ir_rx_poll(ir_packet_t *packet) {
    if (!s_packet_ready || !packet || !s_captured_packet) return false;
    *packet = *s_captured_packet;
    s_packet_ready = false;
    return true;
}

void fun_ir_tx_send(const ir_packet_t *packet) {
    if (!packet || packet->pulse_count == 0) return;

    uint32_t irq_status = save_and_disable_interrupts();

    for (uint16_t i = 0; i < packet->pulse_count; i++) {
        uint16_t duration = packet->pulses[i];

        if ((i & 1) == 0) {
            // 偶数索引: Mark (开启 38kHz PWM 调制波)
            pwm_set_enabled(s_tx_pwm_slice, true);
        } else {
            // 奇数索引: Space (关闭载波，GPIO 保持低电平)
            pwm_set_enabled(s_tx_pwm_slice, false);
            gpio_put(s_tx_pin, 0);
        }
        busy_wait_us(duration);
    }

    pwm_set_enabled(s_tx_pwm_slice, false);
    gpio_put(s_tx_pin, 0);

    restore_interrupts(irq_status);
}

/* ================= 5. TF 卡波形文件持久化与流式解析 ================= */
bool fun_ir_save_auto(const ir_packet_t *packet, char *out_path, size_t max_len) {
    if (!packet || packet->pulse_count == 0) return false;

    f_mkdir("0:/ir");

    char filepath[48];
    FILINFO fno;
    uint16_t file_idx = 1;
    while (file_idx < 1000) {
        snprintf(filepath, sizeof(filepath), "0:/ir/raw_%03u.ir", file_idx);
        if (f_stat(filepath, &fno) == FR_NO_FILE) {
            break;
        }
        file_idx++;
    }

    FIL fil;
    if (f_open(&fil, filepath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return false;
    }

    char line_buf[128];
    UINT bw;

    snprintf(line_buf, sizeof(line_buf),
             "Filetype: IR library file\nVersion: 1\n#\nname: raw_%03u\ntype: raw\nfrequency: 38000\nduty_cycle: 0.33\ndata: ",
             file_idx);
    f_write(&fil, line_buf, strlen(line_buf), &bw);

    for (uint16_t i = 0; i < packet->pulse_count; i++) {
        snprintf(line_buf, sizeof(line_buf), "%u ", packet->pulses[i]);
        f_write(&fil, line_buf, strlen(line_buf), &bw);
    }
    f_write(&fil, "\n", 1, &bw);
    f_close(&fil);

    if (out_path && max_len > 0) {
        strncpy(out_path, filepath, max_len - 1);
        out_path[max_len - 1] = '\0';
    }
    return true;
}

// 避开 strcasecmp 避免拉入 Newlib locale 全家桶
static inline bool is_data_field_tag(const char *s) {
    return ((s[0] == 'd' || s[0] == 'D') &&
            (s[1] == 'a' || s[1] == 'A') &&
            (s[2] == 't' || s[2] == 'T') &&
            (s[3] == 'a' || s[3] == 'A') &&
            s[4] == ':' && s[5] == '\0');
}

/* ⚡ 逐词流式状态机读取器：完全避免 128 字节缓冲区腰斩长行的问题，不依赖 f_gets */
bool fun_ir_load_file(const char *filepath, ir_packet_t *packet) {
    if (!filepath || !packet) return false;

    FIL fil;
    if (f_open(&fil, filepath, FA_READ) != FR_OK) {
        return false;
    }

    packet->pulse_count = 0;
    packet->protocol = IR_PROTO_RAW;

    char token[16];
    uint8_t t_idx = 0;
    char c = 0;
    UINT br = 0;
    bool in_data_field = false;

    while (f_read(&fil, &c, 1, &br) == FR_OK && br > 0) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (t_idx > 0) {
                token[t_idx] = '\0';
                t_idx = 0;

                if (!in_data_field) {
                    if (is_data_field_tag(token)) {
                        in_data_field = true;
                    }
                } else {
                    uint32_t val = (uint32_t)strtoul(token, NULL, 10);
                    if (val > 0 && packet->pulse_count < IR_MAX_PULSES) {
                        packet->pulses[packet->pulse_count++] = (val > 65535) ? 65535 : (uint16_t)val;
                    }
                    if (c == '\n' && packet->pulse_count > 0) {
                        break;
                    }
                }
            }
        } else {
            if (t_idx < sizeof(token) - 1) {
                token[t_idx++] = c;
            }
        }
    }

    // 尾部 Token 补齐检查
    if (t_idx > 0 && in_data_field) {
        token[t_idx] = '\0';
        uint32_t val = (uint32_t)strtoul(token, NULL, 10);
        if (val > 0 && packet->pulse_count < IR_MAX_PULSES) {
            packet->pulses[packet->pulse_count++] = (val > 65535) ? 65535 : (uint16_t)val;
        }
    }

    f_close(&fil);

    if (packet->pulse_count >= 67) {
        fun_ir_decode_nec(packet);
    }
    return packet->pulse_count > 0;
}