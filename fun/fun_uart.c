#include "fun_uart.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

static uart_inst_t *get_uart_instance(fun_uart_hw_instance_t instance)
{
    switch (instance) {
    case FUN_UART_HW_UART0:
        return uart0;
    case FUN_UART_HW_UART1:
        return uart1;
    default:
        return NULL;
    }
}

static uart_parity_t to_sdk_parity(fun_uart_parity_t parity)
{
    switch (parity) {
    case FUN_UART_PARITY_EVEN:
        return UART_PARITY_EVEN;
    case FUN_UART_PARITY_ODD:
        return UART_PARITY_ODD;
    default:
        return UART_PARITY_NONE;
    }
}

void fun_uart_config_init(fun_uart_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->hw_instance = FUN_UART_HW_UART1;
    config->tx_gpio = 20;
    config->rx_gpio = 21;
    config->baudrate = 115200;
    config->data_bits = 8;
    config->stop_bits = 1;
    config->parity = FUN_UART_PARITY_NONE;
}

bool fun_uart_is_pin_forbidden(uint8_t gpio)
{
    return gpio == 0 || gpio == 1;
}

fun_uart_hw_instance_t fun_uart_probe_hw_capability(uint8_t tx_gpio,
                                                    uint8_t rx_gpio)
{
    if (tx_gpio != FUN_UART_PIN_UNUSED && fun_uart_is_pin_forbidden(tx_gpio)) {
        return FUN_UART_HW_NONE;
    }
    if (rx_gpio != FUN_UART_PIN_UNUSED && fun_uart_is_pin_forbidden(rx_gpio)) {
        return FUN_UART_HW_NONE;
    }

    /* UART1 的完整通道：GP4/GP5 或 GP20/GP21。 */
    if ((tx_gpio == 4 || tx_gpio == 20) &&
        (rx_gpio == 5 || rx_gpio == 21)) {
        return FUN_UART_HW_UART1;
    }

    /* UART1 单向通道。 */
    if ((tx_gpio == 4 || tx_gpio == 20) &&
        rx_gpio == FUN_UART_PIN_UNUSED) {
        return FUN_UART_HW_UART1;
    }
    if (tx_gpio == FUN_UART_PIN_UNUSED &&
        (rx_gpio == 5 || rx_gpio == 21)) {
        return FUN_UART_HW_UART1;
    }

    /* 当前板级映射中，UART0 只开放 GP28 TX 单向输出。 */
    if (tx_gpio == 28 && rx_gpio == FUN_UART_PIN_UNUSED) {
        return FUN_UART_HW_UART0;
    }

    return FUN_UART_HW_NONE;
}

bool fun_uart_open(fun_uart_t *uart,
                   const fun_uart_config_t *config)
{
    if (uart == NULL || config == NULL) {
        return false;
    }

    if (uart->opened) {
        fun_uart_close(uart);
    }

    if (config->baudrate == 0 ||
        config->data_bits < 5 || config->data_bits > 8 ||
        config->stop_bits < 1 || config->stop_bits > 2) {
        return false;
    }

    fun_uart_hw_instance_t detected =
        fun_uart_probe_hw_capability(config->tx_gpio, config->rx_gpio);
    if (detected == FUN_UART_HW_NONE) {
        return false;
    }

    uart_inst_t *uart_hw = get_uart_instance(detected);
    if (uart_hw == NULL) {
        return false;
    }

    memset(uart, 0, sizeof(*uart));
    uart->config = *config;
    uart->config.hw_instance = detected;
    uart->uart_hw = uart_hw;

    uart_init(uart_hw, config->baudrate);
    if (config->tx_gpio != FUN_UART_PIN_UNUSED) {
        gpio_set_function(config->tx_gpio, GPIO_FUNC_UART);
    }
    if (config->rx_gpio != FUN_UART_PIN_UNUSED) {
        gpio_set_function(config->rx_gpio, GPIO_FUNC_UART);
        gpio_pull_up(config->rx_gpio);
    }

    uart_set_hw_flow(uart_hw, false, false);
    uart_set_format(uart_hw,
                    config->data_bits,
                    config->stop_bits,
                    to_sdk_parity(config->parity));
    uart_set_fifo_enabled(uart_hw, true);

    while (uart_is_readable(uart_hw)) {
        (void)uart_getc(uart_hw);
    }

    uart->opened = true;
    return true;
}

void fun_uart_close(fun_uart_t *uart)
{
    if (uart == NULL || !uart->opened) {
        return;
    }

    if (uart->uart_hw != NULL) {
        uart_deinit(uart->uart_hw);
        if (uart->config.tx_gpio != FUN_UART_PIN_UNUSED) {
            gpio_set_function(uart->config.tx_gpio, GPIO_FUNC_NULL);
        }
        if (uart->config.rx_gpio != FUN_UART_PIN_UNUSED) {
            gpio_set_function(uart->config.rx_gpio, GPIO_FUNC_NULL);
        }
    }

    uart->opened = false;
    uart->uart_hw = NULL;
}

bool fun_uart_set_baudrate(fun_uart_t *uart, uint32_t baudrate)
{
    if (uart == NULL || !uart->opened || uart->uart_hw == NULL ||
        baudrate == 0) {
        return false;
    }

    uart_set_baudrate(uart->uart_hw, baudrate);
    uart->config.baudrate = baudrate;
    return true;
}

bool fun_uart_is_writable(const fun_uart_t *uart)
{
    return uart != NULL && uart->opened && uart->uart_hw != NULL &&
           uart_is_writable(uart->uart_hw);
}

bool fun_uart_is_readable(const fun_uart_t *uart)
{
    return uart != NULL && uart->opened && uart->uart_hw != NULL &&
           uart_is_readable(uart->uart_hw);
}

bool fun_uart_putc(fun_uart_t *uart, uint8_t value)
{
    if (uart == NULL || !uart->opened || uart->uart_hw == NULL) {
        return false;
    }

    while (!uart_is_writable(uart->uart_hw)) {
        tight_loop_contents();
    }
    uart_putc_raw(uart->uart_hw, value);
    uart->tx_count++;
    return true;
}

int fun_uart_getc(fun_uart_t *uart)
{
    if (!fun_uart_is_readable(uart)) {
        return -1;
    }

    uart->rx_count++;
    return uart_getc(uart->uart_hw);
}

size_t fun_uart_write(fun_uart_t *uart,
                      const uint8_t *data,
                      size_t length)
{
    if (uart == NULL || data == NULL || !uart->opened) {
        return 0;
    }

    size_t written = 0;
    while (written < length && fun_uart_putc(uart, data[written])) {
        ++written;
    }
    return written;
}

size_t fun_uart_write_nonblocking(fun_uart_t *uart,
                                  const uint8_t *data,
                                  size_t length)
{
    if (uart == NULL ||
        data == NULL ||
        length == 0 ||
        !uart->opened ||
        uart->uart_hw == NULL) {
        return 0;
    }

    size_t written = 0;
    while (written < length && uart_is_writable(uart->uart_hw)) {
        uart_putc_raw(uart->uart_hw, data[written]);
        uart->tx_count++;
        written++;
    }

    return written;
}

size_t fun_uart_read(fun_uart_t *uart,
                     uint8_t *buffer,
                     size_t capacity)
{
    if (uart == NULL || buffer == NULL || capacity == 0 ||
        !uart->opened) {
        return 0;
    }

    size_t count = 0;
    while (count < capacity && fun_uart_is_readable(uart)) {
        int value = fun_uart_getc(uart);
        if (value < 0) {
            break;
        }
        buffer[count++] = (uint8_t)value;
    }
    return count;
}

size_t fun_uart_usb_read(uint8_t *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0) {
        return 0;
    }

    size_t count = 0;
    while (count < capacity) {
        int value = getchar_timeout_us(0);
        if (value == PICO_ERROR_TIMEOUT) {
            break;
        }
        buffer[count++] = (uint8_t)value;
    }
    return count;
}

size_t fun_uart_usb_write(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        putchar_raw(data[i]);
    }
    stdio_flush();
    return length;
}

bool fun_uart_is_open(const fun_uart_t *uart)
{
    return uart != NULL && uart->opened;
}

uint32_t fun_uart_get_tx_count(const fun_uart_t *uart)
{
    return uart != NULL ? uart->tx_count : 0;
}

uint32_t fun_uart_get_rx_count(const fun_uart_t *uart)
{
    return uart != NULL ? uart->rx_count : 0;
}

void fun_uart_clear_count(fun_uart_t *uart)
{
    if (uart != NULL) {
        uart->tx_count = 0;
        uart->rx_count = 0;
    }
}
