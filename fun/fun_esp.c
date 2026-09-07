/*
 * @Author: 轩
 * @Date: 2026-09-07 12:43:35
 * @LastEditTime: 2026-09-07 13:53:59
 * @FilePath: \pico\fun\fun_esp.c
 */
#include "fun_esp.h"

#include <string.h>
#include "hardware/gpio.h"
#include "pico/stdlib.h"

static bool s_initialized = false;

bool fun_esp_init()
{
    if (s_initialized) {
        return true;
    }
    uint act_baud = uart_init(ESP_UART, ESP_BAUD);
    gpio_set_function(ESP_TX, GPIO_FUNC_UART);
    gpio_set_function(ESP_RX, GPIO_FUNC_UART);
    gpio_pull_up(ESP_RX);
    uart_set_format(ESP_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(ESP_UART, true);

    // 修复：必须使用 uart_is_readable 排空已有的残留垃圾数据
    while (uart_is_readable(ESP_UART)) {
        (void)uart_getc(ESP_UART);
    }

    s_initialized = (act_baud != 0);
    return s_initialized;
}

size_t esp_send(const uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0) return 0;
    uart_write_blocking(ESP_UART, data, len);
    return len; // 补齐返回值
}

size_t esp_send_string(const char *text)
{
    if (text == NULL) return 0;
    return esp_send((const uint8_t *)text, strlen(text));
}

bool esp_readable()
{
    return s_initialized && uart_is_readable(ESP_UART);
}

size_t esp_read(uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0) return 0;
    size_t count = 0;
    while (count < len && uart_is_readable(ESP_UART)) {
        data[count++] = (uint8_t)uart_getc(ESP_UART);
    }
    return count;
}