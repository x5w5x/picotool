#ifndef FUN_UART_H
#define FUN_UART_H

/*
 * UART 功能层。
 * USB CDC 由系统启动流程统一开启，不在本模块中 open/close。
 * 本模块当前负责动态选择硬件 UART0/UART1；PIO UART 后续接入。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FUN_UART_PIN_UNUSED 0xFFu

typedef enum {
    FUN_UART_HW_NONE = 0,
    FUN_UART_HW_UART0,
    FUN_UART_HW_UART1
} fun_uart_hw_instance_t;

typedef enum {
    FUN_UART_PARITY_NONE = 0,
    FUN_UART_PARITY_EVEN,
    FUN_UART_PARITY_ODD
} fun_uart_parity_t;

typedef struct {
    fun_uart_hw_instance_t hw_instance;
    uint8_t tx_gpio;
    uint8_t rx_gpio;
    uint32_t baudrate;
    uint8_t data_bits;
    uint8_t stop_bits;
    fun_uart_parity_t parity;
} fun_uart_config_t;

typedef struct {
    bool opened;
    fun_uart_config_t config;
    uart_inst_t *uart_hw;
    uint32_t tx_count;
    uint32_t rx_count;
} fun_uart_t;

void fun_uart_config_init(fun_uart_config_t *config);
bool fun_uart_is_pin_forbidden(uint8_t gpio);
fun_uart_hw_instance_t fun_uart_probe_hw_capability(uint8_t tx_gpio,
                                                    uint8_t rx_gpio);

bool fun_uart_open(fun_uart_t *uart,
                   const fun_uart_config_t *config);
void fun_uart_close(fun_uart_t *uart);
bool fun_uart_set_baudrate(fun_uart_t *uart, uint32_t baudrate);

bool fun_uart_is_writable(const fun_uart_t *uart);
bool fun_uart_is_readable(const fun_uart_t *uart);
size_t fun_uart_write(fun_uart_t *uart,
                      const uint8_t *data,
                      size_t length);
/*
 * 非阻塞发送：只发送当前 UART FIFO 能接收的数据，FIFO 满时立即返回。
 * 调用方必须保存返回值之后尚未发送的字节。
 */
size_t fun_uart_write_nonblocking(fun_uart_t *uart,
                                  const uint8_t *data,
                                  size_t length);
size_t fun_uart_read(fun_uart_t *uart,
                     uint8_t *buffer,
                     size_t capacity);
bool fun_uart_putc(fun_uart_t *uart, uint8_t value);
int fun_uart_getc(fun_uart_t *uart);

/* USB CDC 已由 stdio_init_all() 和 CMake 开启，不需要 open/close。 */
size_t fun_uart_usb_read(uint8_t *buffer, size_t capacity);
size_t fun_uart_usb_write(const uint8_t *data, size_t length);

bool fun_uart_is_open(const fun_uart_t *uart);
uint32_t fun_uart_get_tx_count(const fun_uart_t *uart);
uint32_t fun_uart_get_rx_count(const fun_uart_t *uart);
void fun_uart_clear_count(fun_uart_t *uart);

#ifdef __cplusplus
}
#endif

#endif
