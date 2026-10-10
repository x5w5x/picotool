/*
 * @Author: 轩
 * @Date: 2026-09-07 12:43:35
 * @LastEditTime: 2026-10-05 15:46:22
 * @FilePath: \pico\fun\fun_esp.c
 */
#include "fun_esp.h"
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"

static bool s_initialized = false;

// 1. 键盘专用 FIFO 队列 (中断安全)
#define KB_LINE_MAX     48
#define KB_QUEUE_SIZE   16
static char s_kb_queue[KB_QUEUE_SIZE][KB_LINE_MAX];
static volatile uint8_t s_kb_q_head = 0;
static volatile uint8_t s_kb_q_tail = 0;
volatile bool g_kb_has_frame = false;

// 2. 普通串口透传环形缓冲区 (保证现有 app_esp 正常读取)
#define ESP_RX_RING_SIZE 1024
static uint8_t s_esp_rx_ring[ESP_RX_RING_SIZE];
static volatile uint16_t s_esp_rx_head = 0;
static volatile uint16_t s_esp_rx_tail = 0;

// 中断行缓冲
static char s_irq_line[KB_LINE_MAX];
static size_t s_irq_idx = 0;

// ⚡ UART0 硬件中断服务函数 (执行耗时 < 1 微秒，零卡顿)
static void on_esp_uart_rx_irq(void)
{
    /* 下载协议包含任意二进制字节，必须逐字节保留原始串口流。 */
    while (uart_is_readable(ESP_UART)) {
        uint8_t c = (uint8_t)uart_getc(ESP_UART);
        uint16_t next = (uint16_t)((s_esp_rx_head + 1u) % ESP_RX_RING_SIZE);
        if (next != s_esp_rx_tail) {
            s_esp_rx_ring[s_esp_rx_head] = c;
            s_esp_rx_head = next;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            s_irq_line[s_irq_idx] = '\0';
            if (strncmp(s_irq_line, "#KB:", 4) == 0) {
                uint8_t next_head = (uint8_t)((s_kb_q_head + 1u) % KB_QUEUE_SIZE);
                if (next_head != s_kb_q_tail) {
                    strncpy(s_kb_queue[s_kb_q_head], s_irq_line, KB_LINE_MAX - 1u);
                    s_kb_queue[s_kb_q_head][KB_LINE_MAX - 1u] = '\0';
                    s_kb_q_head = next_head;
                    g_kb_has_frame = true;
                }
            }
            s_irq_idx = 0;
        } else if (s_irq_idx < KB_LINE_MAX - 1u) {
            s_irq_line[s_irq_idx++] = (char)c;
        }
    }
}
bool fun_esp_init(void)
{
    if (s_initialized) return true;

    uint act_baud = uart_init(ESP_UART, ESP_BAUD);
    gpio_set_function(ESP_TX, GPIO_FUNC_UART);
    gpio_set_function(ESP_RX, GPIO_FUNC_UART);
    gpio_pull_up(ESP_RX);

    uart_set_format(ESP_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(ESP_UART, true);

    while (uart_is_readable(ESP_UART)) {
        (void)uart_getc(ESP_UART);
    }

    // ⚡ 激活 UART0 接收硬件中断
    irq_set_exclusive_handler(UART0_IRQ, on_esp_uart_rx_irq);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(ESP_UART, true, false);

    s_initialized = (act_baud != 0);
    return s_initialized;
}

size_t esp_send(const uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0) return 0;
    uart_write_blocking(ESP_UART, data, len);
    return len;
}

size_t esp_send_string(const char *text)
{
    if (text == NULL) return 0;
    return esp_send((const uint8_t *)text, strlen(text));
}

bool esp_readable(void)
{
    return s_initialized && (s_esp_rx_head != s_esp_rx_tail);
}

size_t esp_read(uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0) return 0;
    size_t count = 0;
    while (count < len && s_esp_rx_head != s_esp_rx_tail) {
        data[count++] = s_esp_rx_ring[s_esp_rx_tail];
        s_esp_rx_tail = (uint16_t)((s_esp_rx_tail + 1) % ESP_RX_RING_SIZE);
    }
    return count;
}

// 弹出提取完整一帧键盘命令（纯内存操作，耗时 < 1 微秒）
bool esp_pop_keyboard_line(char *out, size_t max_len)
{
    if (s_kb_q_head == s_kb_q_tail || out == NULL || max_len == 0) {
        g_kb_has_frame = false;
        return false;
    }

    strncpy(out, s_kb_queue[s_kb_q_tail], max_len - 1);
    out[max_len - 1] = '\0';
    s_kb_q_tail = (uint8_t)((s_kb_q_tail + 1) % KB_QUEUE_SIZE);

    g_kb_has_frame = (s_kb_q_head != s_kb_q_tail);
    return true;
}