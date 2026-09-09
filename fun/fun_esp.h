/*
 * @Author: 轩
 * @Date: 2026-09-07 12:43:27
 * @LastEditTime: 2026-09-07 13:08:09
 * @FilePath: \pico\fun\fun_esp.h
 */
#ifndef FUN_ESP
#define FUN_ESP
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/uart.h"
#ifdef __cplusplus
extern "C" {
#endif

#define ESP_TX 0
#define ESP_RX 1
#define ESP_UART uart0
#define ESP_BAUD 921600
bool fun_esp_init();
bool esp_readable();
size_t esp_read(uint8_t *data,size_t len);
size_t esp_send_string(const char *text);
#ifdef __cplusplus
}
#endif
#endif // !FUN_ESP
