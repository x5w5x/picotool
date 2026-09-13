#ifndef FUN_UART_PROFILE_H
#define FUN_UART_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUN_UART_PROFILE_MAGIC   0x55505246u /* "UPRF" */
#define FUN_UART_PROFILE_VERSION 1u

/*
 * 与UI控件和UART硬件对象解耦的稳定配置格式。
 * 后续可将整个结构直接存入W25Q64，再通过CRC判断数据是否有效。
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t baudrate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t tx_gpio;
    uint8_t rx_gpio;
    uint8_t reserved[3];
    uint32_t crc32;
} fun_uart_profile_t;

void fun_uart_profile_set_defaults(fun_uart_profile_t *profile);
void fun_uart_profile_finalize(fun_uart_profile_t *profile);
bool fun_uart_profile_is_valid(const fun_uart_profile_t *profile);

#ifdef __cplusplus
}
#endif

#endif /* FUN_UART_PROFILE_H */
