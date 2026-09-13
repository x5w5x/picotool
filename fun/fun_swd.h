#ifndef FUN_SWD_H
#define FUN_SWD_H
#include <stdbool.h>
#include <stdint.h>
#define FUN_SWD_SWCLK_PIN 2u
#define FUN_SWD_SWDIO_PIN 3u
void fun_swd_set_delay(uint8_t delay);
bool fun_swd_init(void);
void fun_swd_line_reset(void);
uint8_t fun_swd_transfer(uint8_t request, uint32_t *data, bool read);
uint8_t fun_swd_last_write_ack(void);
uint8_t fun_swd_last_read_ack(void);
void fun_swd_write(uint32_t address, uint32_t value);
void fun_swd_write_half(uint32_t address, uint16_t value);
uint32_t fun_swd_read(uint32_t address);
#endif
