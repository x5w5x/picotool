#include "app_uart.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_scrollview.h"
#include "ui_tabview.h"
#include "ui_terminal.h"
#include "ui_btn.h"
#include "ui_input.h"
#include "ui_keyboard.h"
#include "ui_dropdown.h"
#include "ui_switch.h"
#include "ui_toast.h"
#include "ui_pin_config.h"
#include "lcd.h"

#include "pico/stdlib.h"
#include <string.h>
#include "fun_uart.h"
#include "fun_uart_profile.h"
#include "fun_pio_uart.h"
#include "pin_manager.h"
#include "pio/pio_manager.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/structs/dma.h"
#include <stdio.h>

static const char *s_uart_tabs[] = {
    "串口状态",
    "串口配置",
    "终端"};
#define TAB_COUNT (sizeof(s_uart_tabs) / sizeof(s_uart_tabs[0]))

#define STATUS_CONTENT_H   520
#define SETTINGS_CONTENT_H 360   // 内容总高精简至 360px，滚动体验更平滑
#define UART_DMA_RING_SIZE 2048u
#define UART_DMA_RING_BITS 11u
#define UART_TX_RING_SIZE  1024u
#define UART_TX_RING_MASK  (UART_TX_RING_SIZE - 1u)
#define UART_TERM_QUEUE_SIZE 16
#define UART_TERM_LINE_SIZE  64

/* Tab 2: 终端布局 */
#define UART_TERM_X       120
#define UART_TERM_Y       38
#define UART_TERM_W       350
#define UART_TERM_H       226

#define UART_INPUT_X      120
#define UART_INPUT_Y      272
#define UART_INPUT_W      226
#define UART_INPUT_H      36

#define UART_CLEAR_X      352
#define UART_CLEAR_Y      272
#define UART_CLEAR_W      56
#define UART_CLEAR_H      36

#define UART_SEND_X       414
#define UART_SEND_Y       272
#define UART_SEND_W       56
#define UART_SEND_H       36

/* ⚡ Tab 1: 串口配置网格化布局 (彻底杜绝文字与控件重叠) */
#define UART_CFG_LABEL_X  14    // 标签 X 起点 (14 ~ 78px)
#define UART_CFG_CTRL_X   95    // 控件 X 起点 (留足 17px 呼吸间距)
#define UART_CFG_CTRL_W   230   // 控件宽 (95 + 230 = 325px，右侧预留 25px 滚动条空隙)
#define UART_CFG_CTRL_H   32    // 控件标准高度

#define UART_BAUD_Y       16
#define UART_DATA_Y       68
#define UART_PARITY_Y     120
#define UART_STOP_Y       172
#define UART_TX_PIN_Y     224

#define UART_OPTION_X     95
#define UART_OPTION_W     68
#define UART_OPTION_H     32
#define UART_OPTION_GAP   6

typedef enum {
    UART_BACKEND_HARDWARE = 0,
    UART_BACKEND_PIO
}uart_backend_t;

typedef struct
{
    char text[UART_TERM_LINE_SIZE];
    uint16_t color;
} uart_term_message_t;

static uint8_t s_uart_dma_ring[UART_DMA_RING_SIZE]
    __attribute__((aligned(UART_DMA_RING_SIZE)));

static volatile uint16_t s_uart_dma_tail = 0;
static int s_uart_dma_channel = -1;

static uint8_t s_uart_tx_ring[UART_TX_RING_SIZE];
static uint16_t s_uart_tx_head = 0;
static uint16_t s_uart_tx_tail = 0;
static uart_term_message_t s_uart_term_queue[UART_TERM_QUEUE_SIZE];
static volatile uint8_t s_uart_term_head = 0;
static volatile uint8_t s_uart_term_tail = 0;

static volatile bool s_uart_session_active = false;

typedef enum
{
    UART_WORK_IDLE = 0,
    UART_WORK_BRIDGE
} uart_mode_t;

static const char* s_baud_options[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"
};
static const uint32_t s_baud_values[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};

static int8_t uart_find_baud_index(uint32_t baudrate)
{
    for (size_t i = 0; i < sizeof(s_baud_values) / sizeof(s_baud_values[0]); ++i) {
        if (s_baud_values[i] == baudrate) return (int8_t)i;
    }
    return 4;
}

typedef struct
{
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab_idx;

    ui_scrollview_t status_scroll;
    ui_scrollview_t setting_scroll;
    ui_terminal_t terminal;

    ui_btn_t btn_clear;
    ui_input_t input_cmd;
    ui_keyboard_t keyboard;
    ui_btn_t btn_send;

    ui_dropdown_t baud;
    ui_btn_t data_bit7;
    ui_btn_t data_bit8;
    ui_btn_t parity_none;
    ui_btn_t parity_even;
    ui_btn_t parity_odd;
    ui_btn_t stop_bit1;
    ui_btn_t stop_bit2;
    ui_pin_config_t tx_pin_config;
    ui_pin_config_t rx_pin_config;
    ui_btn_t apply_config;
    ui_toast_t toast;

    fun_uart_t uart;
    fun_uart_config_t uart_config;
    fun_pio_uart_t pio_uart;
    pin_manager_t *pin_manager;
    uart_backend_t backend;
    uart_backend_t active_backend;
    uint8_t selected_pio;
    volatile uart_mode_t uart_mode;
    volatile bool io_enable;
    volatile bool core1_quiescent;

    volatile uint32_t usb_to_uart_count;
    volatile uint32_t uart_to_usb_count;
    uint32_t last_usb_to_uart_count;
    uint32_t last_uart_to_usb_count;

    char last_message[UART_TERM_LINE_SIZE];
    uint16_t last_message_color;

} uart_data_t;

static app_t s_uart;
/* 当前先作为RAM镜像；后续由W25Q64读写层替换其装载和提交动作。 */
static fun_uart_profile_t s_uart_profile;
static bool s_uart_profile_ready = false;

static void uart_start_dma(uart_data_t *data);
static void uart_stop_dma(void);

static size_t uart_tx_ring_free(void)
{
    return (size_t)((s_uart_tx_tail - s_uart_tx_head - 1u) &
                    UART_TX_RING_MASK);
}

static size_t uart_tx_ring_push(const uint8_t *data, size_t length)
{
    size_t pushed = 0;
    size_t free_space = uart_tx_ring_free();

    if (data == NULL || length == 0) return 0;
    if (length > free_space) length = free_space;

    while (pushed < length) {
        s_uart_tx_ring[s_uart_tx_head] = data[pushed++];
        s_uart_tx_head = (uint16_t)((s_uart_tx_head + 1u) &
                                    UART_TX_RING_MASK);
    }
    return pushed;
}

static void uart_tx_ring_flush(uart_data_t *data)
{
    if (data == NULL || s_uart_tx_tail == s_uart_tx_head) return;

    while (s_uart_tx_tail != s_uart_tx_head) {
        uint16_t tail = s_uart_tx_tail;
        size_t available = (size_t)((s_uart_tx_head - tail) &
                                    UART_TX_RING_MASK);
        size_t contiguous = UART_TX_RING_SIZE - tail;

        if (contiguous > available) contiguous = available;

        //  size_t written = fun_uart_write_nonblocking(
        //     &data->uart,
        //     &s_uart_tx_ring[tail],
        //     contiguous);
        size_t written = 0;

        if (data->active_backend == UART_BACKEND_HARDWARE) {
            written = fun_uart_write_nonblocking(
                &data->uart,
                &s_uart_tx_ring[tail],
                contiguous
            );
        }
        else if (data->active_backend == UART_BACKEND_PIO) {
            written = fun_pio_uart_write_nonblocking(
                &data->pio_uart,
                &s_uart_tx_ring[tail],
                contiguous
            );
        }

        s_uart_tx_tail = (uint16_t)((tail + written) &
                                    UART_TX_RING_MASK);
        data->usb_to_uart_count += (uint32_t)written;

        if (written < contiguous) break;
    }
}

static void uart_queue_push(const char *tag, const uint8_t *data, size_t length, uint16_t color)
{
    if (data == NULL || length == 0) return;

    uint8_t next = (uint8_t)((s_uart_term_head + 1) % UART_TERM_QUEUE_SIZE);
    if (next == s_uart_term_tail) return;

    uart_term_message_t *message = &s_uart_term_queue[s_uart_term_head];
    size_t index = 0;

    if (tag != NULL) {
        while (*tag != '\0' && index < UART_TERM_LINE_SIZE - 1) {
            message->text[index++] = *tag++;
        }
    }
    if (index < UART_TERM_LINE_SIZE - 1) {
        message->text[index++] = ' ';
    }

    for (size_t i = 0; i < length && index < UART_TERM_LINE_SIZE - 1; ++i) {
        uint8_t value = data[i];
        if (value == '\r' || value == '\n') continue;
        if (value >= 32 && value <= 126) {
            message->text[index++] = (char)value;
        } else {
            message->text[index++] = '.';
        }
    }

    message->text[index] = '\0';
    message->color = color;
    s_uart_term_head = next;
}

static void uart_start_dma(uart_data_t *data)
{
    if (data == NULL || data->uart.uart_hw == NULL) return;

    if (s_uart_dma_channel < 0) {
        s_uart_dma_channel = dma_claim_unused_channel(true);
    }

    s_uart_dma_tail = 0;

    dma_channel_config config = dma_channel_get_default_config(s_uart_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_ring(&config, true, UART_DMA_RING_BITS);
    channel_config_set_dreq(&config, uart_get_dreq(data->uart.uart_hw, false));

    dma_channel_configure(
        s_uart_dma_channel,
        &config,
        s_uart_dma_ring,
        &uart_get_hw(data->uart.uart_hw)->dr,
        0xFFFFFFFFu,
        true);
}

static void uart_stop_dma(void)
{
    if (s_uart_dma_channel >= 0) {
        dma_channel_abort(s_uart_dma_channel);
        dma_channel_unclaim(s_uart_dma_channel);
        s_uart_dma_channel = -1;
    }
    s_uart_dma_tail = 0;
}
static void uart_close_active_backend(uart_data_t *data)
{
    if (data == NULL) {
        return;
    }

    data->io_enable = false;
    uint32_t start_us = time_us_32();

    while (!data->core1_quiescent &&
           (uint32_t)(time_us_32() - start_us) < 10000u) {
        tight_loop_contents();
    }

    if (data->active_backend == UART_BACKEND_HARDWARE) {
    uart_stop_dma();

    if (fun_uart_is_open(&data->uart)) {
        fun_uart_close(&data->uart);
    }

    /*
     * 释放当前逻辑UART1占用的硬件TX/RX引脚。
     * uart_config中的引脚编号在fun_uart_close()后仍然有效。
     */
    if (data->pin_manager != NULL) {
        (void)pin_manager_release_pair(
            data->pin_manager,
            data->uart_config.tx_gpio,
            data->uart_config.rx_gpio,
            PIN_OWNER_UART,
            1
        );
    }
}
    else if (data->active_backend == UART_BACKEND_PIO) {
    if (fun_pio_uart_is_open(&data->pio_uart)) {
        /*
         * close会清空pio_uart对象，
         * 所以必须提前保存引脚编号。
         */
        uint8_t tx_gpio = data->pio_uart.tx.tx_gpio;
        uint8_t rx_gpio = data->pio_uart.rx.rx_gpio;

        fun_pio_uart_close(&data->pio_uart);

        if (data->pin_manager != NULL) {
            (void)pin_manager_release_pair(
                data->pin_manager,
                tx_gpio,
                rx_gpio,
                PIN_OWNER_UART,
                1
            );
        }
    }
}

    data->selected_pio = UINT8_MAX;
}
static size_t uart_backend_read(uart_data_t *data,uint8_t *buffer,size_t capacity)
{
    if (data == NULL ||
        buffer == NULL ||
        capacity == 0 ||
        !data->io_enable) {
        return 0;
    }

    if (data->active_backend == UART_BACKEND_PIO) {
        return fun_pio_uart_read(
            &data->pio_uart,
            buffer,
            capacity
        );
    }

    if (data->active_backend != UART_BACKEND_HARDWARE ||
        s_uart_dma_channel < 0) {
        return 0;
    }

    uint16_t dma_head = (uint16_t)(
        ((uintptr_t)
            dma_hw->ch[s_uart_dma_channel].write_addr -
         (uintptr_t)s_uart_dma_ring) &
        (UART_DMA_RING_SIZE - 1u)
    );

    size_t count = 0;

    while (s_uart_dma_tail != dma_head &&
           count < capacity) {
        buffer[count++] =
            s_uart_dma_ring[s_uart_dma_tail];

        s_uart_dma_tail =
            (uint16_t)(
                (s_uart_dma_tail + 1u) &
                (UART_DMA_RING_SIZE - 1u)
            );
    }

    return count;
}

// static void uart_core1_worker(void)
// {
//     uart_data_t *data = (uart_data_t *)g_static_app_ram;

//     uint8_t tx_line[UART_TERM_LINE_SIZE];
//     uint8_t rx_line[UART_TERM_LINE_SIZE];
//     size_t tx_length = 0;
//     size_t rx_length = 0;
//     uint64_t tx_last_us = 0;
//     uint64_t rx_last_us = 0;

//     while (true) {
//         if (!s_uart_session_active || !data->io_enable) {
//             data->core1_quiescent = true;
//             tight_loop_contents();
//             continue;
//         }

//         data->core1_quiescent = false;
//         uint64_t now_us = time_us_64();

//         uint16_t dma_head = (uint16_t)(
//             ((uintptr_t)dma_hw->ch[s_uart_dma_channel].write_addr -
//              (uintptr_t)s_uart_dma_ring) &
//             (UART_DMA_RING_SIZE - 1));

//         uint8_t usb_output[64];
//         size_t output_length = 0;

//         while (s_uart_dma_tail != dma_head) {
//             uint8_t value = s_uart_dma_ring[s_uart_dma_tail];
//             s_uart_dma_tail = (uint16_t)((s_uart_dma_tail + 1) & (UART_DMA_RING_SIZE - 1));

//             if (output_length < sizeof(usb_output)) {
//                 usb_output[output_length++] = value;
//             }
//             data->uart_to_usb_count++;

//             if (rx_length < sizeof(rx_line) - 1) {
//                 rx_line[rx_length++] = value;
//             }
//             if (value == '\n' || rx_length >= sizeof(rx_line) - 1) {
//                 uart_queue_push("[RX]", rx_line, rx_length, 0x5E7F);
//                 rx_length = 0;
//             }
//             rx_last_us = now_us;

//             if (output_length == sizeof(usb_output)) {
//                 fun_uart_usb_write(usb_output, output_length);
//                 output_length = 0;
//             }
//         }

//         if (output_length > 0) {
//             fun_uart_usb_write(usb_output, output_length);
//         }

//         uint8_t usb_buffer[64];
//         size_t usb_length = fun_uart_usb_read(
//             usb_buffer,
//             uart_tx_ring_free() < sizeof(usb_buffer)
//                 ? uart_tx_ring_free()
//                 : sizeof(usb_buffer));

//         if (usb_length > 0) {
//             size_t queued = uart_tx_ring_push(usb_buffer, usb_length);
//             for (size_t i = 0; i < queued; ++i) {
//                 if (tx_length < sizeof(tx_line) - 1) {
//                     tx_line[tx_length++] = usb_buffer[i];
//                 }
//                 if (usb_buffer[i] == '\n' || tx_length >= sizeof(tx_line) - 1) {
//                     uart_queue_push("[TX]", tx_line, tx_length, 0xD3BF);
//                     tx_length = 0;
//                 }
//             }
//             tx_last_us = now_us;
//         }

//         uart_tx_ring_flush(data);

//         if (tx_length > 0 && now_us - tx_last_us > 25000) {
//             uart_queue_push("[TX]", tx_line, tx_length, 0xD3BF);
//             tx_length = 0;
//         }
//         if (rx_length > 0 && now_us - rx_last_us > 25000) {
//             uart_queue_push("[RX]", rx_line, rx_length, 0x5E7F);
//             rx_length = 0;
//         }
//     }
// }
static void uart_core1_worker(void)
{
    uart_data_t *data =
        (uart_data_t *)g_static_app_ram;

    uint8_t tx_line[UART_TERM_LINE_SIZE];
    uint8_t rx_line[UART_TERM_LINE_SIZE];

    size_t tx_length = 0;
    size_t rx_length = 0;

    uint64_t tx_last_us = 0;
    uint64_t rx_last_us = 0;

    while (true) {
        /*
         * App退出或切换串口配置时，Core1进入静止状态，
         * 不再访问UART、PIO或DMA资源。
         */
        if (!s_uart_session_active ||
            !data->io_enable) {
            data->core1_quiescent = true;
            tight_loop_contents();
            continue;
        }

        data->core1_quiescent = false;

        uint64_t now_us = time_us_64();

        /*
         * 根据backend读取数据：
         *
         * 硬件UART -> DMA环形缓冲
         * PIO UART -> PIO RX FIFO
         */
        uint8_t backend_input[64];

        size_t backend_length =
            uart_backend_read(
                data,
                backend_input,
                sizeof(backend_input)
            );

        /*
         * 将外部串口接收到的数据：
         *
         * 1. 透传到USB
         * 2. 累加RX计数
         * 3. 提交到终端日志队列
         */
        uint8_t usb_output[64];
        size_t output_length = 0;

        for (size_t i = 0;
             i < backend_length;
             ++i) {
            uint8_t value = backend_input[i];

            if (output_length <
                sizeof(usb_output)) {
                usb_output[output_length++] =
                    value;
            }

            data->uart_to_usb_count++;

            if (rx_length <
                sizeof(rx_line) - 1u) {
                rx_line[rx_length++] = value;
            }

            if (value == '\n' ||
                rx_length >=
                    sizeof(rx_line) - 1u) {
                uart_queue_push(
                    "[RX]",
                    rx_line,
                    rx_length,
                    0x5E7F
                );

                rx_length = 0;
            }

            rx_last_us = now_us;

            if (output_length ==
                sizeof(usb_output)) {
                fun_uart_usb_write(
                    usb_output,
                    output_length
                );

                output_length = 0;
            }
        }

        if (output_length > 0) {
            fun_uart_usb_write(
                usb_output,
                output_length
            );
        }

        /*
         * 从USB CDC读取待发送数据。
         * 根据TX软件环形缓冲的剩余空间限制读取长度。
         */
        uint8_t usb_buffer[64];

        size_t tx_free =
            uart_tx_ring_free();

        size_t usb_capacity =
            tx_free < sizeof(usb_buffer)
                ? tx_free
                : sizeof(usb_buffer);

        size_t usb_length =
            fun_uart_usb_read(
                usb_buffer,
                usb_capacity
            );

        if (usb_length > 0) {
            size_t queued =
                uart_tx_ring_push(
                    usb_buffer,
                    usb_length
                );

            for (size_t i = 0;
                 i < queued;
                 ++i) {
                if (tx_length <
                    sizeof(tx_line) - 1u) {
                    tx_line[tx_length++] =
                        usb_buffer[i];
                }

                if (usb_buffer[i] == '\n' ||
                    tx_length >=
                        sizeof(tx_line) - 1u) {
                    uart_queue_push(
                        "[TX]",
                        tx_line,
                        tx_length,
                        0xD3BF
                    );

                    tx_length = 0;
                }
            }

            tx_last_us = now_us;
        }

        /*
         * 将TX软件环形缓冲发送到当前后端：
         *
         * 硬件UART -> 硬件TX FIFO
         * PIO UART -> PIO TX FIFO
         */
        uart_tx_ring_flush(data);

        /*
         * 没有换行的数据超过25ms后也提交到终端，
         * 避免短指令一直停留在临时行缓冲里。
         */
        if (tx_length > 0 &&
            now_us - tx_last_us > 25000u) {
            uart_queue_push(
                "[TX]",
                tx_line,
                tx_length,
                0xD3BF
            );

            tx_length = 0;
        }

        if (rx_length > 0 &&
            now_us - rx_last_us > 25000u) {
            uart_queue_push(
                "[RX]",
                rx_line,
                rx_length,
                0x5E7F
            );

            rx_length = 0;
        }

        tight_loop_contents();
    }
}
static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data)
{
    (void)old_tab; (void)new_tab;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data) {
        if (data->keyboard.is_open) {
            ui_keyboard_close(&data->keyboard);
        }
        // 切页时确保关闭所有展开的下拉框
        data->baud.is_open = false;
        data->dirty_rect = tv->content_rect;
    }
}

static void uart_send_text(uart_data_t *data, const char *text)
{
    if (data == NULL || text == NULL || text[0] == '\0') return;

    size_t length = strlen(text);
    if (uart_tx_ring_free() < length + 2u) return;

    (void)uart_tx_ring_push((const uint8_t *)text, length);
    (void)uart_tx_ring_push((const uint8_t *)"\r\n", 2u);

    ui_terminal_write_line(&data->terminal, text, 0xD3BF);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_clear_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;
    ui_terminal_clear(&data->terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_cmd_input_focus(ui_input_t *input, void *user_data)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_send_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL || data->input_cmd.text[0] == '\0') return;
    uart_send_text(data, data->input_cmd.text);
    ui_input_clear(&data->input_cmd);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
}

static void on_keyboard(ui_keyboard_t *keyboard, const char *text, void *user_data)
{
    (void)keyboard;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;
    if (text != NULL && text[0] != '\0') {
        uart_send_text(data, text);
    }
    ui_input_clear(&data->input_cmd);
    ui_keyboard_close(&data->keyboard);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_data_bit7(ui_btn_t* btn,void *user_data){
    uart_data_t *data = (uart_data_t *)user_data;
    if(data==NULL) return;
        if (data->backend == UART_BACKEND_PIO) {
        data->data_bit7.is_checked = false;
        data->data_bit8.is_checked = true;
        data->dirty_rect = data->setting_scroll.view_rect;
        return;
    }
    data->data_bit7.is_checked=true;
    data->data_bit8.is_checked=false;

    data->dirty_rect = data->setting_scroll.view_rect;
}
static void on_data_bit8(ui_btn_t *btn,void *user_data){
    uart_data_t *data = (uart_data_t *)user_data;
    if(data==NULL) return;
    data->data_bit7.is_checked=false;
    data->data_bit8.is_checked=true;
    data->dirty_rect = data->setting_scroll.view_rect;
}

static void on_par_none(ui_btn_t *btn,void *user_data)
{
    uart_data_t *data=(uart_data_t *)user_data;
    if(!data) return;
    data->parity_none.is_checked=true;
    data->parity_even.is_checked=false;
    data->parity_odd.is_checked=false;
    data->dirty_rect = data->setting_scroll.view_rect;
}
static void on_par_even(ui_btn_t *btn,void *user_data)
{
    uart_data_t *data=(uart_data_t *)user_data;
    if(!data) return;

    if (data->backend == UART_BACKEND_PIO) {
        data->parity_none.is_checked = true;
        data->parity_even.is_checked = false;
        data->parity_odd.is_checked = false;
        data->dirty_rect = data->setting_scroll.view_rect;
        return;
    }
    data->parity_none.is_checked=false;
    data->parity_even.is_checked=true;
    data->parity_odd.is_checked=false;
    data->dirty_rect = data->setting_scroll.view_rect;
}
static void on_par_odd(ui_btn_t *btn,void *user_data)
{
    uart_data_t *data=(uart_data_t *)user_data;
    if(!data) return;
    if (data->backend == UART_BACKEND_PIO) {
    data->parity_none.is_checked = true;
    data->parity_even.is_checked = false;
    data->parity_odd.is_checked = false;
    data->dirty_rect = data->setting_scroll.view_rect;
    return;
}
    data->parity_none.is_checked=false;
    data->parity_even.is_checked=false;
    data->parity_odd.is_checked=true;
    data->dirty_rect = data->setting_scroll.view_rect;
}
static void on_stop_bit1(ui_btn_t *btn,void *user_data){
    uart_data_t *data=(uart_data_t *)user_data;
    if(!data) return;
    data->stop_bit1.is_checked=true;
    data->stop_bit2.is_checked=false;
    data->dirty_rect = data->setting_scroll.view_rect;
}
static void on_stop_bit2(ui_btn_t *btn,void *user_data){
    uart_data_t *data=(uart_data_t *)user_data;
    if(!data) return;
        if (data->backend == UART_BACKEND_PIO) {
        data->stop_bit1.is_checked = true;
        data->stop_bit2.is_checked = false;
        data->dirty_rect = data->setting_scroll.view_rect;
        return;
    }
    data->stop_bit1.is_checked=false;
    data->stop_bit2.is_checked=true;
    data->dirty_rect = data->setting_scroll.view_rect;
}
static void uart_layout_pin_configs(uart_data_t *data)
{
    if (data == NULL) return;

    uint8_t tx_gpio =
        ui_pin_config_get_selected(&data->tx_pin_config);
    uint8_t rx_gpio =
        ui_pin_config_get_selected(&data->rx_pin_config);

    ui_pin_config_set_excluded_mask(
        &data->tx_pin_config,
        rx_gpio < 32u ? (1u << rx_gpio) : 0u
    );
    ui_pin_config_set_excluded_mask(
        &data->rx_pin_config,
        tx_gpio < 32u ? (1u << tx_gpio) : 0u
    );

    data->tx_pin_config.rect.y = UART_TX_PIN_Y;
    data->tx_pin_config.rect.h =
        ui_pin_config_get_preferred_height(&data->tx_pin_config);

    data->rx_pin_config.rect.y = (int16_t)(
        data->tx_pin_config.rect.y + data->tx_pin_config.rect.h + 8
    );
    data->rx_pin_config.rect.h =
        ui_pin_config_get_preferred_height(&data->rx_pin_config);

    data->apply_config.rect.y = (int16_t)(
        data->rx_pin_config.rect.y + data->rx_pin_config.rect.h + 12
    );

    data->setting_scroll.content_h = (int16_t)(
        data->apply_config.rect.y + data->apply_config.rect.h + 16
    );
}

static void on_pin_config_changed(
    ui_pin_config_t *config,
    uint8_t gpio,
    void *user_data
)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;

    if (config == &data->tx_pin_config) {
        ui_pin_config_set_excluded_mask(
            &data->rx_pin_config,
            gpio < 32u ? (1u << gpio) : 0u
        );
    }
    else if (config == &data->rx_pin_config) {
        ui_pin_config_set_excluded_mask(
            &data->tx_pin_config,
            gpio < 32u ? (1u << gpio) : 0u
        );
    }

    /* 引脚组合决定实际后端，不再要求用户手动选择硬件或PIO。 */
    uint8_t tx_gpio = ui_pin_config_get_selected(&data->tx_pin_config);
    uint8_t rx_gpio = ui_pin_config_get_selected(&data->rx_pin_config);
    data->backend = fun_uart_probe_hw_capability(tx_gpio, rx_gpio) != FUN_UART_HW_NONE
        ? UART_BACKEND_HARDWARE
        : UART_BACKEND_PIO;

    /* 当前PIO程序只支持8-N-1，切换到PIO组合时同步修正预选格式。 */
    if (data->backend == UART_BACKEND_PIO) {
        data->data_bit7.is_checked = false;
        data->data_bit8.is_checked = true;
        data->parity_none.is_checked = true;
        data->parity_even.is_checked = false;
        data->parity_odd.is_checked = false;
        data->stop_bit1.is_checked = true;
        data->stop_bit2.is_checked = false;
    }

    data->dirty_rect = data->setting_scroll.view_rect;
}
// static void on_apply_config(ui_btn_t *btn, void *user_data)
// {
//     (void)btn;
//     uart_data_t *data = (uart_data_t *)user_data;
//     if (data == NULL) return;

//     uint8_t b_idx = (data->baud.selected_idx >= 0 && (size_t)data->baud.selected_idx < sizeof(s_baud_values)/sizeof(s_baud_values[0]))
//                     ? (uint8_t)data->baud.selected_idx : 4;
//     uint32_t baudrate = s_baud_values[b_idx];
//     /* 数据位/校验位/停止位按钮尚未接入，当前保持标准 8-N-1。 */
//     uint8_t data_bits = data->data_bit7.is_checked?7:8;
//     fun_uart_parity_t parity=FUN_UART_PARITY_NONE;
//     if(data->parity_none.is_checked)
//         parity=FUN_UART_PARITY_NONE;
//     else if(data->parity_even.is_checked)
//         parity=FUN_UART_PARITY_EVEN;
//     else if(data->parity_odd.is_checked)
//         parity=FUN_UART_PARITY_ODD;
//     uint8_t stop_bits = data->stop_bit2.is_checked ? 2:1;
//     uint8_t tx_gpio =20;
//     uint8_t rx_gpio =21;
//     if(data->pin_0405.is_checked){
//         tx_gpio=4;
//         rx_gpio=5;
//     }

//     // 1. 短暂暂停传输
//     data->io_enable = false;
//     sleep_us(500);

//     uart_stop_dma();

//     // 2. 更新并重新初始化硬件
//     data->uart_config.baudrate = baudrate;
//     data->uart_config.data_bits = data_bits;
//     data->uart_config.stop_bits = stop_bits;
//     data->uart_config.parity = parity;
//     data->uart_config.tx_gpio=tx_gpio;
//     data->uart_config.rx_gpio=rx_gpio;

//     if (fun_uart_open(&data->uart, &data->uart_config)) {
      
//         uart_start_dma(data);
//         data->io_enable = true;

//         // 3. 在终端追加一条绿色状态反馈
//         char apply_msg[64];
//         snprintf(apply_msg, sizeof(apply_msg), "[CFG] %lu %d-%s-%d TX%u RX%u",
//                  baudrate, data_bits,
//                  parity == FUN_UART_PARITY_NONE ? "N" : (parity == FUN_UART_PARITY_EVEN ? "E" : "O"),
//                  stop_bits,
//                  tx_gpio,
//                  rx_gpio);
                 
//         ui_terminal_write_line(&data->terminal, apply_msg, 0xBDF7);
//         ui_toast_show(&data->toast,"串口配置已生效",UI_TOAST_SUCCESS,to_ms_since_boot(get_absolute_time()));
//     }else{
//          ui_toast_show(&data->toast,"串口配置失败",UI_TOAST_ERROR,to_ms_since_boot(get_absolute_time()));
//     }

//     data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
// }
static void on_apply_config(ui_btn_t *btn,void *user_data)
{
    (void)btn;

    uart_data_t *data =
        (uart_data_t *)user_data;

    if (data == NULL) {
        return;
    }

    uint8_t baud_index =
        (data->baud.selected_idx >= 0 &&
         (size_t)data->baud.selected_idx <
             sizeof(s_baud_values) /
             sizeof(s_baud_values[0]))
            ? (uint8_t)data->baud.selected_idx
            : 4;

    uint32_t baudrate =
        s_baud_values[baud_index];

    uint8_t data_bits =
        data->data_bit7.is_checked ? 7 : 8;

    fun_uart_parity_t parity =
        FUN_UART_PARITY_NONE;

    if (data->parity_even.is_checked) {
        parity = FUN_UART_PARITY_EVEN;
    }
    else if (data->parity_odd.is_checked) {
        parity = FUN_UART_PARITY_ODD;
    }

    uint8_t stop_bits =
        data->stop_bit2.is_checked ? 2 : 1;

    uint8_t tx_gpio =
        ui_pin_config_get_selected(&data->tx_pin_config);
    uint8_t rx_gpio =
        ui_pin_config_get_selected(&data->rx_pin_config);

    /*
     * 保存用户选择的目标后端。
     * 关闭函数使用active_backend判断旧资源类型。
     */
    uart_backend_t target_backend =
        fun_uart_probe_hw_capability(tx_gpio, rx_gpio) != FUN_UART_HW_NONE
            ? UART_BACKEND_HARDWARE : UART_BACKEND_PIO;

    /* 先校验新配置，失败时保留当前正在工作的串口。 */
    if (tx_gpio >= 32u || rx_gpio >= 32u || tx_gpio == rx_gpio) {
        ui_toast_show(
            &data->toast,
            "TX/RX pin invalid",
            UI_TOAST_ERROR,
            to_ms_since_boot(get_absolute_time())
        );
        data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
        return;
    }

    if (target_backend == UART_BACKEND_PIO &&
        (data_bits != 8 || parity != FUN_UART_PARITY_NONE || stop_bits != 1)) {
        ui_toast_show(
            &data->toast,
            "PIO UART only supports 8-N-1",
            UI_TOAST_ERROR,
            to_ms_since_boot(get_absolute_time())
        );
        data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
        return;
    }

    /*
     * 停止Core1并释放当前活动后端。
     */
    uart_close_active_backend(data);

    /*
     * 丢弃旧后端尚未发送的数据，避免应用新配置后
     * 把旧数据发送到新串口。
     */
    s_uart_tx_head = 0;
    s_uart_tx_tail = 0;

   bool opened = false;
bool pins_claimed = false;

/*
 * 当前UART App暂时作为逻辑UART1。
 * 后面扩展多通道时，再把instance改为通道编号。
 */
pin_source_t target_source =
    target_backend == UART_BACKEND_PIO
        ? PIN_SOURCE_PIO
        : PIN_SOURCE_HARDWARE;

pin_claim_t tx_claim = {
    .owner = PIN_OWNER_UART,
    .source = target_source,
    .function = PIN_FUNC_UART_TX,
    .instance = 1,
    .role = "TX",
    .owner_name = "UART1"
};

pin_claim_t rx_claim = {
    .owner = PIN_OWNER_UART,
    .source = target_source,
    .function = PIN_FUNC_UART_RX,
    .instance = 1,
    .role = "RX",
    .owner_name = "UART1"
};

/*
 * PIO目前只支持8-N-1。
 * 不支持的格式不申请引脚。
 */
bool format_supported =
    target_backend == UART_BACKEND_HARDWARE ||
    (data_bits == 8 &&
     parity == FUN_UART_PARITY_NONE &&
     stop_bits == 1);

if (format_supported && data->pin_manager != NULL) {
    pins_claimed = pin_manager_claim_pair(
        data->pin_manager,
        tx_gpio,
        &tx_claim,
        rx_gpio,
        &rx_claim
    );
}

if (pins_claimed &&
    target_backend == UART_BACKEND_HARDWARE) {
    data->uart_config.baudrate = baudrate;
    data->uart_config.data_bits = data_bits;
    data->uart_config.stop_bits = stop_bits;
    data->uart_config.parity = parity;
    data->uart_config.tx_gpio = tx_gpio;
    data->uart_config.rx_gpio = rx_gpio;

    opened = fun_uart_open(
        &data->uart,
        &data->uart_config
    );

    if (opened) {
        uart_start_dma(data);
        data->active_backend = UART_BACKEND_HARDWARE;
        data->selected_pio = UINT8_MAX;
    }
}
else if (pins_claimed &&
         target_backend == UART_BACKEND_PIO) {
    opened = fun_pio_uart_open_auto(
        &data->pio_uart,
        pio_manager_get_default(),
        tx_gpio,
        rx_gpio,
        baudrate,
        &data->selected_pio
    );

    if (opened) {
        data->active_backend = UART_BACKEND_PIO;
    }
}

/*
 * 引脚申请成功但底层后端打开失败：
 * 回滚TX/RX占用。
 */
if (pins_claimed && !opened) {
    (void)pin_manager_release_pair(
        data->pin_manager,
        tx_gpio,
        rx_gpio,
        PIN_OWNER_UART,
        1
    );
}

    if (opened) {
        data->io_enable = true;
        data->uart_mode = UART_WORK_BRIDGE;

        s_uart_profile.baudrate = baudrate;
        s_uart_profile.data_bits = data_bits;
        s_uart_profile.parity = (uint8_t)parity;
        s_uart_profile.stop_bits = stop_bits;
        s_uart_profile.tx_gpio = tx_gpio;
        s_uart_profile.rx_gpio = rx_gpio;
        fun_uart_profile_finalize(&s_uart_profile);
        s_uart_profile_ready = fun_uart_profile_is_valid(&s_uart_profile);

        char apply_message[80];

        if (data->active_backend ==
            UART_BACKEND_HARDWARE) {
            snprintf(
                apply_message,
                sizeof(apply_message),
                "[CFG] HW %lu %u-%s-%u TX%u RX%u",
                (unsigned long)baudrate,
                data_bits,
                parity == FUN_UART_PARITY_NONE
                    ? "N"
                    : (parity == FUN_UART_PARITY_EVEN
                        ? "E"
                        : "O"),
                stop_bits,
                tx_gpio,
                rx_gpio
            );
        }
        else {
            snprintf(
                apply_message,
                sizeof(apply_message),
                "[CFG] PIO%u %lu 8-N-1 TX%u RX%u",
                data->selected_pio,
                (unsigned long)baudrate,
                tx_gpio,
                rx_gpio
            );
        }

        ui_terminal_write_line(
            &data->terminal,
            apply_message,
            0xBDF7
        );

        ui_toast_show(
            &data->toast,
            "串口配置已生效",
            UI_TOAST_SUCCESS,
            to_ms_since_boot(get_absolute_time())
        );
    }
    else {
        data->io_enable = false;
        data->uart_mode = UART_WORK_IDLE;
        data->selected_pio = UINT8_MAX;

        ui_toast_show(
            &data->toast,
            "串口配置失败",
            UI_TOAST_ERROR,
            to_ms_since_boot(
                get_absolute_time()
            )
        );
    }

    data->dirty_rect =
        rect_union_box(
            data->dirty_rect,
            data->setting_scroll.view_rect
        );
}
// static void on_status_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,int16_t scroll_y, void *user_data)
// {
//     uart_data_t *data = (uart_data_t *)user_data;
//     if(!data) return;
//     const int16_t x = viewport->x + 18;
//     const int16_t y = viewport->y - scroll_y + 18;
//     const uint16_t title_color = 0x4208;
//     const uint16_t value_color = 0x632C;
//     const uint16_t bg_color = 0xFFFF;
//     char text[64];
//     ui_draw_string(tile,x,y,"串口运行状态",FONT_20,title_color,bg_color);
//     snprintf(text,sizeof(text),"状态: %s",data->uart.opened ? "已运行":"未运行");
//     ui_draw_string(tile,x,y+34,text,FONT_16,value_color,bg_color);
//     snprintf(text,sizeof(text),"引脚: TX GP%u / RX GP%u",data->uart_config.tx_gpio,data->uart_config.rx_gpio);
//     ui_draw_string(tile,x, y + 60,text,FONT_16, value_color,bg_color);
//     snprintf(text, sizeof(text), "格式: %lu-%u-%s-%u",(unsigned long)data->uart_config.baudrate,data->uart_config.data_bits, data->uart_config.parity == FUN_UART_PARITY_NONE ? "N" :
//     data->uart_config.parity == FUN_UART_PARITY_EVEN ? "E" : "O",data->uart_config.stop_bits);
//     ui_draw_string(tile,x,y + 86,text, FONT_16,value_color,bg_color);
//     snprintf(text,sizeof(text),"RX 字节: %lu",(unsigned long)data->uart_to_usb_count);
//     ui_draw_string(tile,x,y + 112,text,FONT_16,value_color,bg_color);
//     snprintf(text,sizeof(text),"TX 字节: %lu",(unsigned long)data->usb_to_uart_count);
//     ui_draw_string(tile,x, y + 138,text, FONT_16,value_color, bg_color);
//     snprintf(text,sizeof(text),"DMA: %s",s_uart_dma_channel>=0 ? "运行中" : "已停止");
//     ui_draw_string(tile, x,y + 164,text,FONT_16,value_color, bg_color);
// }
static void on_status_draw_content(
    ui_tile_t *tile,
    const ui_rect_t *viewport,
    int16_t scroll_y,
    void *user_data
)
{
    uart_data_t *data =
        (uart_data_t *)user_data;

    if (data == NULL) {
        return;
    }

    const int16_t x =
        viewport->x + 18;

    const int16_t y =
        viewport->y - scroll_y + 18;

    const uint16_t title_color = 0x4208;
    const uint16_t value_color = 0x632C;
    const uint16_t bg_color = 0xFFFF;

    char text[96];

    bool running = false;
    uint8_t tx_gpio = 0;
    uint8_t rx_gpio = 0;
    uint32_t baudrate = 0;

    if (data->active_backend ==
        UART_BACKEND_HARDWARE) {
        running =
            fun_uart_is_open(&data->uart);

        tx_gpio =
            data->uart_config.tx_gpio;

        rx_gpio =
            data->uart_config.rx_gpio;

        baudrate =
            data->uart_config.baudrate;
    }
    else if (data->active_backend ==
             UART_BACKEND_PIO) {
        running =
            fun_pio_uart_is_open(
                &data->pio_uart
            );

        tx_gpio =
            data->pio_uart.tx.tx_gpio;

        rx_gpio =
            data->pio_uart.rx.rx_gpio;

        baudrate =
            data->pio_uart.tx.baudrate;
    }

    ui_draw_string(
        tile,
        x,
        y,
        "串口运行状态",
        FONT_20,
        title_color,
        bg_color
    );

    snprintf(
        text,
        sizeof(text),
        "实现: %s",
        data->active_backend ==
                UART_BACKEND_PIO
            ? "PIO UART"
            : "硬件 UART"
    );

    ui_draw_string(
        tile,
        x,
        y + 34,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    snprintf(
        text,
        sizeof(text),
        "状态: %s",
        running ? "已运行" : "未运行"
    );

    ui_draw_string(
        tile,
        x,
        y + 60,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    snprintf(
        text,
        sizeof(text),
        "引脚: TX GP%u / RX GP%u",
        tx_gpio,
        rx_gpio
    );

    ui_draw_string(
        tile,
        x,
        y + 86,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    if (data->active_backend ==
        UART_BACKEND_PIO) {
        snprintf(
            text,
            sizeof(text),
            "格式: %lu-8-N-1",
            (unsigned long)baudrate
        );
    }
    else {
        const char *parity_text =
            data->uart_config.parity ==
                    FUN_UART_PARITY_NONE
                ? "N"
                : (data->uart_config.parity ==
                           FUN_UART_PARITY_EVEN
                       ? "E"
                       : "O");

        snprintf(
            text,
            sizeof(text),
            "格式: %lu-%u-%s-%u",
            (unsigned long)baudrate,
            data->uart_config.data_bits,
            parity_text,
            data->uart_config.stop_bits
        );
    }

    ui_draw_string(
        tile,
        x,
        y + 112,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    snprintf(
        text,
        sizeof(text),
        "RX 字节: %lu",
        (unsigned long)
            data->uart_to_usb_count
    );

    ui_draw_string(
        tile,
        x,
        y + 138,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    snprintf(
        text,
        sizeof(text),
        "TX 字节: %lu",
        (unsigned long)
            data->usb_to_uart_count
    );

    ui_draw_string(
        tile,
        x,
        y + 164,
        text,
        FONT_16,
        value_color,
        bg_color
    );

    if (data->active_backend ==
        UART_BACKEND_HARDWARE) {
        snprintf(
            text,
            sizeof(text),
            "DMA: %s",
            s_uart_dma_channel >= 0
                ? "运行中"
                : "已停止"
        );
    }
    else if (data->selected_pio <
             PIO_BLOCK_COUNT) {
        pio_manager_stats_t stats;

        if (pio_manager_get_stats(
                pio_manager_get_default(),
                data->selected_pio,
                &stats)) {
            snprintf(
                text,
                sizeof(text),
                "PIO%u: SM %u/%u  指令 %u/%u",
                data->selected_pio,
                stats.used_sm_count,
                stats.total_sm_count,
                stats.used_cmd_count,
                stats.total_cmd_count
            );
        }
        else {
            snprintf(
                text,
                sizeof(text),
                "PIO资源状态不可用"
            );
        }
    }
    else {
        snprintf(
            text,
            sizeof(text),
            "PIO尚未分配"
        );
    }

    ui_draw_string(
        tile,
        x,
        y + 190,
        text,
        FONT_16,
        value_color,
        bg_color
    );
}
/* ⚡ 修复：独占弹出层事件处理，彻底消除多下拉框同时打开与穿透点击问题 */
static bool on_uart_settings_touch_content(bool is_touching, int16_t content_x, int16_t content_y, uint32_t now_tick, void *user_data)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (!data) return false;

    // 1. 查找当前是否已存在展开的下拉框
    ui_dropdown_t *active_dd = NULL;
    if (data->baud.is_open) active_dd = &data->baud;

    if (active_dd != NULL) {
        ui_rect_t total_r = ui_dropdown_get_total_rect(active_dd);
        if (ui_rect_contains_point(&total_r, content_x, content_y)) {
            (void)ui_dropdown_process_touch(active_dd, is_touching, content_x, content_y, now_tick);
        } else if (is_touching) {
            // 点击下拉框外部区域：收起菜单
            active_dd->is_open = false;
        }
        data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
        return true; // 独占消费，拦截下层所有控件
    }

    // 2. 无下拉框展开时正常按优先级分发（互斥打开）
    bool handled = false;
    if (ui_dropdown_process_touch(&data->baud, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    }else if (ui_btn_process_touch(&data->apply_config, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    }
    if(ui_btn_process_touch(&data->data_bit7,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->data_bit8,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->parity_none,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->parity_even,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->parity_odd,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->stop_bit1,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_btn_process_touch(&data->stop_bit2,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_pin_config_process_touch(&data->tx_pin_config,is_touching,content_x,content_y,now_tick))
        handled=true;
    else if (ui_pin_config_process_touch(&data->rx_pin_config,is_touching,content_x,content_y,now_tick))
        handled=true;
        

    if (handled) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
    }
    return handled;
}

static void on_uart_config_change(ui_dropdown_t *dropdown, int8_t selected_idx, void *user_data)
{
    (void)dropdown; (void)selected_idx;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;
    data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
}

/* ⚡ 修复：Z 轴分层绘制，确保展开的下拉框置顶于最顶层，不被下方控件遮挡 */
static void on_uart_settings_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;

    const int16_t label_screen_x = viewport->x + UART_CFG_LABEL_X;
    const uint16_t label_color   = 0x4208;

    // 1. 绘制左侧对齐标签 (文字垂直居中 +8px)
    ui_draw_string(tile, label_screen_x, viewport->y + UART_BAUD_Y - scroll_y + 8,   "波特率",   FONT_20, label_color, 0xFFFF);
    ui_btn_t apply_config = data->apply_config;
    apply_config.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll, data->apply_config.rect);
    ui_btn_draw(tile, &apply_config);
    ui_dropdown_t baud = data->baud;
    baud.rect = ui_scrollview_content_rect_to_screen(
        &data->setting_scroll, data->baud.rect);
    ui_dropdown_draw(tile, &baud);
    ui_btn_t data_7=data->data_bit7;
    data_7.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->data_bit7.rect);
    ui_btn_draw(tile,&data_7);
    ui_btn_t data_8=data->data_bit8;
    data_8.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->data_bit8.rect);
    ui_btn_draw(tile,&data_8);
    ui_draw_string(tile,label_screen_x,viewport->y+UART_DATA_Y-scroll_y+8,"数据位",FONT_20,label_color,0xFFFF);
    ui_btn_t par_n=data->parity_none;
    par_n.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->parity_none.rect);
    ui_btn_draw(tile,&par_n);
    ui_btn_t par_e=data->parity_even;
    par_e.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->parity_even.rect);
    ui_btn_draw(tile,&par_e);
    ui_btn_t par_o=data->parity_odd;
    par_o.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->parity_odd.rect);
    ui_btn_draw(tile,&par_o);
    ui_draw_string(tile,label_screen_x,viewport->y+UART_PARITY_Y-scroll_y+8,"校验位",FONT_20,label_color,0xFFFF);
    ui_btn_t stop_1=data->stop_bit1;
    stop_1.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->stop_bit1.rect);
    ui_btn_draw(tile,&stop_1);
    ui_btn_t stop_2=data->stop_bit2;
    stop_2.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->stop_bit2.rect);
    ui_btn_draw(tile,&stop_2);
    ui_draw_string(tile,label_screen_x,viewport->y+UART_STOP_Y-scroll_y+8,"停止位",FONT_20,label_color,0xFFFF);
    ui_pin_config_t tx_pin_config = data->tx_pin_config;
    tx_pin_config.rect = ui_scrollview_content_rect_to_screen(
        &data->setting_scroll,
        data->tx_pin_config.rect
    );
    ui_pin_config_draw(tile, &tx_pin_config);

    ui_pin_config_t rx_pin_config = data->rx_pin_config;
    rx_pin_config.rect = ui_scrollview_content_rect_to_screen(
        &data->setting_scroll,
        data->rx_pin_config.rect
    );
    ui_pin_config_draw(tile, &rx_pin_config);
}

static void uarttool_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void uart_enter(app_t *self)
{
    uart_data_t *data = (uart_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(uart_data_t));
    data->pin_manager = pin_manager_get_default();

    if (!s_uart_profile_ready || !fun_uart_profile_is_valid(&s_uart_profile)) {
        fun_uart_profile_set_defaults(&s_uart_profile);
        s_uart_profile_ready = true;
    }

    s_uart_term_head = 0;
    s_uart_term_tail = 0;
    s_uart_tx_head = 0;
    s_uart_tx_tail = 0;

    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = 0;
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_uart_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    ui_scrollview_init(&data->status_scroll, 120, 40, 350, 260,
                       STATUS_CONTENT_H, on_status_draw_content);
    data->status_scroll.user_data = data;
    data->status_scroll.color_bg = 0xFFFF;
    data->status_scroll.color_scrollbar = 0xA514;
    data->status_scroll.show_scrollbar = true;

    ui_scrollview_init(&data->setting_scroll, 120, 40, 350, 260, SETTINGS_CONTENT_H, on_uart_settings_draw_content);
    data->setting_scroll.user_data = data;
    data->setting_scroll.color_bg = 0xFFFF;
    data->setting_scroll.color_scrollbar = 0xA514;
    data->setting_scroll.show_scrollbar = true;
    ui_scrollview_set_touch_cb(&data->setting_scroll, on_uart_settings_touch_content);

    // ⚡ 初始化下拉框参数（采用规整的 UART_CFG_CTRL_X 与 UART_CFG_CTRL_W）
    ui_dropdown_init(&data->baud, UART_CFG_CTRL_X, UART_BAUD_Y, UART_CFG_CTRL_W, UART_CFG_CTRL_H, s_baud_options, sizeof(s_baud_options)/sizeof(s_baud_options[0]), uart_find_baud_index(s_uart_profile.baudrate), on_uart_config_change);
    ui_dropdown_set_max_visible_items(&data->baud, 4);
    data->baud.user_data = data;
    ui_dropdown_set_color(&data->baud, 0xFFFF, 0xC618, 0x4208, 0x03DF);
    ui_btn_init(&data->data_bit7,UART_OPTION_X,UART_DATA_Y,UART_OPTION_W,UART_OPTION_H,"7",on_data_bit7);
    data->data_bit7.user_data=data;
    data->data_bit7.mode=UI_BTN_MODE_PUSH;
    data->data_bit7.style=UI_BTN_STYLE_ROUND;
    data->data_bit7.is_checked=(s_uart_profile.data_bits == 7u);
    ui_btn_init(&data->data_bit8,UART_OPTION_X+UART_OPTION_W+UART_OPTION_GAP,UART_DATA_Y,UART_OPTION_W,UART_OPTION_H,"8",on_data_bit8);
    data->data_bit8.user_data=data;
    data->data_bit8.mode=UI_BTN_MODE_PUSH;
    data->data_bit8.style=UI_BTN_STYLE_ROUND;
    data->data_bit8.is_checked=(s_uart_profile.data_bits == 8u);
    ui_btn_set_color(&data->data_bit7, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_set_color(&data->data_bit8, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->parity_none,UART_OPTION_X,UART_PARITY_Y,UART_OPTION_W,UART_OPTION_H,"无校验",on_par_none);
    data->parity_none.user_data=data;
    data->parity_none.mode=UI_BTN_MODE_PUSH;
    data->parity_none.style=UI_BTN_STYLE_ROUND;
    data->parity_none.is_checked=(s_uart_profile.parity == FUN_UART_PARITY_NONE);
    ui_btn_set_color(&data->parity_none, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->parity_even,UART_OPTION_X + UART_OPTION_W + UART_OPTION_GAP,UART_PARITY_Y,UART_OPTION_W,UART_OPTION_H,"偶校验",on_par_even);
    data->parity_even.user_data=data;
    data->parity_even.mode=UI_BTN_MODE_PUSH;
    data->parity_even.style=UI_BTN_STYLE_ROUND;
    data->parity_even.is_checked=(s_uart_profile.parity == FUN_UART_PARITY_EVEN);
    ui_btn_set_color(&data->parity_even, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->parity_odd, UART_OPTION_X + 2 * (UART_OPTION_W + UART_OPTION_GAP),UART_PARITY_Y,UART_OPTION_W,UART_OPTION_H,"奇校验",on_par_odd);
    data->parity_odd.user_data=data;
    data->parity_odd.mode=UI_BTN_MODE_PUSH;
    data->parity_odd.style=UI_BTN_STYLE_ROUND;
    data->parity_odd.is_checked=(s_uart_profile.parity == FUN_UART_PARITY_ODD);
    ui_btn_set_color(&data->parity_odd, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->stop_bit1,UART_OPTION_X,UART_STOP_Y,UART_OPTION_W,UART_OPTION_H,"1",on_stop_bit1);
    data->stop_bit1.user_data=data;
    data->stop_bit1.is_checked=(s_uart_profile.stop_bits == 1u);
    data->stop_bit1.mode=UI_BTN_MODE_PUSH;
    data->stop_bit1.style=UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->stop_bit1, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->stop_bit2,UART_OPTION_X+UART_OPTION_W+UART_OPTION_GAP,UART_STOP_Y,UART_OPTION_W,UART_OPTION_H,"2",on_stop_bit2);
    data->stop_bit2.user_data=data;
    data->stop_bit2.is_checked=(s_uart_profile.stop_bits == 2u);
    data->stop_bit2.mode=UI_BTN_MODE_PUSH;
    data->stop_bit2.style=UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->stop_bit2, 0xDEFB, 0xD7FF, 0x4208);
    ui_btn_init(&data->apply_config, UART_CFG_CTRL_X, 0, UART_CFG_CTRL_W, 38, "应用串口配置", on_apply_config);
    data->apply_config.user_data = data;
    data->apply_config.style = UI_BTN_STYLE_ROUND;
    data->apply_config.font_size = FONT_20;
    ui_btn_set_color(&data->apply_config, 0x5E7F, 0x0275, 0xFFFF);

    ui_toast_init(&data->toast);
    ui_pin_config_init(
        &data->tx_pin_config,
        UART_CFG_CTRL_X,
        UART_TX_PIN_Y,
        UART_CFG_CTRL_W,
        "发送引脚",
        data->pin_manager,
        PIN_OWNER_UART,
        PIN_SOURCE_NONE,
        PIN_FUNC_UART_TX,
        1,
        s_uart_profile.tx_gpio,
        on_pin_config_changed
    );
    data->tx_pin_config.user_data = data;

    ui_pin_config_init(
        &data->rx_pin_config,
        UART_CFG_CTRL_X,
        0,
        UART_CFG_CTRL_W,
        "接收引脚",
        data->pin_manager,
        PIN_OWNER_UART,
        PIN_SOURCE_NONE,
        PIN_FUNC_UART_RX,
        1,
        s_uart_profile.rx_gpio,
        on_pin_config_changed
    );
    data->rx_pin_config.user_data = data;

    ui_pin_config_set_allowed_mask(&data->tx_pin_config, UINT32_MAX);
    ui_pin_config_set_allowed_mask(&data->rx_pin_config, UINT32_MAX);
    ui_pin_config_set_excluded_mask(&data->tx_pin_config, 1u << 21);
    ui_pin_config_set_excluded_mask(&data->rx_pin_config, 1u << 20);
    uart_layout_pin_configs(data);
    // 终端初始化
    ui_terminal_init(&data->terminal, UART_TERM_X, UART_TERM_Y, UART_TERM_W, UART_TERM_H);
    data->terminal.color_bg        = 0x10A2;
    data->terminal.color_border    = 0x31A6;
    data->terminal.color_text_def  = 0xDEFB;
    data->terminal.color_scrollbar = 0x05DF;
    ui_terminal_write_line(&data->terminal, "串口终端已启动", 0x5E7F);

    ui_input_init(&data->input_cmd, UART_INPUT_X, UART_INPUT_Y, UART_INPUT_W, UART_INPUT_H, "输入串口指令", on_cmd_input_focus);
    data->input_cmd.user_data = data;
    data->input_cmd.color_bg = 0xFFFF;
    data->input_cmd.color_border = 0xC618;
    data->input_cmd.color_focus_border = 0x03DF;
    data->input_cmd.color_text = 0x4208;
    data->input_cmd.color_placeholder = 0x8410;

    ui_btn_init(&data->btn_clear, UART_CLEAR_X, UART_CLEAR_Y, UART_CLEAR_W, UART_CLEAR_H, "清屏", on_clear_click);
    data->btn_clear.user_data = data;
    data->btn_clear.style = UI_BTN_STYLE_ROUND;
    data->btn_clear.font_size = FONT_16;
    ui_btn_set_color(&data->btn_clear, 0x8410, 0x630C, 0xFFFF);

    ui_btn_init(&data->btn_send, UART_SEND_X, UART_SEND_Y, UART_SEND_W, UART_SEND_H, "发送", on_send_click);
    data->btn_send.user_data = data;
    data->btn_send.style = UI_BTN_STYLE_ROUND;
    data->btn_send.font_size = FONT_16;
    ui_btn_set_color(&data->btn_send, 0x03DF, 0x0275, 0xFFFF);

    ui_keyboard_init(&data->keyboard, on_keyboard);
    data->keyboard.user_data = data;


    fun_pio_uart_init(&data->pio_uart);
    data->backend=UART_BACKEND_HARDWARE;
    data->active_backend=UART_BACKEND_HARDWARE;
    data->selected_pio=UINT8_MAX;
    fun_uart_config_init(&data->uart_config);
    data->uart_config.hw_instance = FUN_UART_HW_UART1;
    data->uart_config.tx_gpio = s_uart_profile.tx_gpio;
    data->uart_config.rx_gpio = s_uart_profile.rx_gpio;
    data->uart_config.baudrate = s_uart_profile.baudrate;
    data->uart_config.data_bits = s_uart_profile.data_bits;
    data->uart_config.stop_bits = s_uart_profile.stop_bits;
    data->uart_config.parity = (fun_uart_parity_t)s_uart_profile.parity;
    
    pin_claim_t tx_claim = {
    .owner = PIN_OWNER_UART,
    .source = PIN_SOURCE_HARDWARE,
    .function = PIN_FUNC_UART_TX,
    .instance = 1,
    .role = "TX",
    .owner_name = "UART1"
};

pin_claim_t rx_claim = {
    .owner = PIN_OWNER_UART,
    .source = PIN_SOURCE_HARDWARE,
    .function = PIN_FUNC_UART_RX,
    .instance = 1,
    .role = "RX",
    .owner_name = "UART1"
};

if (!pin_manager_claim_pair(
        data->pin_manager,
        data->uart_config.tx_gpio,
        &tx_claim,
        data->uart_config.rx_gpio,
        &rx_claim)) {
    data->uart_mode = UART_WORK_IDLE;
    data->io_enable = false;
    return;
}

    // if (!fun_uart_open(&data->uart, &data->uart_config)) {
    //     data->uart_mode = UART_WORK_IDLE;
    //     data->io_enable = false;
    //     return;
    // }
    if (!fun_uart_open(&data->uart, &data->uart_config)) {
    /*
     * 引脚登记成功但硬件UART打开失败，
     * 必须回滚GP20/GP21的占用状态。
     */
    (void)pin_manager_release_pair(
        data->pin_manager,
        data->uart_config.tx_gpio,
        data->uart_config.rx_gpio,
        PIN_OWNER_UART,
        1
    );

    data->uart_mode = UART_WORK_IDLE;
    data->io_enable = false;
    return;
}
    uart_start_dma(data);

    data->uart_mode = UART_WORK_BRIDGE;
    data->io_enable = true;
    data->core1_quiescent = false;
    s_uart_session_active = true;

    multicore_reset_core1();
    multicore_launch_core1(uart_core1_worker);
}

// static void uart_exit(app_t *self)
// {
//     uart_data_t *data = (uart_data_t *)self->user_data;
//     if (data != NULL) {
//         bool was_active = s_uart_session_active;
//         s_uart_session_active = false;
//         data->io_enable = false;

//         if (was_active) {
//             multicore_reset_core1();
//         }
//         uart_stop_dma();
//         fun_uart_close(&data->uart);
//         data->core1_quiescent = true;
//     }
//     self->user_data = NULL;
// }
static void uart_exit(app_t *self)
{
    uart_data_t *data =
        (uart_data_t *)self->user_data;

    if (data == NULL) {
        self->user_data = NULL;
        return;
    }

    bool was_active =
        s_uart_session_active;

    /*
     * 通知Core1退出当前串口会话。
     */
    s_uart_session_active = false;

    /*
     * 根据active_backend关闭当前资源：
     *
     * 硬件UART -> 停DMA并关闭UART
     * PIO UART -> 释放TX/RX状态机和程序
     */
    uart_close_active_backend(data);

    /*
     * 后端资源释放后再停止Core1。
     */
    if (was_active) {
        multicore_reset_core1();
    }

    data->core1_quiescent = true;
    data->uart_mode = UART_WORK_IDLE;
    data->selected_pio = UINT8_MAX;

    self->user_data = NULL;
}
static void uart_update(app_t *self, uint32_t now_tick)
{
    
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if(ui_toast_update(&data->toast,now_tick))
        data->dirty_rect = rect_union_box(data->dirty_rect,ui_toast_get_dirty_union(&data->toast));
    bool terminal_updata = false;

    while (s_uart_term_tail != s_uart_term_head) {
        uart_term_message_t *message = &s_uart_term_queue[s_uart_term_tail];
        ui_terminal_write_line(&data->terminal, message->text, message->color);
        s_uart_term_tail = (uint8_t)((s_uart_term_tail + 1) % UART_TERM_QUEUE_SIZE);
        terminal_updata = true;
    }

    if (terminal_updata && data->tabview.active_tab_idx == 2 && !data->keyboard.is_open) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    }

    if (data->tabview.active_tab_idx == 0) {
        if (data->usb_to_uart_count != data->last_usb_to_uart_count ||
            data->uart_to_usb_count != data->last_uart_to_usb_count) {
            data->last_usb_to_uart_count = data->usb_to_uart_count;
            data->last_uart_to_usb_count = data->uart_to_usb_count;
            data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        }
    }
}

static bool uart_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data) return false;

    if (data->tx_pin_config.modal.is_open) {
        bool was_open = true;
        bool handled = ui_pin_config_process_modal_touch(
            &data->tx_pin_config, is_touching, tx, ty, now_tick);
        data->dirty_rect = was_open && data->tx_pin_config.modal.is_open
            ? data->tx_pin_config.modal.rect
            : ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return handled;
    }
    if (data->rx_pin_config.modal.is_open) {
        bool was_open = true;
        bool handled = ui_pin_config_process_modal_touch(
            &data->rx_pin_config, is_touching, tx, ty, now_tick);
        data->dirty_rect = was_open && data->rx_pin_config.modal.is_open
            ? data->rx_pin_config.modal.rect
            : ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return handled;
    }

    if (data->keyboard.is_open) {
        if (ui_keyboard_process_touch(&data->keyboard, is_touching, tx, ty, now_tick)) {
            if (!data->keyboard.is_open) {
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            } else {
                data->dirty_rect = rect_union_box(data->dirty_rect, data->keyboard.rect);
                data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
            }
            return true;
        }
        if (is_touching && !ui_rect_contains_point(&data->keyboard.rect, tx, ty)) {
            ui_keyboard_close(&data->keyboard);
            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            return true;
        }
        return false;
    }

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect = data->tabview.rect;
        } else {
            data->dirty_rect = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    bool handled = false;

    switch (data->tabview.active_tab_idx) {
    case 0:
        if (ui_scrollview_process_touch(&data->status_scroll, is_touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
            handled = true;
        }
        break;

    case 1:
        /* 展开的下拉框独占纵向手势，内部滑动时不带动配置页。 */
        if (data->baud.is_open) {
            int16_t content_x = tx - data->setting_scroll.view_rect.x;
            int16_t content_y = ty - data->setting_scroll.view_rect.y +
                                data->setting_scroll.scroll_y;
            if (on_uart_settings_touch_content(is_touching, content_x,
                                               content_y, now_tick, data)) {
                data->dirty_rect = rect_union_box(
                    data->dirty_rect,
                    data->setting_scroll.view_rect);
                handled = true;
            }
        } else if (ui_scrollview_process_touch(&data->setting_scroll, is_touching, tx, ty, now_tick)) {
            if (data->tx_pin_config.modal.is_open || data->rx_pin_config.modal.is_open)
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            else
                data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
            handled = true;
        }
        break;

    case 2: {
        if (ui_terminal_process_touch(&data->terminal, is_touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->btn_clear, is_touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_clear));
            handled = true;
        }
        if (ui_input_process_touch(&data->input_cmd, is_touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->btn_send, is_touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_send));
            handled = true;
        }
        break;
    }
    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void uart_draw(app_t *self, ui_tile_t *tile)
{
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (!data->keyboard.is_open && !ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }


    switch (data->tabview.active_tab_idx) {
    case 0:
        ui_scrollview_draw(tile, &data->status_scroll);
        break;

    case 1:
        ui_scrollview_draw(tile, &data->setting_scroll);
        break;

    case 2:
        ui_terminal_draw(tile, &data->terminal);
        ui_input_draw(tile, &data->input_cmd);
        ui_btn_draw(tile, &data->btn_clear);
        ui_btn_draw(tile, &data->btn_send);
        break;

    default:
        break;
    }

    if (data->keyboard.is_open) {
        ui_keyboard_draw(tile, &data->keyboard);
    }
    ui_toast_draw(tile, &data->toast);
    ui_pin_config_draw_modal(tile, &data->tx_pin_config);
    ui_pin_config_draw_modal(tile, &data->rx_pin_config);
}

app_t *app_uart_get_instance(void)
{
    s_uart.id = APP_ID_CUSTOM_0;
    s_uart.name = "串口工具";
    s_uart.init = uarttool_init;
    s_uart.enter = uart_enter;
    s_uart.exit = uart_exit;
    s_uart.update = uart_update;
    s_uart.process_touch = uart_process_touch;
    s_uart.draw = uart_draw;
    return &s_uart;
}
