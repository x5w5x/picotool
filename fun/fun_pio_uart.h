#ifndef FUN_PIO_UART_H
#define FUN_PIO_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"
#include "pio_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 独立的 PIO UART TX 通道，当前固定实现 8-N-1。 */
typedef struct {
    bool opened;
    pio_manager_t *manager;
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
    uint8_t program_offset;
    uint8_t tx_gpio;
    uint32_t baudrate;
} fun_pio_uart_tx_t;

/* 独立的 PIO UART RX 通道，当前固定实现 8-N-1。 */
typedef struct {
    bool opened;
    pio_manager_t *manager;
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
    uint8_t program_offset;
    uint8_t rx_gpio;
    uint32_t baudrate;
} fun_pio_uart_rx_t;

bool fun_pio_uart_tx_open(fun_pio_uart_tx_t *tx,
                          pio_manager_t *manager,
                          uint8_t pio_index,
                          uint8_t tx_gpio,
                          uint32_t baudrate);
void fun_pio_uart_tx_close(fun_pio_uart_tx_t *tx);
bool fun_pio_uart_tx_is_writable(const fun_pio_uart_tx_t *tx);
bool fun_pio_uart_tx_putc(fun_pio_uart_tx_t *tx, uint8_t value);
size_t fun_pio_uart_tx_write(fun_pio_uart_tx_t *tx,
                             const uint8_t *data,
                             size_t length);

bool fun_pio_uart_rx_open(fun_pio_uart_rx_t *rx,
                          pio_manager_t *manager,
                          uint8_t pio_index,
                          uint8_t rx_gpio,
                          uint32_t baudrate);
void fun_pio_uart_rx_close(fun_pio_uart_rx_t *rx);
bool fun_pio_uart_rx_is_readable(const fun_pio_uart_rx_t *rx);
int fun_pio_uart_rx_getc(fun_pio_uart_rx_t *rx);
int fun_pio_uart_rx_getc_blocking(fun_pio_uart_rx_t *rx);
size_t fun_pio_uart_rx_read(fun_pio_uart_rx_t *rx,
                            uint8_t *data,
                            size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
