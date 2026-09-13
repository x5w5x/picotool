#include "fun_pio_swd.h"
#include "swd.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include <string.h>

#define PIO_SWD_PROGRAM_NAME "swd_read"

static uint8_t make_request(bool ap, bool read, uint8_t address)
{
    uint8_t request = 0x81u;
    if (ap) request |= 1u << 1;
    if (read) request |= 1u << 2;
    request |= (address & 0x0cu) << 1;
    uint8_t parity = 0;
    for (uint8_t i = 1; i <= 4; ++i) parity ^= (request >> i) & 1u;
    if (parity) request |= 1u << 5;
    return request;
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16; value ^= value >> 8;
    value ^= value >> 4; value ^= value >> 2; value ^= value >> 1;
    return value & 1u;
}

void fun_pio_swd_init(fun_pio_swd_t *swd)
{
    if (swd) memset(swd, 0, sizeof(*swd));
}

static void line_reset_gpio(uint8_t clk, uint8_t io)
{
    gpio_set_function(clk, GPIO_FUNC_SIO);
    gpio_set_function(io, GPIO_FUNC_SIO);
    gpio_set_dir(clk, GPIO_OUT); gpio_set_dir(io, GPIO_OUT);
    gpio_put(clk, 1); gpio_put(io, 1);
    for (uint16_t i = 0; i < 64; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    /* JTAG-to-SWD 0xE79E, LSB first. */
    for (uint8_t i = 0; i < 16; ++i) {
        gpio_put(io, (0xE79Eu >> i) & 1u);
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    gpio_put(io, 1);
    for (uint16_t i = 0; i < 64; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    /* 与已验证的软件版本一致：切换序列后再发送16个低电平周期。 */
    gpio_put(io, 0);
    for (uint8_t i = 0; i < 16; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    gpio_put(io, 1);
}

bool fun_pio_swd_open_auto(fun_pio_swd_t *swd, pio_manager_t *manager,
                           uint8_t clk, uint8_t io, uint32_t hz)
{
    if (!swd || !manager || swd->opened || clk >= 30 || io >= 30 ||
        clk == io || hz == 0) return false;
    for (uint8_t block = 0; block < PIO_BLOCK_COUNT; ++block) {
        int sm_id = pio_manager_claim_sm_from(manager, block, PIO_SWD_PROGRAM_NAME);
        if (sm_id < 0) continue;
        uint8_t sm = (uint8_t)(sm_id % PIO_SM_COUNT);
        int offset = pio_manager_load_program(manager, block, PIO_SWD_PROGRAM_NAME,
                                              &swd_read_program);
        if (offset < 0) { pio_manager_release_sm(manager, block, sm); continue; }
        PIO pio = pio_manager_get_hardware(manager, block);
        /* 每个请求/ACK/数据位由低、高两个PIO指令周期组成。 */
        float div = (float)clock_get_hz(clk_sys) / ((float)hz * 2.0f);
        if (div < 1.0f) div = 1.0f;
        pio_sm_config cfg = swd_read_program_get_default_config((uint)offset);
        sm_config_set_out_pins(&cfg, io, 1);
        sm_config_set_set_pins(&cfg, io, 1);
        sm_config_set_in_pins(&cfg, io);
        sm_config_set_sideset_pins(&cfg, clk);
        sm_config_set_out_shift(&cfg, true, false, 32);
        sm_config_set_in_shift(&cfg, true, false, 32);
        sm_config_set_clkdiv(&cfg, div);
        line_reset_gpio(clk, io);
        pio_gpio_init(pio, clk); pio_gpio_init(pio, io);
        gpio_pull_up(io);
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_clear_fifos(pio, sm);
        pio_sm_init(pio, sm, (uint)offset, &cfg);
        pio_sm_set_pindirs_with_mask(pio, sm, (1u << clk) | (1u << io),
                                     (1u << clk) | (1u << io));
        pio_sm_set_pins_with_mask(pio, sm, (1u << clk) | (1u << io),
                                  (1u << clk) | (1u << io));
        pio_sm_set_enabled(pio, sm, true);
        swd->opened = true; swd->manager = manager; swd->pio = pio;
        swd->pio_index = block; swd->sm = sm; swd->offset = (uint)offset;
        swd->swclk_gpio = clk; swd->swdio_gpio = io; swd->clock_hz = hz;
        return true;
    }
    return false;
}

void fun_pio_swd_close(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return;
    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_manager_release_program(swd->manager, swd->pio_index, PIO_SWD_PROGRAM_NAME);
    pio_manager_release_sm(swd->manager, swd->pio_index, swd->sm);
    gpio_set_function(swd->swclk_gpio, GPIO_FUNC_SIO);
    gpio_set_function(swd->swdio_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(swd->swclk_gpio, GPIO_IN);
    gpio_set_dir(swd->swdio_gpio, GPIO_IN);
    fun_pio_swd_init(swd);
}

bool fun_pio_swd_set_clock(fun_pio_swd_t *swd, uint32_t clock_hz)
{
    if (!swd || !swd->opened || clock_hz == 0u) return false;
    float div = (float)clock_get_hz(clk_sys) / ((float)clock_hz * 2.0f);
    if (div < 1.0f) div = 1.0f;
    pio_sm_set_clkdiv(swd->pio, swd->sm, div);
    swd->clock_hz = clock_hz;
    return true;
}

bool fun_pio_swd_read_dp_idcode(fun_pio_swd_t *swd, uint32_t *idcode,
                                uint8_t *ack)
{
    return fun_pio_swd_read_dp_register(swd, 0x00u, idcode, ack);
}

bool fun_pio_swd_read_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                  uint32_t *value, uint8_t *ack)
{
    if (!swd || !swd->opened || !value || (address & 3u)) return false;
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_sm_put_blocking(swd->pio, swd->sm, make_request(false, true, address));
    uint8_t response = (uint8_t)(pio_sm_get_blocking(swd->pio, swd->sm) >> 29);
    uint32_t raw_value = pio_sm_get_blocking(swd->pio, swd->sm);
    uint8_t received_parity = (uint8_t)(pio_sm_get_blocking(swd->pio, swd->sm) >> 31);
    if (ack) *ack = response;
    *value = raw_value;
    return response == 1u && received_parity == parity32(raw_value);
}
