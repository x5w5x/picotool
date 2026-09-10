#include "fun_pio_uart.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "uart.pio.h"

#define FUN_PIO_UART_CYCLES_PER_BIT 8u
#define FUN_PIO_UART_TX_PROGRAM_NAME "uart_tx"
#define FUN_PIO_UART_RX_PROGRAM_NAME "uart_rx"

bool fun_pio_uart_tx_open(fun_pio_uart_tx_t *tx,
                          pio_manager_t *manager,
                          uint8_t pio_index,
                          uint8_t tx_gpio,
                          uint32_t baudrate)
{
    if (tx == NULL || manager == NULL ||
        pio_index >= PIO_COUNT || tx_gpio >= NUM_BANK0_GPIOS ||
        tx_gpio == 0 || tx_gpio == 1 || baudrate == 0) {
        return false;
    }

    if (tx->opened) {
        fun_pio_uart_tx_close(tx);
    }

    float clkdiv = (float)clock_get_hz(clk_sys) /
                   ((float)baudrate * FUN_PIO_UART_CYCLES_PER_BIT);
    if (clkdiv < 1.0f || clkdiv > 65536.0f) {
        return false;
    }

    int sm_id = pio_manager_claim_sm_from(manager, pio_index,
                                           "pio_uart_tx");
    if (sm_id < 0) {
        return false;
    }

    uint8_t sm = PIO_MANAGER_ID_TO_SM(sm_id);
    int offset = pio_manager_load_program(manager, pio_index,
                                          FUN_PIO_UART_TX_PROGRAM_NAME,
                                          &uart_tx_program);
    if (offset < 0) {
        pio_manager_release_sm(manager, pio_index, sm);
        return false;
    }

    PIO pio = pio_manager_get_pio(manager, pio_index);
    pio_sm_config config =
        uart_tx_program_get_default_config((uint)offset);
    sm_config_set_out_pins(&config, tx_gpio, 1);
    sm_config_set_sideset_pins(&config, tx_gpio);
    sm_config_set_out_shift(&config, true, false, 32);
    sm_config_set_clkdiv(&config, clkdiv);

    pio_gpio_init(pio, tx_gpio);
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_init(pio, sm, (uint)offset, &config);

    /* UART 空闲电平为高，启用状态机前先设置电平和输出方向。 */
    pio_sm_set_pins_with_mask(pio, sm, 1u << tx_gpio, 1u << tx_gpio);
    pio_sm_set_pindirs_with_mask(pio, sm, 1u << tx_gpio, 1u << tx_gpio);
    pio_sm_set_enabled(pio, sm, true);

    memset(tx, 0, sizeof(*tx));
    tx->opened = true;
    tx->manager = manager;
    tx->pio = pio;
    tx->pio_index = pio_index;
    tx->sm = sm;
    tx->program_offset = (uint8_t)offset;
    tx->tx_gpio = tx_gpio;
    tx->baudrate = baudrate;
    return true;
}

void fun_pio_uart_tx_close(fun_pio_uart_tx_t *tx)
{
    if (tx == NULL || !tx->opened || tx->manager == NULL || tx->pio == NULL) {
        return;
    }

    pio_sm_set_enabled(tx->pio, tx->sm, false);
    pio_sm_clear_fifos(tx->pio, tx->sm);
    pio_sm_set_consecutive_pindirs(tx->pio, tx->sm, tx->tx_gpio, 1, false);
    gpio_set_function(tx->tx_gpio, GPIO_FUNC_NULL);

    pio_manager_release_sm(tx->manager, tx->pio_index, tx->sm);
    pio_manager_release_program(tx->manager, tx->pio_index,
                                FUN_PIO_UART_TX_PROGRAM_NAME);
    memset(tx, 0, sizeof(*tx));
}

bool fun_pio_uart_tx_is_writable(const fun_pio_uart_tx_t *tx)
{
    return tx != NULL && tx->opened && tx->pio != NULL &&
           !pio_sm_is_tx_fifo_full(tx->pio, tx->sm);
}

bool fun_pio_uart_tx_putc(fun_pio_uart_tx_t *tx, uint8_t value)
{
    if (tx == NULL || !tx->opened || tx->pio == NULL) {
        return false;
    }

    pio_sm_put_blocking(tx->pio, tx->sm, value);
    return true;
}

size_t fun_pio_uart_tx_write(fun_pio_uart_tx_t *tx,
                             const uint8_t *data,
                             size_t length)
{
    if (tx == NULL || data == NULL || !tx->opened) {
        return 0;
    }

    size_t written = 0;
    while (written < length && fun_pio_uart_tx_putc(tx, data[written])) {
        written++;
    }
    return written;
}

bool fun_pio_uart_rx_open(fun_pio_uart_rx_t *rx,
                          pio_manager_t *manager,
                          uint8_t pio_index,
                          uint8_t rx_gpio,
                          uint32_t baudrate)
{
    if (rx == NULL || manager == NULL ||
        pio_index >= PIO_COUNT || rx_gpio >= NUM_BANK0_GPIOS ||
        rx_gpio == 0 || rx_gpio == 1 || baudrate == 0) {
        return false;
    }

    if (rx->opened) {
        fun_pio_uart_rx_close(rx);
    }

    float clkdiv = (float)clock_get_hz(clk_sys) /
                   ((float)baudrate * FUN_PIO_UART_CYCLES_PER_BIT);
    if (clkdiv < 1.0f || clkdiv > 65536.0f) {
        return false;
    }

    int sm_id = pio_manager_claim_sm_from(manager, pio_index,
                                           "pio_uart_rx");
    if (sm_id < 0) {
        return false;
    }

    uint8_t sm = PIO_MANAGER_ID_TO_SM(sm_id);
    int offset = pio_manager_load_program(manager, pio_index,
                                          FUN_PIO_UART_RX_PROGRAM_NAME,
                                          &uart_rx_program);
    if (offset < 0) {
        pio_manager_release_sm(manager, pio_index, sm);
        return false;
    }

    PIO pio = pio_manager_get_pio(manager, pio_index);
    pio_sm_config config =
        uart_rx_program_get_default_config((uint)offset);
    sm_config_set_in_pins(&config, rx_gpio);
    sm_config_set_in_shift(&config, true, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&config, clkdiv);

    pio_gpio_init(pio, rx_gpio);
    gpio_pull_up(rx_gpio);
    pio_sm_set_consecutive_pindirs(pio, sm, rx_gpio, 1, false);
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_init(pio, sm, (uint)offset, &config);
    pio_sm_set_enabled(pio, sm, true);

    memset(rx, 0, sizeof(*rx));
    rx->opened = true;
    rx->manager = manager;
    rx->pio = pio;
    rx->pio_index = pio_index;
    rx->sm = sm;
    rx->program_offset = (uint8_t)offset;
    rx->rx_gpio = rx_gpio;
    rx->baudrate = baudrate;
    return true;
}

void fun_pio_uart_rx_close(fun_pio_uart_rx_t *rx)
{
    if (rx == NULL || !rx->opened || rx->manager == NULL || rx->pio == NULL) {
        return;
    }

    pio_sm_set_enabled(rx->pio, rx->sm, false);
    pio_sm_clear_fifos(rx->pio, rx->sm);
    gpio_disable_pulls(rx->rx_gpio);
    gpio_set_function(rx->rx_gpio, GPIO_FUNC_NULL);

    pio_manager_release_sm(rx->manager, rx->pio_index, rx->sm);
    pio_manager_release_program(rx->manager, rx->pio_index,
                                FUN_PIO_UART_RX_PROGRAM_NAME);
    memset(rx, 0, sizeof(*rx));
}

bool fun_pio_uart_rx_is_readable(const fun_pio_uart_rx_t *rx)
{
    return rx != NULL && rx->opened && rx->pio != NULL &&
           !pio_sm_is_rx_fifo_empty(rx->pio, rx->sm);
}

int fun_pio_uart_rx_getc(fun_pio_uart_rx_t *rx)
{
    if (!fun_pio_uart_rx_is_readable(rx)) {
        return -1;
    }

    return (int)(pio_sm_get(rx->pio, rx->sm) >> 24);
}

int fun_pio_uart_rx_getc_blocking(fun_pio_uart_rx_t *rx)
{
    if (rx == NULL || !rx->opened || rx->pio == NULL) {
        return -1;
    }

    return (int)(pio_sm_get_blocking(rx->pio, rx->sm) >> 24);
}

size_t fun_pio_uart_rx_read(fun_pio_uart_rx_t *rx,
                            uint8_t *data,
                            size_t capacity)
{
    if (rx == NULL || data == NULL || capacity == 0 || !rx->opened) {
        return 0;
    }

    size_t count = 0;
    while (count < capacity && fun_pio_uart_rx_is_readable(rx)) {
        data[count++] = (uint8_t)fun_pio_uart_rx_getc(rx);
    }
    return count;
}
