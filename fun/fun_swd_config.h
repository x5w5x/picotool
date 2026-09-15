#ifndef FUN_SWD_CONFIG_H
#define FUN_SWD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t swclk_gpio;
    uint8_t swdio_gpio;
    uint32_t clock_hz;
} fun_swd_config_t;

const fun_swd_config_t *fun_swd_config_get(void);
bool fun_swd_config_set_pins(uint8_t swclk_gpio, uint8_t swdio_gpio);
bool fun_swd_config_set_clock(uint32_t clock_hz);

#endif
