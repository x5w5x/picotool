/*
 * @Author: 轩
 * @Date: 2026-09-10 19:40:15
 * @LastEditTime: 2026-09-12 15:16:54
 * @FilePath: \pico\fun\fun_pio_uart.h
 */
#ifndef FUN_PIO_UART_H
#define FUN_PIO_UART_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "hardware/pio.h"
#include "pio_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool opened;

    pio_manager_t *manager;

    PIO pio;
    uint8_t pio_index;
    uint8_t sm;

    uint offset;

    uint8_t tx_gpio;
    uint32_t baudrate;
} fun_pio_uart_tx_t;
typedef struct
{
    bool opened;
    pio_manager_t *manager;
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
    uint offset;
    uint8_t rx_gpio;
    uint32_t baudrate;
} fun_pio_uart_rx_t;

typedef struct {
    bool opened;
    fun_pio_uart_tx_t tx;
    fun_pio_uart_rx_t rx;
}fun_pio_uart_t;
void fun_pio_uart_tx_init(fun_pio_uart_tx_t *tx);
bool fun_pio_uart_tx_open(fun_pio_uart_tx_t *tx,pio_manager_t *manager,uint8_t pio_index, uint8_t tx_gpio,uint32_t baudrate);
bool fun_pio_uart_tx_putc(fun_pio_uart_tx_t *tx,uint8_t value);
size_t fun_pio_uart_tx_write(fun_pio_uart_tx_t *tx,const uint8_t *data,size_t len);
bool fun_pio_uart_tx_is_writable(const fun_pio_uart_tx_t *tx);
bool fun_pio_uart_tx_try_putc(fun_pio_uart_tx_t *tx,uint8_t value);
bool fun_pio_uart_tx_close(fun_pio_uart_tx_t *tx);
void fun_pio_uart_rx_init(fun_pio_uart_rx_t *rx);
bool fun_pio_uart_rx_open(fun_pio_uart_rx_t *rx,pio_manager_t *manager,uint8_t pio_index,uint8_t rx_gpio,uint32_t baudrate);
bool fun_pio_uart_rx_is_readable(const fun_pio_uart_rx_t *rx);
bool fun_pio_uart_rx_try_getc(fun_pio_uart_rx_t *rx,uint8_t *value);
size_t fun_pio_uart_rx_read(fun_pio_uart_rx_t *rx,uint8_t *data,size_t len);
bool fun_pio_uart_rx_close(fun_pio_uart_rx_t *rx);
void fun_pio_uart_init(fun_pio_uart_t *uart);
bool fun_pio_uart_open(fun_pio_uart_t *uart,pio_manager_t *manager,uint8_t pio_index,uint8_t tx_gpio,uint8_t rx_gpio,uint32_t baudrate);
bool fun_pio_uart_close(fun_pio_uart_t *uart);
bool fun_pio_uart_putc(fun_pio_uart_t *uart,uint8_t value);
bool fun_pio_uart_try_putc(fun_pio_uart_t *uart,uint8_t value);
size_t fun_pio_uart_write(fun_pio_uart_t *uart,const uint8_t *data,size_t len);
size_t fun_pio_uart_write_nonblocking(fun_pio_uart_t *uart,const uint8_t *data,size_t len);
bool fun_pio_uart_is_readable(const fun_pio_uart_t *uart);
bool fun_pio_uart_try_getc(fun_pio_uart_t *uart,uint8_t *value);
size_t fun_pio_uart_read(fun_pio_uart_t *uart,uint8_t *data,size_t len);
bool fun_pio_uart_is_open(const fun_pio_uart_t *uart);
bool fun_pio_uart_is_writable(const fun_pio_uart_t *uart);
bool fun_pio_uart_can_open_on(const pio_manager_t *manager,uint8_t pio_index);
int fun_pio_uart_select_pio(const pio_manager_t *manager);
bool fun_pio_uart_open_auto(fun_pio_uart_t *uart,pio_manager_t *manager,uint8_t tx_gpio,uint8_t rx_gpio,uint32_t baudrate,uint8_t *selected_pio);

#ifdef __cplusplus
}
#endif

#endif