#ifndef FUN_PIO_SWD_H
#define FUN_PIO_SWD_H

#include <stdbool.h>
#include <stdint.h>
#include "pio_manager.h"

typedef struct {
    bool opened;
    pio_manager_t *manager;
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
    uint offset;
    uint8_t swclk_gpio;
    uint8_t swdio_gpio;
    uint32_t clock_hz;
} fun_pio_swd_t;

void fun_pio_swd_init(fun_pio_swd_t *swd);
bool fun_pio_swd_open_auto(fun_pio_swd_t *swd, pio_manager_t *manager,
                           uint8_t swclk_gpio, uint8_t swdio_gpio,
                           uint32_t clock_hz);
void fun_pio_swd_close(fun_pio_swd_t *swd);
/* 运行时调整 SWCLK 频率；返回 false 表示句柄无效或频率为 0。 */
bool fun_pio_swd_set_clock(fun_pio_swd_t *swd, uint32_t clock_hz);
bool fun_pio_swd_read_dp_idcode(fun_pio_swd_t *swd, uint32_t *idcode,
                                uint8_t *ack);
/* 读取 DP 读寄存器（地址必须为 0/4/8/C）。 */
bool fun_pio_swd_read_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                  uint32_t *value, uint8_t *ack);

#endif
