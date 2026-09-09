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
#include "lcd.h"

#include "pico/stdlib.h"
#include <string.h>
#include "fun_uart.h"
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
#define UART_APPLY_Y      224

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

static const char* s_data_bits_options[] = {
    "7", "8"
};
static const char *s_parity_options[] = {
    "无校验", "偶校验", "奇校验"
};
static const char *s_stop_bits_options[] = {
    "1", "2"
};

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
    ui_dropdown_t data_bits;
    ui_dropdown_t parity;
    ui_dropdown_t stop_bits;
    ui_btn_t apply_config;
    ui_toast_t toast;

    fun_uart_t uart;
    fun_uart_config_t uart_config;
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

        // size_t written = fun_uart_write(
        //     &data->uart,
        //     &s_uart_tx_ring[tail],
        //     contiguous);
         size_t written = fun_uart_write_nonblocking(
            &data->uart,
            &s_uart_tx_ring[tail],
            contiguous);

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

static void uart_core1_worker(void)
{
    uart_data_t *data = (uart_data_t *)g_static_app_ram;

    uint8_t tx_line[UART_TERM_LINE_SIZE];
    uint8_t rx_line[UART_TERM_LINE_SIZE];
    size_t tx_length = 0;
    size_t rx_length = 0;
    uint64_t tx_last_us = 0;
    uint64_t rx_last_us = 0;

    while (true) {
        if (!s_uart_session_active || !data->io_enable) {
            data->core1_quiescent = true;
            tight_loop_contents();
            continue;
        }

        data->core1_quiescent = false;
        uint64_t now_us = time_us_64();

        uint16_t dma_head = (uint16_t)(
            ((uintptr_t)dma_hw->ch[s_uart_dma_channel].write_addr -
             (uintptr_t)s_uart_dma_ring) &
            (UART_DMA_RING_SIZE - 1));

        uint8_t usb_output[64];
        size_t output_length = 0;

        while (s_uart_dma_tail != dma_head) {
            uint8_t value = s_uart_dma_ring[s_uart_dma_tail];
            s_uart_dma_tail = (uint16_t)((s_uart_dma_tail + 1) & (UART_DMA_RING_SIZE - 1));

            if (output_length < sizeof(usb_output)) {
                usb_output[output_length++] = value;
            }
            data->uart_to_usb_count++;

            if (rx_length < sizeof(rx_line) - 1) {
                rx_line[rx_length++] = value;
            }
            if (value == '\n' || rx_length >= sizeof(rx_line) - 1) {
                uart_queue_push("[RX]", rx_line, rx_length, 0x5E7F);
                rx_length = 0;
            }
            rx_last_us = now_us;

            if (output_length == sizeof(usb_output)) {
                fun_uart_usb_write(usb_output, output_length);
                output_length = 0;
            }
        }

        if (output_length > 0) {
            fun_uart_usb_write(usb_output, output_length);
        }

        uint8_t usb_buffer[64];
        size_t usb_length = fun_uart_usb_read(
            usb_buffer,
            uart_tx_ring_free() < sizeof(usb_buffer)
                ? uart_tx_ring_free()
                : sizeof(usb_buffer));

        if (usb_length > 0) {
            size_t queued = uart_tx_ring_push(usb_buffer, usb_length);
            for (size_t i = 0; i < queued; ++i) {
                if (tx_length < sizeof(tx_line) - 1) {
                    tx_line[tx_length++] = usb_buffer[i];
                }
                if (usb_buffer[i] == '\n' || tx_length >= sizeof(tx_line) - 1) {
                    uart_queue_push("[TX]", tx_line, tx_length, 0xD3BF);
                    tx_length = 0;
                }
            }
            tx_last_us = now_us;
        }

        uart_tx_ring_flush(data);

        if (tx_length > 0 && now_us - tx_last_us > 25000) {
            uart_queue_push("[TX]", tx_line, tx_length, 0xD3BF);
            tx_length = 0;
        }
        if (rx_length > 0 && now_us - rx_last_us > 25000) {
            uart_queue_push("[RX]", rx_line, rx_length, 0x5E7F);
            rx_length = 0;
        }
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
        data->data_bits.is_open = false;
        data->parity.is_open = false;
        data->stop_bits.is_open = false;
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

/* ⚡ 修复：真正实现串口参数的硬件重设与生效 *//* ⚡ 修复：真正实现串口参数的硬件重设与生效 */
static void on_apply_config(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) return;

    uint8_t b_idx = (data->baud.selected_idx >= 0 && (size_t)data->baud.selected_idx < sizeof(s_baud_values)/sizeof(s_baud_values[0]))
                    ? (uint8_t)data->baud.selected_idx : 4;
    uint32_t baudrate = s_baud_values[b_idx];
    uint8_t data_bits = (data->data_bits.selected_idx == 0) ? 7 : 8;
    fun_uart_parity_t parity = (fun_uart_parity_t)data->parity.selected_idx;
    uint8_t stop_bits = (data->stop_bits.selected_idx == 0) ? 1 : 2;

    // 1. 短暂暂停传输
    data->io_enable = false;
    sleep_us(500);

    uart_stop_dma();

    // 2. 更新并重新初始化硬件
    data->uart_config.baudrate = baudrate;
    data->uart_config.data_bits = data_bits;
    data->uart_config.stop_bits = stop_bits;
    data->uart_config.parity = parity;

    if (fun_uart_open(&data->uart, &data->uart_config)) {
      
        uart_start_dma(data);
        data->io_enable = true;

        // 3. 在终端追加一条绿色状态反馈
        char apply_msg[64];
        snprintf(apply_msg, sizeof(apply_msg), "[CFG] %lu %d-%s-%d",
                 baudrate, data_bits,
                 parity == FUN_UART_PARITY_NONE ? "N" : (parity == FUN_UART_PARITY_EVEN ? "E" : "O"),
                 stop_bits);
                 
        ui_terminal_write_line(&data->terminal, apply_msg, 0xBDF7);
        ui_toast_show(&data->toast,"串口配置已生效",UI_TOAST_SUCCESS,to_ms_since_boot(get_absolute_time()));
    }else{
         ui_toast_show(&data->toast,"串口配置失败",UI_TOAST_ERROR,to_ms_since_boot(get_absolute_time()));
    }

    data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
}

static void on_status_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                   int16_t scroll_y, void *user_data)
{
    (void)tile; (void)viewport; (void)scroll_y; (void)user_data;
}

/* ⚡ 修复：独占弹出层事件处理，彻底消除多下拉框同时打开与穿透点击问题 */
static bool on_uart_settings_touch_content(bool is_touching, int16_t content_x, int16_t content_y, uint32_t now_tick, void *user_data)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (!data) return false;

    // 1. 查找当前是否已存在展开的下拉框
    ui_dropdown_t *active_dd = NULL;
    if (data->baud.is_open) active_dd = &data->baud;
    else if (data->data_bits.is_open) active_dd = &data->data_bits;
    else if (data->parity.is_open) active_dd = &data->parity;
    else if (data->stop_bits.is_open) active_dd = &data->stop_bits;

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
    } else if (ui_dropdown_process_touch(&data->data_bits, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    } else if (ui_dropdown_process_touch(&data->parity, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    } else if (ui_dropdown_process_touch(&data->stop_bits, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    }else if (ui_btn_process_touch(&data->apply_config, is_touching, content_x, content_y, now_tick)) {
        handled = true;
    }

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
    ui_draw_string(tile, label_screen_x, viewport->y + UART_BAUD_Y - scroll_y + 8,   "波特率",   FONT_16, label_color, 0xFFFF);
    ui_draw_string(tile, label_screen_x, viewport->y + UART_DATA_Y - scroll_y + 8,   "数据位",   FONT_16, label_color, 0xFFFF);
    ui_draw_string(tile, label_screen_x, viewport->y + UART_PARITY_Y - scroll_y + 8, "校验位",   FONT_16, label_color, 0xFFFF);
    ui_draw_string(tile, label_screen_x, viewport->y + UART_STOP_Y - scroll_y + 8,   "停止位",   FONT_16, label_color, 0xFFFF);
    ui_btn_t apply_config = data->apply_config;
    apply_config.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll, data->apply_config.rect);
    ui_btn_draw(tile, &apply_config);

    // 3. 优先绘制所有处于关闭态的下拉框
    ui_dropdown_t *dds[4] = { &data->baud, &data->data_bits, &data->parity, &data->stop_bits };
    ui_dropdown_t *open_dd = NULL;

    for (int i = 0; i < 4; ++i) {
        if (dds[i]->is_open) {
            open_dd = dds[i]; // 记录下打开的下拉框，留在最后画
        } else {
            ui_dropdown_t copy_dd = *dds[i];
            copy_dd.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll, dds[i]->rect);
            ui_dropdown_draw(tile, &copy_dd);
        }
    }

    // 4. 关键：若存在展开的下拉框，在最顶层绘制（菜单完美浮于所有控件上方）
    if (open_dd != NULL) {
        ui_dropdown_t top_dd = *open_dd;
        top_dd.rect = ui_scrollview_content_rect_to_screen(&data->setting_scroll, open_dd->rect);
        ui_dropdown_draw(tile, &top_dd);
    }
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
    ui_dropdown_init(&data->baud, UART_CFG_CTRL_X, UART_BAUD_Y, UART_CFG_CTRL_W, UART_CFG_CTRL_H, s_baud_options, sizeof(s_baud_options)/sizeof(s_baud_options[0]), 4, on_uart_config_change);
    ui_dropdown_set_max_visible_items(&data->baud, 4);
    ui_dropdown_init(&data->data_bits, UART_CFG_CTRL_X, UART_DATA_Y, UART_CFG_CTRL_W, UART_CFG_CTRL_H, s_data_bits_options, sizeof(s_data_bits_options)/sizeof(s_data_bits_options[0]), 1, on_uart_config_change);
    ui_dropdown_init(&data->parity, UART_CFG_CTRL_X, UART_PARITY_Y, UART_CFG_CTRL_W, UART_CFG_CTRL_H, s_parity_options, sizeof(s_parity_options)/sizeof(s_parity_options[0]), 0, on_uart_config_change);
    ui_dropdown_init(&data->stop_bits, UART_CFG_CTRL_X, UART_STOP_Y, UART_CFG_CTRL_W, UART_CFG_CTRL_H, s_stop_bits_options, sizeof(s_stop_bits_options)/sizeof(s_stop_bits_options[0]), 0, on_uart_config_change);

    data->baud.user_data = data;
    data->data_bits.user_data = data;
    data->parity.user_data = data;
    data->stop_bits.user_data = data;

    ui_dropdown_set_color(&data->baud, 0xFFFF, 0xC618, 0x4208, 0x03DF);
    ui_dropdown_set_color(&data->data_bits, 0xFFFF, 0xC618, 0x4208, 0x03DF);
    ui_dropdown_set_color(&data->parity, 0xFFFF, 0xC618, 0x4208, 0x03DF);
    ui_dropdown_set_color(&data->stop_bits, 0xFFFF, 0xC618, 0x4208, 0x03DF);
    // 应用按键与上方控件等宽同轴 (X: 95, W: 230)
    ui_btn_init(&data->apply_config, UART_CFG_CTRL_X, UART_APPLY_Y, UART_CFG_CTRL_W, 38, "应用串口配置", on_apply_config);
    data->apply_config.user_data = data;
    data->apply_config.style = UI_BTN_STYLE_ROUND;
    data->apply_config.font_size = FONT_16;
    ui_btn_set_color(&data->apply_config, 0x03DF, 0x0275, 0xFFFF);

    ui_toast_init(&data->toast);

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

    fun_uart_config_init(&data->uart_config);
    data->uart_config.hw_instance = FUN_UART_HW_UART1;
    data->uart_config.tx_gpio = 20;
    data->uart_config.rx_gpio = 21;
    data->uart_config.baudrate = 115200;
    data->uart_config.data_bits = 8;
    data->uart_config.stop_bits = 1;
    data->uart_config.parity = FUN_UART_PARITY_NONE;

    if (!fun_uart_open(&data->uart, &data->uart_config)) {
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

static void uart_exit(app_t *self)
{
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (data != NULL) {
        bool was_active = s_uart_session_active;
        s_uart_session_active = false;
        data->io_enable = false;

        if (was_active) {
            multicore_reset_core1();
        }
        uart_stop_dma();
        fun_uart_close(&data->uart);
        data->core1_quiescent = true;
    }
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
        if (data->baud.is_open || data->data_bits.is_open ||
            data->parity.is_open || data->stop_bits.is_open) {
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
