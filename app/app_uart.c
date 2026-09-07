#include "app_uart.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_scrollview.h"
#include "ui_tabview.h"
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

#define STATUS_CONTENT_H 520
#define SETTINGS_CONTENT_H 520
#define UART_DMA_RING_SIZE 2048u
#define UART_DMA_RING_BITS 11u
#define UART_TX_RING_SIZE 1024u
#define UART_TX_RING_MASK (UART_TX_RING_SIZE - 1u)
#define UART_TERM_QUEUE_SIZE 16
#define UART_TERM_LINE_SIZE 64

typedef struct
{
    char text[UART_TERM_LINE_SIZE];
    uint16_t color;
} uart_term_message_t;

static uint8_t s_uart_dma_ring[UART_DMA_RING_SIZE]
    __attribute__((aligned(UART_DMA_RING_SIZE)));

static volatile uint16_t s_uart_dma_tail = 0;
static int s_uart_dma_channel = -1;

/*
 * USB -> UART 发送队列。
 * USB 输入只负责入队，UART FIFO 有空间时由 Core1 逐步发送，避免阻塞
 * DMA 接收和 UART -> USB 转发。该队列只由 Core1 访问。
 */
static uint8_t s_uart_tx_ring[UART_TX_RING_SIZE];
static uint16_t s_uart_tx_head = 0;
static uint16_t s_uart_tx_tail = 0;
static uart_term_message_t s_uart_term_queue[UART_TERM_QUEUE_SIZE];
static volatile uint8_t s_uart_term_head = 0;
static volatile uint8_t s_uart_term_tail = 0;

/* Core1 只在透传期间工作；退出 App 时先停 IO，再复位 Core1。 */
static volatile bool s_uart_session_active = false;

typedef enum
{
    UART_WORK_IDLE = 0,
    UART_WORK_BRIDGE
} uart_mode_t;
/* =========================================================================
 * 1. App 私有数据结构体
 *
 * 【添加新控件 - 第 1 步】：在此声明你的控件实体。
 * 铁律：dirty_rect 必须严格位于第 1 位！
 * ========================================================================= */

typedef struct
{
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab_idx;

    ui_scrollview_t status_scroll;
    ui_scrollview_t setting_scroll;

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

    if (data == NULL || length == 0) {
        return 0;
    }

    if (length > free_space) {
        length = free_space;
    }

    while (pushed < length) {
        s_uart_tx_ring[s_uart_tx_head] = data[pushed++];
        s_uart_tx_head = (uint16_t)((s_uart_tx_head + 1u) &
                                    UART_TX_RING_MASK);
    }

    return pushed;
}

static void uart_tx_ring_flush(uart_data_t *data)
{
    if (data == NULL || s_uart_tx_tail == s_uart_tx_head) {
        return;
    }

    while (s_uart_tx_tail != s_uart_tx_head) {
        uint16_t tail = s_uart_tx_tail;
        size_t available = (size_t)((s_uart_tx_head - tail) &
                                    UART_TX_RING_MASK);
        size_t contiguous = UART_TX_RING_SIZE - tail;

        if (contiguous > available) {
            contiguous = available;
        }

        size_t written = fun_uart_write_nonblocking(
            &data->uart,
            &s_uart_tx_ring[tail],
            contiguous);

        s_uart_tx_tail = (uint16_t)((tail + written) &
                                    UART_TX_RING_MASK);
        data->usb_to_uart_count += (uint32_t)written;

        if (written < contiguous) {
            /* UART FIFO 已满，本轮让出 Core1，下一轮继续。 */
            break;
        }
    }
}

static void uart_queue_push(const char *tag,
                            const uint8_t *data,
                            size_t length,
                            uint16_t color)
{
    if (data == NULL || length == 0)
    {
        return;
    }

    uint8_t next =
        (uint8_t)((s_uart_term_head + 1) %
                  UART_TERM_QUEUE_SIZE);

    if (next == s_uart_term_tail)
    {
        /* 队列满时丢弃本条日志，不能阻塞 Core1。 */
        return;
    }

    uart_term_message_t *message =
        &s_uart_term_queue[s_uart_term_head];

    size_t index = 0;

    if (tag != NULL)
    {
        while (*tag != '\0' &&
               index < UART_TERM_LINE_SIZE - 1)
        {
            message->text[index++] = *tag++;
        }
    }

    if (index < UART_TERM_LINE_SIZE - 1)
    {
        message->text[index++] = ' ';
    }

    for (size_t i = 0;
         i < length &&
         index < UART_TERM_LINE_SIZE - 1;
         ++i)
    {
        uint8_t value = data[i];

        if (value == '\r' || value == '\n')
        {
            continue;
        }

        if (value >= 32 && value <= 126)
        {
            message->text[index++] = (char)value;
        }
        else
        {
            message->text[index++] = '.';
        }
    }

    message->text[index] = '\0';
    message->color = color;

    s_uart_term_head = next;
}

static void uart_start_dma(uart_data_t *data)
{
    if (data == NULL || data->uart.uart_hw == NULL) {
        return;
    }

    if (s_uart_dma_channel < 0) {
        s_uart_dma_channel = dma_claim_unused_channel(true);
    }

    s_uart_dma_tail = 0;

    dma_channel_config config =
        dma_channel_get_default_config(s_uart_dma_channel);

    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_ring(&config, true, UART_DMA_RING_BITS);
    channel_config_set_dreq(
        &config,
        uart_get_dreq(data->uart.uart_hw, false));

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

/*
 * Core1 专门负责 USB/UART 数据搬运。
 * 这里不能调用任何 UI API，收到的数据通过 s_uart_term_queue 交给 Core0。
 */
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
        if (!s_uart_session_active || !data->io_enable) {
            data->core1_quiescent = true;
            tight_loop_contents();
            continue;
        }

        data->core1_quiescent = false;
        uint64_t now_us = time_us_64();

        /*
         * 先处理 UART DMA -> USB，保证 GP21 的接收不会被 GP20 发送阻塞。
         */
        uint16_t dma_head = (uint16_t)(
            ((uintptr_t)dma_hw->ch[s_uart_dma_channel].write_addr -
             (uintptr_t)s_uart_dma_ring) &
            (UART_DMA_RING_SIZE - 1));

        uint8_t usb_output[64];
        size_t output_length = 0;

        while (s_uart_dma_tail != dma_head) {
            uint8_t value = s_uart_dma_ring[s_uart_dma_tail];

            s_uart_dma_tail = (uint16_t)(
                (s_uart_dma_tail + 1) &
                (UART_DMA_RING_SIZE - 1));

            if (output_length < sizeof(usb_output)) {
                usb_output[output_length++] = value;
            }

            data->uart_to_usb_count++;

            if (rx_length < sizeof(rx_line) - 1) {
                rx_line[rx_length++] = value;
            }

            if (value == '\n' ||
                rx_length >= sizeof(rx_line) - 1) {
                uart_queue_push("[RX]",
                                rx_line,
                                rx_length,
                                0x07FF);
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

        /* USB CDC -> TX ring，不再在这里阻塞等待 UART FIFO。 */
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

                if (usb_buffer[i] == '\n' ||
                    tx_length >= sizeof(tx_line) - 1) {
                    uart_queue_push("[TX]",
                                    tx_line,
                                    tx_length,
                                    0x05E0);
                    tx_length = 0;
                }
            }

            tx_last_us = now_us;
        }

        /* 尽可能发送队列内容；FIFO 满时立即返回。 */
        uart_tx_ring_flush(data);

        if (tx_length > 0 &&
            now_us - tx_last_us > 25000) {
            uart_queue_push("[TX]",
                            tx_line,
                            tx_length,
                            0x05E0);
            tx_length = 0;
        }

        if (rx_length > 0 &&
            now_us - rx_last_us > 25000) {
            uart_queue_push("[RX]",
                            rx_line,
                            rx_length,
                            0x07FF);
            rx_length = 0;
        }
    }
}
/**
 * @brief 矩形安全合并助手（将两个脏矩形合为一个能包住它们的外接大矩形）
 */
static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (a.w == 0 || a.h == 0)
        return b;
    if (b.w == 0 || b.h == 0)
        return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

// Tab 切换回调
static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data)
{
    (void)old_tab;
    (void)new_tab;
    uart_data_t *data = (uart_data_t *)user_data;
    if (data)
    {
        data->dirty_rect = tv->content_rect;
    }
}

// Tab 0 内容占位：后续在这里添加串口状态信息。
static void on_status_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                   int16_t scroll_y, void *user_data)
{
    (void)tile;
    (void)viewport;
    (void)scroll_y;
    (void)user_data;
}

static bool on_uart_settings_touch_content(bool is_touching,
                                           int16_t content_x,
                                           int16_t content_y,
                                           uint32_t now_tick,
                                           void *user_data)
{
    (void)is_touching;
    (void)content_x;
    (void)content_y;
    (void)now_tick;
    uart_data_t *data = (uart_data_t *)user_data;
    if (!data)
        return false;

    return false;
}

static void on_uart_settings_draw_content(ui_tile_t *tile,
                                          const ui_rect_t *viewport,
                                          int16_t scroll_y,
                                          void *user_data)
{
    uart_data_t *data = (uart_data_t *)user_data;
    if (data == NULL) {
        return;
    }

    (void)tile;
    (void)viewport;
    (void)scroll_y;
    (void)data;
}

/* =========================================================================
 * 3. App 生命周期管理
 * ========================================================================= */

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

    // 首屏默认全量重绘一次
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = 0;
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_uart_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0 系统状态滚动视图，与偏好设置保持相同的视口规格。
    ui_scrollview_init(&data->status_scroll, 120, 40, 350, 260,
                       STATUS_CONTENT_H, on_status_draw_content);
    data->status_scroll.user_data = data;
    data->status_scroll.color_bg = 0xFFFF;
    data->status_scroll.color_scrollbar = 0xA514;
    data->status_scroll.show_scrollbar = true;

    // 4. Tab 1 控件排版：
    ui_scrollview_init(&data->setting_scroll, 120, 40, 350, 260,
                       SETTINGS_CONTENT_H, on_uart_settings_draw_content);
    data->setting_scroll.user_data = data;
    data->setting_scroll.color_bg = 0xFFFF;
    data->setting_scroll.color_scrollbar = 0xA514;
    data->setting_scroll.show_scrollbar = true;
    ui_scrollview_set_touch_cb(&data->setting_scroll,
                               on_uart_settings_touch_content);

    fun_uart_config_init(&data->uart_config);
    data->uart_config.hw_instance = FUN_UART_HW_UART1;
    data->uart_config.tx_gpio = 0;
    data->uart_config.rx_gpio = 1;
    data->uart_config.baudrate = 921600;
    data->uart_config.data_bits = 8;
    data->uart_config.stop_bits = 1;
    data->uart_config.parity = FUN_UART_PARITY_NONE;
    if (!fun_uart_open(&data->uart,
                       &data->uart_config))
    {
        data->uart_mode = UART_WORK_IDLE;
        data->io_enable = false;
        return;
    }
    uart_start_dma(data);

    data->uart_mode = UART_WORK_BRIDGE;
    data->io_enable = true;
    data->core1_quiescent = false;
    s_uart_session_active = true;

    /* Core1 可能被其他模块复位过，因此每次成功进入时重新启动。 */
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

        /* 先停止本 App 的 Core1 任务，再停止 DMA 和 UART。 */
        if (was_active) {
            multicore_reset_core1();
        }
        uart_stop_dma();
        fun_uart_close(&data->uart);
        data->core1_quiescent = true;
    }

    self->user_data = NULL;
}

/* =========================================================================
 * 4. 定时轮询与后台更新 (在主循环被高频调用)
 * ========================================================================= */

static void uart_update(app_t *self, uint32_t now_tick)
{
    (void)now_tick;
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data)
        return;

    // 关键：进入新一帧时无条件清零脏矩形
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    /* Core0 消费 Core1 产生的日志，不能在 Core1 中触碰 UI。 */
    while (s_uart_term_tail != s_uart_term_head) {
        uart_term_message_t *message =
            &s_uart_term_queue[s_uart_term_tail];

        memcpy(data->last_message,
               message->text,
               sizeof(data->last_message));
        data->last_message[sizeof(data->last_message) - 1] = '\0';
        data->last_message_color = message->color;

        s_uart_term_tail = (uint8_t)(
            (s_uart_term_tail + 1) % UART_TERM_QUEUE_SIZE);

        data->dirty_rect = rect_union_box(
            data->dirty_rect,
            data->status_scroll.view_rect);
    }

    if (data->usb_to_uart_count != data->last_usb_to_uart_count ||
        data->uart_to_usb_count != data->last_uart_to_usb_count) {
        data->last_usb_to_uart_count = data->usb_to_uart_count;
        data->last_uart_to_usb_count = data->uart_to_usb_count;
        data->dirty_rect = rect_union_box(
            data->dirty_rect,
            data->status_scroll.view_rect);
    }
}

/* =========================================================================
 * 5. 触控事件分发
 *
 * 【添加新控件 - 第 4 步】：按层级将触摸数据流分发给控件。
 * 顺序：全局控件 -> TabView 侧边栏 -> 当前 Tab 内部控件
 * ========================================================================= */

static bool uart_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data)
        return false;

    // 2. 侧边栏导航分发 (切页检测)
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick))
    {
        if (data->tabview.active_tab_idx != data->last_tab_idx)
        {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect = data->tabview.rect; // 切页整屏重绘
        }
        else
        {
            data->dirty_rect = ui_tabview_get_bar_dirty_rect(&data->tabview); // 仅指示条重绘
        }
        return true;
    }

    bool handled = false;

    // 3. 根据当前激活的 Tab 分层分流
    switch (data->tabview.active_tab_idx)
    {
    case 0: // --- Tab 0: 系统状态滚动视图 ---
        if (ui_scrollview_process_touch(&data->status_scroll, is_touching, tx, ty, now_tick))
        {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
            handled = true;
        }
        break;

    case 1: // --- Tab 1: 可滚动偏好设置 ---
        // ScrollView 统一完成坐标转换、拖动识别和子控件事件分发。
        if (ui_scrollview_process_touch(&data->setting_scroll, is_touching, tx, ty, now_tick))
        {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
            handled = true;
        }
        break;

    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* =========================================================================
 * 6. Tile 切片光栅化渲染
 *
 * 【添加新控件 - 第 5 步】：调用控件 draw API。
 * 顺序：底板 -> 全局控件 -> Tab 外壳 -> Early Exit 门禁 -> Tab 内部控件
 * ========================================================================= */

static void uart_draw(app_t *self, ui_tile_t *tile)
{
    uart_data_t *data = (uart_data_t *)self->user_data;
    if (!data)
        return;
    ui_rect_t overlap;

    // 1. 底板涂白
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap))
    {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    // 3. 绘制 TabView 外壳与侧边栏
    ui_tabview_draw(tile, &data->tabview);

    // 4. 性能门禁：若切片不在右侧内容工作区内，立即退出 (大幅降低 CPU 开销)
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap))
    {
        return;
    }

    switch (data->tabview.active_tab_idx)
    {
    case 0:
        ui_scrollview_draw(tile, &data->status_scroll);
        break;

    case 1:
        ui_scrollview_draw(tile, &data->setting_scroll);
        break;

    // case 2:
    //     ui_draw_string(tile,260,58,"PICO",FONT_24,0xC618,0xFFFF);
    //     ui_draw_string(tile,188,82,"多功能嵌入式工具",FONT_24,0xC618,0XFFFF);

    //     break;
    case 2:
    {
        const int16_t x = data->tabview.content_rect.x + 8;
        const int16_t y = data->tabview.content_rect.y + 28;

        break;
    }
    default:
        break;
    }
}

/* =========================================================================
 * 7. 导出单例实体 (注册到系统)
 * ========================================================================= */

app_t *app_uart_get_instance(void)
{
    s_uart.id = APP_ID_CUSTOM_0; // 注册到 app_manager 的枚举 ID
    s_uart.name = "串口工具";    // 桌面 App 图标名称
    s_uart.init = uarttool_init;
    s_uart.enter = uart_enter;
    s_uart.exit = uart_exit;
    s_uart.update = uart_update;
    s_uart.process_touch = uart_process_touch;
    s_uart.draw = uart_draw;
    return &s_uart;
}
