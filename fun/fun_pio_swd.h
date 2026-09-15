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
    bool write_opened;
    PIO write_pio;
    uint8_t write_pio_index;
    uint8_t write_sm;
    uint write_offset;
    uint8_t last_write_ack;
    uint8_t power_stage;
    bool mem_select_valid;
    bool mem_csw_valid;
    bool mem_tar_valid;
    uint32_t mem_csw;
    uint32_t mem_tar;
} fun_pio_swd_t;

void fun_pio_swd_init(fun_pio_swd_t *swd);
bool fun_pio_swd_open_auto(fun_pio_swd_t *swd, pio_manager_t *manager,
                           uint8_t swclk_gpio, uint8_t swdio_gpio,
                           uint32_t clock_hz);
bool fun_pio_swd_open_session(fun_pio_swd_t *swd, pio_manager_t *manager,
                              uint8_t swclk_gpio, uint8_t swdio_gpio,
                              uint32_t clock_hz, bool line_reset);
/* 不释放PIO资源，重新发送SWD切换序列并重启状态机。 */
bool fun_pio_swd_recover(fun_pio_swd_t *swd);
void fun_pio_swd_close(fun_pio_swd_t *swd);
/* 运行时调整 SWCLK 频率；返回 false 表示句柄无效或频率为 0。 */
bool fun_pio_swd_set_clock(fun_pio_swd_t *swd, uint32_t clock_hz);
uint8_t fun_pio_swd_transfer(fun_pio_swd_t *swd, uint8_t request,
                             uint32_t *data);
bool fun_pio_swd_read_dp_idcode(fun_pio_swd_t *swd, uint32_t *idcode,
                                uint8_t *ack);
/* 读取 DP 读寄存器（地址必须为 0/4/8/C）。 */
bool fun_pio_swd_read_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                  uint32_t *value, uint8_t *ack);
bool fun_pio_swd_read_register(fun_pio_swd_t *swd, bool ap, uint8_t address,
                               uint32_t *value, uint8_t *ack);
bool fun_pio_swd_write_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                   uint32_t value, uint8_t *ack);
bool fun_pio_swd_write_register(fun_pio_swd_t *swd, bool ap, uint8_t address,
                                uint32_t value, uint8_t *ack);
bool fun_pio_swd_power_up(fun_pio_swd_t *swd);
bool fun_pio_swd_read_memory32(fun_pio_swd_t *swd, uint32_t address,
                               uint32_t *value);
bool fun_pio_swd_write_memory32(fun_pio_swd_t *swd, uint32_t address,
                                uint32_t value);
bool fun_pio_swd_write_memory16(fun_pio_swd_t *swd, uint32_t address,
                                uint16_t value);
/* Write an arbitrary byte buffer through MEM-AP. The transfer uses 32-bit
 * address auto-increment and restarts TAR at every 1 KiB MEM-AP boundary. */
bool fun_pio_swd_write_memory_block32(fun_pio_swd_t *swd, uint32_t address,
                                      const uint8_t *data, uint32_t length);

#endif
