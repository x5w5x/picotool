#include "cmsis_dap.h"
#include "fun_pio_swd.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include <string.h>

#define PIN_SWCLK           2   // Pico GP2 (Pin 4) -> 目标 SWCLK
#define PIN_SWDIO           3   // Pico GP3 (Pin 5) -> 目标 SWDIO

#define SWCLK_HIGH()        sio_hw->gpio_set = (1u << PIN_SWCLK)
#define SWCLK_LOW()         sio_hw->gpio_clr = (1u << PIN_SWCLK)
#define SWDIO_HIGH()        sio_hw->gpio_set = (1u << PIN_SWDIO)
#define SWDIO_LOW()         sio_hw->gpio_clr = (1u << PIN_SWDIO)
#define SWDIO_READ()        ((sio_hw->gpio_in & (1u << PIN_SWDIO)) ? 1 : 0)

volatile bool     g_dap_enabled        = false;
volatile uint32_t g_dap_packet_counter = 0;
volatile uint32_t g_dap_cur_clock_hz   = 5000000;

static volatile uint32_t s_dap_delay_cnt = 6;
static uint8_t           s_idle_cycles   = 0;
static uint16_t          s_retry_count   = 128;
static uint16_t          s_match_retry   = 8192;
static uint8_t           s_nreset_pin    = 1;
static uint32_t          s_match_mask    = 0xFFFFFFFF;
/* 当前仍默认使用软件 SWD。PIO 后端接口完成后，只需切换此变量即可。 */
static cmsis_dap_backend_t s_backend = CMSIS_DAP_BACKEND_SOFTWARE;
static uint8_t dap_backend_transfer_word(uint8_t req, uint32_t *data);

void cmsis_dap_set_backend(cmsis_dap_backend_t backend)
{
    /* PIO 尚未覆盖完整 CMSIS-DAP 传输，暂不允许半成品后端接管 USB。 */
    s_backend = (backend == CMSIS_DAP_BACKEND_PIO)
                    ? CMSIS_DAP_BACKEND_PIO : CMSIS_DAP_BACKEND_SOFTWARE;
}

cmsis_dap_backend_t cmsis_dap_get_backend(void)
{
    return s_backend;
}

static inline void swd_delay(void) {
    for (volatile uint32_t i = 0; i < s_dap_delay_cnt; i++) {
        __asm volatile("nop");
    }
}

static inline void swdio_out(void) { gpio_set_dir(PIN_SWDIO, GPIO_OUT); }
static inline void swdio_in(void)  { gpio_set_dir(PIN_SWDIO, GPIO_IN); gpio_pull_up(PIN_SWDIO); }

static inline void swd_write_bit(uint8_t bit) {
    if (bit) SWDIO_HIGH(); else SWDIO_LOW();
    swd_delay();
    SWCLK_LOW();
    swd_delay();
    SWCLK_HIGH();
}

static inline uint8_t swd_read_bit(void) {
    uint8_t bit;
    SWCLK_LOW();
    swd_delay();
    bit = SWDIO_READ();
    SWCLK_HIGH();
    swd_delay();
    return bit;
}

static inline uint8_t dap_gen_req(uint8_t dap_req) {
    uint8_t is_ap   = (dap_req & 0x01) ? 1 : 0;
    uint8_t is_read = (dap_req & 0x02) ? 1 : 0;
    uint8_t a2      = (dap_req & 0x04) ? 1 : 0;
    uint8_t a3      = (dap_req & 0x08) ? 1 : 0;

    uint8_t req = 0x81;
    if (is_ap)   req |= (1 << 1);
    if (is_read) req |= (1 << 2);
    if (a2)      req |= (1 << 3);
    if (a3)      req |= (1 << 4);
    if (is_ap ^ is_read ^ a2 ^ a3) {
        req |= (1 << 5);
    }
    return req;
}

static uint8_t dap_swd_transfer_word(uint8_t req, uint32_t *data) {
    uint8_t ack = 0;
    uint8_t p = 0;

    swdio_out();
    for (int i = 0; i < 8; i++) {
        swd_write_bit((req >> i) & 1);
    }

    swdio_in();
    swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();

    ack |= swd_read_bit() << 0;
    ack |= swd_read_bit() << 1;
    ack |= swd_read_bit() << 2;

    if (ack == 0x01) {
        if (req & (1 << 2)) { // Read
            uint32_t val = 0;
            for (int i = 0; i < 32; i++) {
                uint8_t b = swd_read_bit();
                if (b) val |= (1UL << i);
                p ^= b;
            }
            uint8_t parity_bit = swd_read_bit();
            if (p != parity_bit) ack = 0x04;

            swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
            swdio_out();
            SWDIO_LOW();
            if (data) *data = val;
        } else { // Write
            swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
            swdio_out();

            uint32_t val = *data;
            p = 0;
            for (int i = 0; i < 32; i++) {
                uint8_t b = (val >> i) & 1;
                swd_write_bit(b);
                p ^= b;
            }
            swd_write_bit(p);
            SWDIO_LOW();
        }
    } else {
        swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
        swdio_out();
        SWDIO_LOW();
    }

    for (int i = 0; i < s_idle_cycles + 8; i++) {
        SWCLK_LOW(); swd_delay(); SWCLK_HIGH(); swd_delay();
    }
    return ack;
}

static void target_mem_write(uint32_t addr, uint32_t val) {
    uint32_t tmp;
    tmp = 0x00000000; dap_backend_transfer_word(dap_gen_req(0x08), &tmp); // DP SELECT = 0
    tmp = 0x23000002; dap_backend_transfer_word(dap_gen_req(0x01), &tmp); // AP CSW = 32-bit
    tmp = addr;       dap_backend_transfer_word(dap_gen_req(0x05), &tmp); // AP TAR = addr
    dap_backend_transfer_word(dap_gen_req(0x0D), &val);                    // AP DRW = val
    dap_backend_transfer_word(dap_gen_req(0x0E), &tmp);                    // DP RDBUFF 提交
}

/* ================= 核心 DAP 指令分发器 ================= */

uint32_t dap_process_command(const uint8_t *req, uint8_t *resp) {
    uint8_t cmd = req[0];
    resp[0] = cmd;
    uint32_t resp_len = 1;

    switch (cmd) {
        case ID_DAP_Info: {
            uint8_t info_id = req[1];
            switch (info_id) {
                case DAP_INFO_VENDOR:
                    resp[1] = 9;
                    memcpy(&resp[2], "Raspberry", 9);
                    resp_len = 2 + 9;
                    break;
                case DAP_INFO_PRODUCT:
                    resp[1] = 14;
                    memcpy(&resp[2], "Pico CMSIS-DAP", 14);
                    resp_len = 2 + 14;
                    break;
                case DAP_INFO_SER_NUM:
                    resp[1] = 8;
                    memcpy(&resp[2], "12345678", 8);
                    resp_len = 2 + 8;
                    break;
                case DAP_INFO_FW_VER:
                    resp[1] = 6;
                    memcpy(&resp[2], "v1.1.0", 6);
                    resp_len = 2 + 6;
                    break;
                case DAP_INFO_CAPABILITIES:
                    resp[1] = 1;
                    resp[2] = 0x01; // SWD Only
                    resp_len = 3;
                    break;
                case DAP_INFO_PACKET_COUNT:
                    resp[1] = 1;
                    resp[2] = 1;
                    resp_len = 3;
                    break;
                case DAP_INFO_PACKET_SIZE:
                    resp[1] = 2;
                    resp[2] = 64;
                    resp[3] = 0;
                    resp_len = 4;
                    break;
                default:
                    resp[1] = 0;
                    resp_len = 2;
                    break;
            }
            break;
        }

        case ID_DAP_HostStatus: {
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_Connect: {
            gpio_init(PIN_SWCLK);
            gpio_init(PIN_SWDIO);
            gpio_set_dir(PIN_SWCLK, GPIO_OUT);
            gpio_set_dir(PIN_SWDIO, GPIO_OUT);
            gpio_pull_up(PIN_SWDIO);

            gpio_set_drive_strength(PIN_SWCLK, GPIO_DRIVE_STRENGTH_8MA);
            gpio_set_drive_strength(PIN_SWDIO, GPIO_DRIVE_STRENGTH_8MA);
            gpio_set_slew_rate(PIN_SWCLK, GPIO_SLEW_RATE_FAST);
            gpio_set_slew_rate(PIN_SWDIO, GPIO_SLEW_RATE_FAST);

            SWCLK_HIGH();
            SWDIO_HIGH();
            resp[1] = 0x01;
            resp_len = 2;
            break;
        }

        case ID_DAP_Disconnect: {
            gpio_set_dir(PIN_SWCLK, GPIO_IN);
            gpio_set_dir(PIN_SWDIO, GPIO_IN);
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_SWJ_Pins: {
            uint8_t pin_val = req[1];
            uint8_t pin_sel = req[2];
            uint32_t wait_us = req[3] | (req[4] << 8) | (req[5] << 16) | (req[6] << 24);

            if (pin_sel & 0x01) {
                if (pin_val & 0x01) SWCLK_HIGH(); else SWCLK_LOW();
            }
            if (pin_sel & 0x02) {
                if (pin_val & 0x02) SWDIO_HIGH(); else SWDIO_LOW();
            }
            if (pin_sel & 0x80) {
                s_nreset_pin = (pin_val & 0x80) ? 1 : 0;
                if (s_nreset_pin == 0) {
                    // ⚡ 软复位 (无 nRESET 硬件线):通过 SWD 写 AIRCR.SYSRESETREQ
                    // 完整序列:DP SELECT=0 → AP CSW=32bit → TAR=AIRCR → DRW=0x05FA0004 → RDBUFF 提交
                    target_mem_write(0xE000ED0C, 0x05FA0004);
                }
            }

            if (wait_us) sleep_us(wait_us);

            uint8_t in_val = (gpio_get(PIN_SWCLK) ? 0x01 : 0) |
                             (gpio_get(PIN_SWDIO) ? 0x02 : 0) |
                             (s_nreset_pin ? 0x80 : 0x00);
            resp[1] = in_val;
            resp_len = 2;
            break;
        }

        case ID_DAP_SWJ_Clock: {
            uint32_t clk_hz = req[1] | (req[2] << 8) | (req[3] << 16) | (req[4] << 24);
            g_dap_cur_clock_hz = clk_hz;
            if (clk_hz >= 10000000)     s_dap_delay_cnt = 2;  // ~10MHz
            else if (clk_hz >= 5000000) s_dap_delay_cnt = 6;  // ~5MHz
            else if (clk_hz >= 2000000) s_dap_delay_cnt = 16; // ~2.5MHz
            else if (clk_hz >= 1000000) s_dap_delay_cnt = 35; // ~1.2MHz
            else                        s_dap_delay_cnt = 80; // ~500kHz
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_SWJ_Sequence: {
            uint16_t bit_cnt = req[1] ? req[1] : 256;
            const uint8_t *p_data = &req[2];
            swdio_out();
            for (uint16_t i = 0; i < bit_cnt; i++) {
                swd_write_bit((p_data[i / 8] >> (i % 8)) & 1);
            }
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_SWD_Configure: {
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_TransferConfigure: {
            s_idle_cycles = req[1];
            s_retry_count = req[2] | (req[3] << 8);
            s_match_retry = req[4] | (req[5] << 8);

            if (s_retry_count < 100) s_retry_count = 100;
            if (s_match_retry < 8192) s_match_retry = 8192;

            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_Transfer: {
            uint8_t count = req[2];
            const uint8_t *p_req = &req[3];
            uint8_t *p_resp_data = &resp[3];

            resp[1] = 0;
            resp[2] = 0x01;
            resp_len = 3;

            uint8_t post_read = 0;
            uint8_t check_write = 0;
            uint8_t ack = 0x01;
            uint32_t data_val = 0;

            for (uint8_t i = 0; i < count; i++) {
                uint8_t transfer_req = *p_req++;

                if (transfer_req & 0x20) { // Match Mask
                    s_match_mask = p_req[0] | (p_req[1] << 8) | (p_req[2] << 16) | (p_req[3] << 24);
                    p_req += 4;
                    resp[1]++;
                    continue;
                }

                if (transfer_req & 0x02) { // Read
                    if (transfer_req & 0x10) { // Value Match
                        uint32_t match_val = p_req[0] | (p_req[1] << 8) | (p_req[2] << 16) | (p_req[3] << 24);
                        p_req += 4;

                        if (post_read) {
                            uint32_t dummy = 0;
                            for (uint16_t r = 0; r < s_retry_count; r++) {
                                ack = dap_swd_transfer_word(dap_gen_req(0x0E), &dummy);
                                if (ack != 0x02) break;
                            }
                            post_read = 0;
                        }

                        uint32_t match_cnt = s_match_retry;
                        for (;;) {
                            for (uint16_t r = 0; r < s_retry_count; r++) {
                                ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                                if (ack != 0x02) break;
                            }
                            if (ack != 0x01) break;

                            if (transfer_req & 0x01) {
                                for (uint16_t r = 0; r < s_retry_count; r++) {
                                    ack = dap_swd_transfer_word(dap_gen_req(0x0E), &data_val);
                                    if (ack != 0x02) break;
                                }
                                if (ack != 0x01) break;
                            }

                            if ((data_val & s_match_mask) == match_val) break;

                            if (match_cnt-- == 0) {
                                ack = 0x04;
                                break;
                            }
                            sleep_us(10);
                        }

                        if (ack != 0x01) { resp[2] = ack; break; }
                        resp[1]++;
                        check_write = 0;
                    } else { // 常规 Read
                        if (transfer_req & 0x01) { // AP Read
                            if (post_read) {
                                for (uint16_t r = 0; r < s_retry_count; r++) {
                                    ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                                    if (ack != 0x02) break;
                                }
                                if (ack != 0x01) { resp[2] = ack; break; }

                                *p_resp_data++ = (uint8_t)(data_val >>  0);
                                *p_resp_data++ = (uint8_t)(data_val >>  8);
                                *p_resp_data++ = (uint8_t)(data_val >> 16);
                                *p_resp_data++ = (uint8_t)(data_val >> 24);
                                resp_len += 4;
                            } else {
                                for (uint16_t r = 0; r < s_retry_count; r++) {
                                    ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                                    if (ack != 0x02) break;
                                }
                                if (ack != 0x01) { resp[2] = ack; break; }
                                post_read = 1;
                            }
                        } else { // DP Read
                            if (post_read) {
                                uint32_t prev_data = 0;
                                for (uint16_t r = 0; r < s_retry_count; r++) {
                                    ack = dap_swd_transfer_word(dap_gen_req(0x0E), &prev_data);
                                    if (ack != 0x02) break;
                                }
                                if (ack != 0x01) { resp[2] = ack; break; }

                                *p_resp_data++ = (uint8_t)(prev_data >>  0);
                                *p_resp_data++ = (uint8_t)(prev_data >>  8);
                                *p_resp_data++ = (uint8_t)(prev_data >> 16);
                                *p_resp_data++ = (uint8_t)(prev_data >> 24);
                                resp_len += 4;
                                post_read = 0;
                            }

                            for (uint16_t r = 0; r < s_retry_count; r++) {
                                ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                                if (ack != 0x02) break;
                            }
                            if (ack != 0x01) { resp[2] = ack; break; }

                            *p_resp_data++ = (uint8_t)(data_val >>  0);
                            *p_resp_data++ = (uint8_t)(data_val >>  8);
                            *p_resp_data++ = (uint8_t)(data_val >> 16);
                            *p_resp_data++ = (uint8_t)(data_val >> 24);
                            resp_len += 4;
                        }
                        resp[1]++;
                        check_write = 0;
                    }
                } else { // Write
                    data_val = p_req[0] | (p_req[1] << 8) | (p_req[2] << 16) | (p_req[3] << 24);
                    p_req += 4;

                    if (post_read) {
                        uint32_t prev_data = 0;
                        for (uint16_t r = 0; r < s_retry_count; r++) {
                            ack = dap_swd_transfer_word(dap_gen_req(0x0E), &prev_data);
                            if (ack != 0x02) break;
                        }
                        if (ack != 0x01) { resp[2] = ack; break; }

                        *p_resp_data++ = (uint8_t)(prev_data >>  0);
                        *p_resp_data++ = (uint8_t)(prev_data >>  8);
                        *p_resp_data++ = (uint8_t)(prev_data >> 16);
                        *p_resp_data++ = (uint8_t)(prev_data >> 24);
                        resp_len += 4;
                        post_read = 0;
                    }

                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                        if (ack != 0x02) break;
                    }
                    if (ack != 0x01) { resp[2] = ack; break; }
                    resp[1]++;
                    check_write = 1;
                }
            }

            if (resp[2] == 0x01) {
                if (post_read) {
                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(0x0E), &data_val);
                        if (ack != 0x02) break;
                    }
                    if (ack != 0x01) {
                        resp[2] = ack;
                    } else {
                        *p_resp_data++ = (uint8_t)(data_val >>  0);
                        *p_resp_data++ = (uint8_t)(data_val >>  8);
                        *p_resp_data++ = (uint8_t)(data_val >> 16);
                        *p_resp_data++ = (uint8_t)(data_val >> 24);
                        resp_len += 4;
                    }
                } else if (check_write) {
                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(0x0E), &data_val);
                        if (ack != 0x02) break;
                    }
                    if (ack != 0x01) resp[2] = ack;
                }
            }
            break;
        }

        case ID_DAP_TransferBlock: {
            uint16_t block_count = req[2] | (req[3] << 8);
            uint8_t transfer_req = req[4];
            const uint8_t *p_req = &req[5];
            uint8_t *p_resp_data = &resp[4];

            resp[1] = 0; resp[2] = 0;
            resp[3] = 0x01;
            resp_len = 4;

            uint16_t transferred = 0;
            uint8_t ack = 0x01;
            uint32_t data_val = 0;

            if (transfer_req & 0x02) { // Block Read
                if (transfer_req & 0x01) { // AP Block Read
                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                        if (ack != 0x02) break;
                    }
                    if (ack == 0x01) {
                        for (uint16_t i = 0; i < block_count - 1; i++) {
                            for (uint16_t r = 0; r < s_retry_count; r++) {
                                ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                                if (ack != 0x02) break;
                            }
                            if (ack != 0x01) break;

                            transferred++;
                            *p_resp_data++ = (uint8_t)(data_val >>  0);
                            *p_resp_data++ = (uint8_t)(data_val >>  8);
                            *p_resp_data++ = (uint8_t)(data_val >> 16);
                            *p_resp_data++ = (uint8_t)(data_val >> 24);
                            resp_len += 4;
                        }

                        if (ack == 0x01) {
                            for (uint16_t r = 0; r < s_retry_count; r++) {
                                ack = dap_swd_transfer_word(dap_gen_req(0x0E), &data_val);
                                if (ack != 0x02) break;
                            }
                            if (ack == 0x01) {
                                transferred++;
                                *p_resp_data++ = (uint8_t)(data_val >>  0);
                                *p_resp_data++ = (uint8_t)(data_val >>  8);
                                *p_resp_data++ = (uint8_t)(data_val >> 16);
                                *p_resp_data++ = (uint8_t)(data_val >> 24);
                                resp_len += 4;
                            }
                        }
                    }
                    resp[3] = ack;
                } else { // DP Block Read
                    for (uint16_t i = 0; i < block_count; i++) {
                        for (uint16_t r = 0; r < s_retry_count; r++) {
                            ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                            if (ack != 0x02) break;
                        }
                        if (ack != 0x01) break;

                        transferred++;
                        *p_resp_data++ = (uint8_t)(data_val >>  0);
                        *p_resp_data++ = (uint8_t)(data_val >>  8);
                        *p_resp_data++ = (uint8_t)(data_val >> 16);
                        *p_resp_data++ = (uint8_t)(data_val >> 24);
                        resp_len += 4;
                    }
                    resp[3] = ack;
                }
            } else { // Block Write
                for (uint16_t i = 0; i < block_count; i++) {
                    data_val = p_req[0] | (p_req[1] << 8) | (p_req[2] << 16) | (p_req[3] << 24);
                    p_req += 4;

                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(transfer_req), &data_val);
                        if (ack != 0x02) break;
                    }
                    if (ack != 0x01) break;
                    transferred++;
                }

                if (ack == 0x01) {
                    for (uint16_t r = 0; r < s_retry_count; r++) {
                        ack = dap_swd_transfer_word(dap_gen_req(0x0E), &data_val);
                        if (ack != 0x02) break;
                    }
                }
                resp[3] = ack;
            }

            resp[1] = (uint8_t)(transferred & 0xFF);
            resp[2] = (uint8_t)((transferred >> 8) & 0xFF);
            break;
        }

        case ID_DAP_TransferAbort: {
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_WriteABORT: {
            uint32_t abort_val = req[2] | (req[3] << 8) | (req[4] << 16) | (req[5] << 24);
            dap_swd_transfer_word(dap_gen_req(0x00), &abort_val);
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        case ID_DAP_Delay: {
            uint16_t delay_us = req[1] | (req[2] << 8);
            sleep_us(delay_us);
            resp[1] = DAP_OK;
            resp_len = 2;
            break;
        }

        // ⚡ 标准复位：关闭捕获 -> 释放 Halt -> 触发系统复位
        case ID_DAP_ResetTarget: {
            // ⚡ 安全单次软件复位：防止连续写总线造成 Pico 挂起
            uint32_t tmp;
            // 1. 发送一次复位请求 (AIRCR.SYSRESETREQ)
            tmp = 0x00000000; dap_swd_transfer_word(dap_gen_req(0x08), &tmp); // DP SELECT = 0
            tmp = 0x23000002; dap_swd_transfer_word(dap_gen_req(0x01), &tmp); // AP CSW
            tmp = 0xE000ED0C; dap_swd_transfer_word(dap_gen_req(0x05), &tmp); // AP TAR = AIRCR
            tmp = 0x05FA0004; dap_swd_transfer_word(dap_gen_req(0x0D), &tmp); // AP DRW = SYSRESETREQ
            dap_swd_transfer_word(dap_gen_req(0x0E), &tmp);                    // 提交

            sleep_us(500); // 极短延时等待目标板硬件重启

            // 2. 释放内核挂起，允许全速运行
            tmp = 0xE000EDF0; dap_swd_transfer_word(dap_gen_req(0x05), &tmp); // AP TAR = DHCSR
            tmp = 0xA05F0000; dap_swd_transfer_word(dap_gen_req(0x0D), &tmp); // AP DRW = 释放
            dap_swd_transfer_word(dap_gen_req(0x0E), &tmp);                    // 提交

            resp[1] = DAP_OK;
            resp[2] = 0x00; // 0x00 告知 Keil 复位已由调试器在内部完全处理完毕
            resp_len = 3;
            break;
        }

        default:
            resp[1] = DAP_ERROR;
            resp_len = 2;
            break;
    }

    return resp_len;
}

/* CMSIS-DAP 统一传输入口。
 * 目前 PIO 后端尚未实现完整的 AP/DP 传输，因此继续走已验证的软件实现；
 * 后续只需在这里根据 s_backend 分派，不需要修改上层 Transfer/Block 逻辑。 */
static uint8_t dap_backend_transfer_word(uint8_t req, uint32_t *data)
{
    if (s_backend == CMSIS_DAP_BACKEND_PIO) {
        /* 保留切换点：PIO 支持完整传输后在此调用 fun_pio_swd_transfer()。 */
    }
    return dap_swd_transfer_word(req, data);
}
