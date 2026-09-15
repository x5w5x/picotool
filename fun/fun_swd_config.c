#include "fun_swd_config.h"
#include "pin_manager.h"

static fun_swd_config_t s_config = {
    .swclk_gpio = 2u,
    .swdio_gpio = 3u,
    .clock_hz = 10000000u
};

const fun_swd_config_t *fun_swd_config_get(void) { return &s_config; }

bool fun_swd_config_set_pins(uint8_t swclk_gpio, uint8_t swdio_gpio)
{
    if (swclk_gpio == swdio_gpio ||
        !pin_manager_supports(swclk_gpio, PIN_FUNC_PIO) ||
        !pin_manager_supports(swdio_gpio, PIN_FUNC_PIO)) return false;
    s_config.swclk_gpio = swclk_gpio;
    s_config.swdio_gpio = swdio_gpio;
    return true;
}

bool fun_swd_config_set_clock(uint32_t clock_hz)
{
    if (clock_hz == 0u || clock_hz > 10000000u) return false;
    s_config.clock_hz = clock_hz;
    return true;
}
