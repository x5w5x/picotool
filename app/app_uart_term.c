// #include "app_uart_term.h"
// #include "app_manager.h"
// #include "ui_draw.h"
// #include "ui_btn.h"
// #include "ui_switch.h"
// #include "ui_tabview.h"
// #include "lcd.h"
// #include "chinese.h"

// #include "pico/stdlib.h"
// #include "pico/multicore.h"
// // #include "pico/stdio_usb.h"
// #include "hardware/uart.h"
// #include "hardware/gpio.h"
// #include <stdio.h>

// #define UART_INST       uart0
// #define PIN_UART_TX     0   // GP0 -> 外部模块 RX
// #define PIN_UART_RX     1   // GP1 -> 外部模块 TX

// // 侧边栏 Tab 标签
// static const char *s_uart_tabs[] = {
//     "透传监视",
//     "波特配置",
//     "硬件回显"
// };
// #define TAB_COUNT (sizeof(s_uart_tabs) / sizeof(s_uart_tabs[0]))

// // 常用波特率预设表
// static const uint32_t s_baud_table[] = {9600, 115200, 460800, 921600, 2000000};
// #define BAUD_TABLE_SIZE (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

// // 运行模式枚举
// typedef enum {
//     UART_MODE_IDLE = 0,   // 0: 挂起待机
//     UART_MODE_BRIDGE,     // 1: USB ↔ GP0/GP1 双向透传
//     UART_MODE_ECHO        // 2: 硬件回显测试 (发收 1:1 对齐)
// } uart_work_mode_t;

// /* ================= 1. App 私有数据结构体 ================= */

// typedef struct {
//     ui_rect_t               dirty_rect;     // ⚠️ 必须放在第 1 位
//     ui_tabview_t            tabview;        // 侧边栏
//     uint8_t                 last_tab_idx;   // 上次 Tab 索引

//     // ⚡ 双核跨核共享状态 (volatile 保证 Core 1 实时感知)
//     volatile uart_work_mode_t work_mode;
//     volatile uint8_t        baud_idx;
//     volatile uint32_t       tx_bytes_cnt;   // USB 输入累计
//     volatile uint32_t       rx_bytes_cnt;   // USB 输出/回传累计

//     // UI 刷新比对缓存 (用于事件驱动刷新)
//     uint32_t                last_ui_tx;
//     uint32_t                last_ui_rx;
//     uint32_t                last_stat_tick;

//     // Tab 0: 透传监视
//     ui_switch_t             sw_bridge_en;
//     ui_btn_t                btn_clear_cnt;
//     ui_rect_t               stat_card;

//     // Tab 1: 波特率配置
//     ui_btn_t                btn_baud_next;
//     ui_rect_t               baud_card;

//     // Tab 2: 硬件回显
//     ui_switch_t             sw_echo_en;
//     ui_rect_t               echo_card;
// } app_uart_data_t;

// static app_uart_data_t s_uart_data;
// static app_t           s_app_uart;

// /* ================= 2. Core 1 业务引擎 (批量 FIFO 搬运) ================= */

// static void app_uart_core1_worker(void) {
//     app_uart_data_t *data = &s_uart_data;

//     while (1) {
//         switch (data->work_mode) {
//             case UART_MODE_BRIDGE: {
//                 // 通道 A: USB CDC -> UART0 (GP0 TX) ⚡ 使用 while 批量读空 USB FIFO
//                 int ch;
//                 while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
//                     while (!uart_is_writable(UART_INST)) {
//                         tight_loop_contents();
//                     }
//                     uart_putc_raw(UART_INST, (char)ch);
//                     data->tx_bytes_cnt++;
//                 }

//                 // 通道 B: UART0 (GP1 RX) -> USB CDC ⚡ 批量送出硬件 FIFO
//                 while (uart_is_readable(UART_INST)) {
//                     char rx_ch = uart_getc(UART_INST);
//                     putchar_raw(rx_ch);
//                     data->rx_bytes_cnt++;
//                 }
//                 break;
//             }

//             case UART_MODE_ECHO: {
//                 // 回显模式: 收到 USB 字符，原样打回 USB 并送给 GP0
//                 int ch;
//                 while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
//                     putchar_raw((char)ch); // 原路回传给 PC
//                     if (uart_is_writable(UART_INST)) {
//                         uart_putc_raw(UART_INST, (char)ch);
//                     }
//                     // ⚡ 严格 1:1 对齐：接收算 TX，回送算 RX
//                     data->tx_bytes_cnt++;
//                     data->rx_bytes_cnt++;
//                 }
//                 break;
//             }

//             case UART_MODE_IDLE:
//             default:
//                 tight_loop_contents();
//                 break;
//         }
//     }
// }

// /* ================= 3. 交互回调 (含开关互斥联动) ================= */

// static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
//     (void)tv; (void)old_tab; (void)new_tab; (void)user_data;
// }

// // ⚡ Tab 0: 透传开关 (互斥关闭回显)
// static void on_bridge_switch(ui_switch_t *sw, bool state, void *user_data) {
//     app_uart_data_t *data = (app_uart_data_t *)user_data;
//     if (state) {
//         data->work_mode = UART_MODE_BRIDGE;
//         // 联动把回显开关关掉
//         ui_switch_set_state(&data->sw_echo_en, false);
//     } else {
//         if (data->work_mode == UART_MODE_BRIDGE) {
//             data->work_mode = UART_MODE_IDLE;
//         }
//     }
// }

// // ⚡ Tab 2: 回显开关 (互斥关闭透传)
// static void on_echo_switch(ui_switch_t *sw, bool state, void *user_data) {
//     app_uart_data_t *data = (app_uart_data_t *)user_data;
//     if (state) {
//         data->work_mode = UART_MODE_ECHO;
//         // 联动把透传开关关掉
//         ui_switch_set_state(&data->sw_bridge_en, false);
//     } else {
//         if (data->work_mode == UART_MODE_ECHO) {
//             data->work_mode = UART_MODE_IDLE;
//         }
//     }
// }

// // 清空计数
// static void on_clear_cnt_click(ui_btn_t *btn, void *user_data) {
//     (void)btn;
//     app_uart_data_t *data = (app_uart_data_t *)user_data;
//     data->tx_bytes_cnt = 0;
//     data->rx_bytes_cnt = 0;
//     data->last_ui_tx   = 0;
//     data->last_ui_rx   = 0;
//     data->dirty_rect   = data->stat_card;
// }

// // 轮切波特率
// static void on_baud_next_click(ui_btn_t *btn, void *user_data) {
//     (void)btn;
//     app_uart_data_t *data = (app_uart_data_t *)user_data;
//     data->baud_idx = (data->baud_idx + 1) % BAUD_TABLE_SIZE;
//     uart_set_baudrate(UART_INST, s_baud_table[data->baud_idx]);
// }

// /* ================= 4. 生命周期管理 ================= */

// static void app_uart_init(app_t *self) {
//     self->user_data = &s_uart_data;
//     app_uart_data_t *data = &s_uart_data;

//     data->dirty_rect      = ui_rect_make(0, 0, 0, 0);
//     data->work_mode       = UART_MODE_BRIDGE;
//     data->baud_idx        = 1; // 默认 115200
//     data->tx_bytes_cnt    = 0;
//     data->rx_bytes_cnt    = 0;
//     data->last_ui_tx      = 0;
//     data->last_ui_rx      = 0;
//     data->last_stat_tick  = 0;
//     data->last_tab_idx    = 0;

//     // 1. 初始化 TabView (侧边栏宽 110px)
//     ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
//                     110, 48, s_uart_tabs, TAB_COUNT, 0, on_tab_changed);
//     data->tabview.user_data = data;

//     // 2. Tab 0: 监视页控件 (默认开)
//     data->stat_card = ui_rect_make(125, 48, 340, 100);
//     ui_switch_init(&data->sw_bridge_en, 385, 168, 64, 32, true, on_bridge_switch);
//     data->sw_bridge_en.user_data = data;

//     ui_btn_init(&data->btn_clear_cnt, 175, 225, 240, 42, "清空吞吐统计", on_clear_cnt_click);
//     data->btn_clear_cnt.user_data = data;
//     data->btn_clear_cnt.style     = UI_BTN_STYLE_ROUND;
//     data->btn_clear_cnt.font_size = FONT_20;
//     ui_btn_set_color(&data->btn_clear_cnt, 0xBF5E, 0x03DF, 0x1A7B);

//     // 3. Tab 1: 波特率配置
//     data->baud_card = ui_rect_make(125, 48, 340, 90);
//     ui_btn_init(&data->btn_baud_next, 175, 170, 240, 44, "切换下一档波特率", on_baud_next_click);
//     data->btn_baud_next.user_data = data;
//     data->btn_baud_next.style     = UI_BTN_STYLE_ROUND;
//     data->btn_baud_next.font_size = FONT_20;
//     ui_btn_set_color(&data->btn_baud_next, 0xBF5E, 0x03DF, 0x1A7B);

//     // 4. Tab 2: 回显页控件 (默认关)
//     data->echo_card = ui_rect_make(125, 48, 340, 100);
//     ui_switch_init(&data->sw_echo_en, 385, 175, 64, 32, false, on_echo_switch);
//     data->sw_echo_en.user_data = data;
// }

// static void app_uart_enter(app_t *self) {
//     app_uart_data_t *data = (app_uart_data_t *)self->user_data;

//     // ⚡ 禁用 stdio 自动 CR/LF 转换，保证数据原汁原味 1:1 透传
//     #if PICO_STDIO_USB
//     stdio_set_translate_crlf(&stdio_usb, false);
//     #endif

//     // 1. 初始化 UART 硬件
//     uart_init(UART_INST, s_baud_table[data->baud_idx]);
//     gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
//     gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);

//     uart_set_hw_flow(UART_INST, false, false);
//     uart_set_format(UART_INST, 8, 1, UART_PARITY_NONE);
//     uart_set_fifo_enabled(UART_INST, true);

//     // 2. ⚡ 点火 Core 1
//     multicore_reset_core1();
//     multicore_launch_core1(app_uart_core1_worker);
// }

// static void app_uart_exit(app_t *self) {
//     (void)self;

//     // ⚡ 退出时复位 Core 1 并释放 UART 外设
//     multicore_reset_core1();

//     uart_deinit(UART_INST);
//     gpio_set_function(PIN_UART_TX, GPIO_FUNC_NULL);
//     gpio_set_function(PIN_UART_RX, GPIO_FUNC_NULL);
// }

// /* ================= 5. 事件驱动 + 50ms 节流快速刷新 ================= */

// static void app_uart_update(app_t *self, uint32_t now_tick) {
//     app_uart_data_t *data = (app_uart_data_t *)self->user_data;

//     // ⚡ 数据发生变动时，每隔 50ms 极速推流刷新，避免人眼感知延迟
//     bool data_changed = (data->tx_bytes_cnt != data->last_ui_tx) || 
//                         (data->rx_bytes_cnt != data->last_ui_rx);

//     if (data_changed && (now_tick - data->last_stat_tick >= 50)) {
//         data->last_stat_tick = now_tick;
//         data->last_ui_tx     = data->tx_bytes_cnt;
//         data->last_ui_rx     = data->rx_bytes_cnt;

//         if (data->tabview.active_tab_idx == 0) {
//             data->dirty_rect = data->stat_card;
//         } else if (data->tabview.active_tab_idx == 2) {
//             data->dirty_rect = data->echo_card;
//         }
//     }
// }

// /* ================= 6. 触控分发 ================= */

// static bool app_uart_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
//     app_uart_data_t *data = (app_uart_data_t *)self->user_data;

//     // 1. 侧边栏触控
//     if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
//         if (data->tabview.active_tab_idx != data->last_tab_idx) {
//             data->last_tab_idx = data->tabview.active_tab_idx;
//             data->dirty_rect   = data->tabview.rect; // 全工作区重绘
//         } else {
//             data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
//         }
//         return true;
//     }

//     // 2. 当前 Tab 控件事件
//     switch (data->tabview.active_tab_idx) {
//         case 0: { // 监视页
//             bool sw_act  = ui_switch_process_touch(&data->sw_bridge_en, is_touching, tx, ty, now_tick);
//             bool clr_act = ui_btn_process_touch(&data->btn_clear_cnt, is_touching, tx, ty, now_tick);

//             if (sw_act || clr_act) {
//                 ui_rect_t w_dirty = sw_act ? ui_switch_get_dirty_rect(&data->sw_bridge_en) 
//                                            : ui_btn_get_dirty_rect(&data->btn_clear_cnt);
//                 ui_rect_union(&w_dirty, &data->stat_card, &data->dirty_rect);
//                 return true;
//             }
//             break;
//         }

//         case 1: { // 波特率页
//             if (ui_btn_process_touch(&data->btn_baud_next, is_touching, tx, ty, now_tick)) {
//                 ui_rect_t b_dirty = ui_btn_get_dirty_rect(&data->btn_baud_next);
//                 ui_rect_union(&b_dirty, &data->baud_card, &data->dirty_rect);
//                 return true;
//             }
//             break;
//         }

//         case 2: { // 回显页
//             if (ui_switch_process_touch(&data->sw_echo_en, is_touching, tx, ty, now_tick)) {
//                 ui_rect_t s_dirty = ui_switch_get_dirty_rect(&data->sw_echo_en);
//                 ui_rect_union(&s_dirty, &data->echo_card, &data->dirty_rect);
//                 return true;
//             }
//             break;
//         }
//     }

//     return false;
// }

// /* ================= 7. 切片光栅化渲染 ================= */

// static void app_uart_draw(app_t *self, ui_tile_t *tile) {
//     app_uart_data_t *data = (app_uart_data_t *)self->user_data;
//     ui_rect_t overlap;

//     // 1. 绘制白底背景
//     ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//     if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
//         ui_draw_fill_rect(tile, &overlap, 0xFFFF);
//     }

//     // 2. 绘制 TabView 侧边栏
//     ui_tabview_draw(tile, &data->tabview);

//     // 3. 右侧内容区切片测试
//     if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
//         return;
//     }

//     char str_buf[64];

//     // 4. 各 Tab 页面绘制
//     switch (data->tabview.active_tab_idx) {
//         case 0: { // ===== Tab 0: 监视页 =====
//             if (ui_rect_intersect(&tile->rect, &data->stat_card, &overlap)) {
//                 ui_draw_fill_rect(tile, &overlap, 0xEF7D);
//                 ui_draw_rect(tile, &data->stat_card, 0xC618);

//                 ui_draw_string(tile, 140, 56, "双核高频透传引擎 (Core 1)", FONT_16, 0x1A7B, 0xEF7D);

//                 snprintf(str_buf, sizeof(str_buf), "TX (USB->GP0): %lu Bytes", data->tx_bytes_cnt);
//                 ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);

//                 snprintf(str_buf, sizeof(str_buf), "RX (GP1->USB): %lu Bytes", data->rx_bytes_cnt);
//                 ui_draw_string(tile, 140, 104, str_buf, FONT_16, 0x03DF, 0xEF7D);

//                 uint16_t dot_color = (data->work_mode == UART_MODE_BRIDGE) ? 0x05E0 : 0x8410;
//                 ui_draw_fill_circle(tile, 445, 64, 6, dot_color);
//             }

//             int16_t lbl_y = 174;
//             if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
//                 ui_draw_string(tile, 130, lbl_y, "透传引擎使能:", FONT_20, 0x1A7B, 0xFFFF);
//             }

//             ui_switch_draw(tile, &data->sw_bridge_en);
//             ui_btn_draw(tile, &data->btn_clear_cnt);
//             break;
//         }

//         case 1: { // ===== Tab 1: 波特率配置 =====
//             if (ui_rect_intersect(&tile->rect, &data->baud_card, &overlap)) {
//                 ui_draw_fill_rect(tile, &overlap, 0xEF7D);
//                 ui_draw_rect(tile, &data->baud_card, 0xC618);

//                 ui_draw_string(tile, 140, 58, "当前通信波特率", FONT_16, 0x1A7B, 0xEF7D);

//                 snprintf(str_buf, sizeof(str_buf), "%lu bps (8-N-1)", s_baud_table[data->baud_idx]);
//                 ui_draw_string(tile, 140, 86, str_buf, FONT_24, 0x03DF, 0xEF7D);
//             }

//             int16_t tip_y = 230;
//             if ((tip_y + 20) > tile->rect.y && tip_y < (tile->rect.y + tile->rect.h)) {
//                 ui_draw_string(tile, 140, tip_y, "支持 9600 ~ 2Mbps 极速切换", FONT_16, 0x4208, 0xFFFF);
//             }

//             ui_btn_draw(tile, &data->btn_baud_next);
//             break;
//         }

//         case 2: { // ===== Tab 2: 硬件回显 =====
//             if (ui_rect_intersect(&tile->rect, &data->echo_card, &overlap)) {
//                 ui_draw_fill_rect(tile, &overlap, 0xEF7D);
//                 ui_draw_rect(tile, &data->echo_card, 0xC618);

//                 ui_draw_string(tile, 140, 56, "硬件自测回显模式", FONT_16, 0x1A7B, 0xEF7D);
//                 snprintf(str_buf, sizeof(str_buf), "回显吞吐量: %lu Bytes (1:1)", data->tx_bytes_cnt);
//                 ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);
//                 ui_draw_string(tile, 140, 104, "USB 输入自动原路回传并打给 GP0", FONT_16, 0x4208, 0xEF7D);

//                 uint16_t dot_color = (data->work_mode == UART_MODE_ECHO) ? 0x05E0 : 0x8410;
//                 ui_draw_fill_circle(tile, 445, 64, 6, dot_color);
//             }

//             int16_t lbl_y = 180;
//             if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
//                 ui_draw_string(tile, 130, lbl_y, "开启自发自收:", FONT_20, 0x1A7B, 0xFFFF);
//             }

//             ui_switch_draw(tile, &data->sw_echo_en);
//             break;
//         }
//     }
// }

// /* ================= 8. 导出单例实体 ================= */

// app_t *app_uart_term_get_instance(void) {
//     s_app_uart.id            = APP_ID_UART_TERM;
//     s_app_uart.name          = "串口助手";
//     s_app_uart.init          = app_uart_init;
//     s_app_uart.enter         = app_uart_enter;
//     s_app_uart.exit          = app_uart_exit;
//     s_app_uart.update        = app_uart_update;
//     s_app_uart.process_touch = app_uart_process_touch;
//     s_app_uart.draw          = app_uart_draw;
//     return &s_app_uart;
// }
#include "app_uart_term.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/structs/dma.h"
#include <stdio.h>

#define UART_INST           uart0
#define PIN_UART_TX         0   // GP0 (TX) -> 接 ML307R RXD
#define PIN_UART_RX         1   // GP1 (RX) -> 接 ML307R TXD

// ⚡ 2048 字节硬件 DMA 环形接收缓冲区 (按 2048 字节硬件严格对齐)
#define RX_DMA_RING_SIZE    2048
static uint8_t              s_rx_dma_ring[RX_DMA_RING_SIZE] __attribute__((aligned(RX_DMA_RING_SIZE)));
static volatile uint16_t    s_rx_tail = 0;
static int                  s_dma_rx_chan = -1;

// 侧边栏 Tab 标签
static const char *s_uart_tabs[] = {
    "透传监视",
    "波特配置",
    "硬件回显"
};
#define TAB_COUNT (sizeof(s_uart_tabs) / sizeof(s_uart_tabs[0]))

// 常用波特率预设表 (ML307R 默认为 115200)
static const uint32_t s_baud_table[] = {9600, 115200, 460800, 921600, 2000000};
#define BAUD_TABLE_SIZE (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

// 运行模式枚举
typedef enum {
    UART_MODE_IDLE = 0,   // 0: 挂起待机
    UART_MODE_BRIDGE,     // 1: USB ↔ GP0/GP1 双向透传
    UART_MODE_ECHO        // 2: 硬件回显测试
} uart_work_mode_t;

/* ================= 1. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t               dirty_rect;     // ⚠️ 必须放在第 1 位
    ui_tabview_t            tabview;        // 侧边栏
    uint8_t                 last_tab_idx;   // 上次 Tab 索引

    // ⚡ 双核跨核共享状态
    volatile uart_work_mode_t work_mode;
    volatile uint8_t        baud_idx;
    volatile uint32_t       tx_bytes_cnt;   // USB 输入累计
    volatile uint32_t       rx_bytes_cnt;   // USB 输出/回传累计

    // UI 刷新比对缓存
    uint32_t                last_ui_tx;
    uint32_t                last_ui_rx;
    uint32_t                last_stat_tick;

    // Tab 0: 透传监视
    ui_switch_t             sw_bridge_en;
    ui_btn_t                btn_clear_cnt;
    ui_rect_t               stat_card;

    // Tab 1: 波特率配置
    ui_btn_t                btn_baud_next;
    ui_rect_t               baud_card;

    // Tab 2: 硬件回显
    ui_switch_t             sw_echo_en;
    ui_rect_t               echo_card;
} app_uart_data_t;

static app_uart_data_t s_uart_data;
static app_t           s_app_uart;

/* ================= 2. Core 1 专核业务主线程 (DMA 零阻塞搬运) ================= */

static void app_uart_core1_worker(void) {
    app_uart_data_t *data = &s_uart_data;

    while (1) {
        switch (data->work_mode) {
            case UART_MODE_BRIDGE: {
                // ================= 通道 A: PC (USB CDC) -> ML307R (GP0 TX) =================
                int usb_ch;
                while ((usb_ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
                    while (!uart_is_writable(UART_INST)) {
                        tight_loop_contents();
                    }
                    uart_putc_raw(UART_INST, (char)usb_ch);
                    data->tx_bytes_cnt++;
                }

                // ================= 通道 B: 硬件 DMA 环形队列 -> PC (USB CDC) =================
                // 计算当前 DMA 硬件写入游标 head
                uint16_t head = (uint16_t)((uintptr_t)dma_hw->ch[s_dma_rx_chan].write_addr - (uintptr_t)s_rx_dma_ring) % RX_DMA_RING_SIZE;
                bool has_rx = false;

                while (s_rx_tail != head) {
                    putchar_raw((char)s_rx_dma_ring[s_rx_tail]);
                    s_rx_tail = (s_rx_tail + 1) % RX_DMA_RING_SIZE;
                    data->rx_bytes_cnt++;
                    has_rx = true;
                }

                if (has_rx) {
                    stdio_flush(); // 强制刷出 USB 发送缓冲
                }
                break;
            }

            case UART_MODE_ECHO: {
                int ch;
                bool has_echo = false;
                while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
                    putchar_raw((char)ch);
                    if (uart_is_writable(UART_INST)) {
                        uart_putc_raw(UART_INST, (char)ch);
                    }
                    data->tx_bytes_cnt++;
                    data->rx_bytes_cnt++;
                    has_echo = true;
                }
                if (has_echo) {
                    stdio_flush();
                }
                break;
            }

            case UART_MODE_IDLE:
            default:
                tight_loop_contents();
                break;
        }
    }
}

/* ================= 3. 交互回调 (含开关互斥联动) ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab; (void)new_tab; (void)user_data;
}

static void on_bridge_switch(ui_switch_t *sw, bool state, void *user_data) {
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (state) {
        data->work_mode = UART_MODE_BRIDGE;
        ui_switch_set_state(&data->sw_echo_en, false);
    } else {
        if (data->work_mode == UART_MODE_BRIDGE) {
            data->work_mode = UART_MODE_IDLE;
        }
    }
}

static void on_echo_switch(ui_switch_t *sw, bool state, void *user_data) {
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (state) {
        data->work_mode = UART_MODE_ECHO;
        ui_switch_set_state(&data->sw_bridge_en, false);
    } else {
        if (data->work_mode == UART_MODE_ECHO) {
            data->work_mode = UART_MODE_IDLE;
        }
    }
}

static void on_clear_cnt_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    data->tx_bytes_cnt = 0;
    data->rx_bytes_cnt = 0;
    data->last_ui_tx   = 0;
    data->last_ui_rx   = 0;
    data->dirty_rect   = data->stat_card;
}

static void on_baud_next_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    data->baud_idx = (data->baud_idx + 1) % BAUD_TABLE_SIZE;
    uart_set_baudrate(UART_INST, s_baud_table[data->baud_idx]);
}

/* ================= 4. 生命周期管理与 DMA 硬件启动 ================= */

static void app_uart_init(app_t *self) {
    self->user_data = &s_uart_data;
    app_uart_data_t *data = &s_uart_data;

    data->dirty_rect      = ui_rect_make(0, 0, 0, 0);
    data->work_mode       = UART_MODE_BRIDGE;
    data->baud_idx        = 1; // 默认 115200 (匹配 ML307R)
    data->tx_bytes_cnt    = 0;
    data->rx_bytes_cnt    = 0;
    data->last_ui_tx      = 0;
    data->last_ui_rx      = 0;
    data->last_stat_tick  = 0;
    data->last_tab_idx    = 0;

    // 1. 初始化 TabView
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_uart_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // 2. Tab 0: 监视页控件 (默认开)
    data->stat_card = ui_rect_make(125, 48, 340, 100);
    ui_switch_init(&data->sw_bridge_en, 385, 168, 64, 32, true, on_bridge_switch);
    data->sw_bridge_en.user_data = data;

    ui_btn_init(&data->btn_clear_cnt, 175, 225, 240, 42, "清空吞吐统计", on_clear_cnt_click);
    data->btn_clear_cnt.user_data = data;
    data->btn_clear_cnt.style     = UI_BTN_STYLE_ROUND;
    data->btn_clear_cnt.font_size = FONT_20;
    ui_btn_set_color(&data->btn_clear_cnt, 0xBF5E, 0x03DF, 0x1A7B);

    // 3. Tab 1: 波特率配置
    data->baud_card = ui_rect_make(125, 48, 340, 90);
    ui_btn_init(&data->btn_baud_next, 175, 170, 240, 44, "切换下一档波特率", on_baud_next_click);
    data->btn_baud_next.user_data = data;
    data->btn_baud_next.style     = UI_BTN_STYLE_ROUND;
    data->btn_baud_next.font_size = FONT_20;
    ui_btn_set_color(&data->btn_baud_next, 0xBF5E, 0x03DF, 0x1A7B);

    // 4. Tab 2: 回显页控件 (默认关)
    data->echo_card = ui_rect_make(125, 48, 340, 100);
    ui_switch_init(&data->sw_echo_en, 385, 175, 64, 32, false, on_echo_switch);
    data->sw_echo_en.user_data = data;
}

static void app_uart_enter(app_t *self) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    // ⚡ 禁用 stdio 自动换行转义
    stdio_set_translate_crlf(&stdio_usb, false);

    // 1. 初始化 UART 硬件
    uart_init(UART_INST, s_baud_table[data->baud_idx]);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_UART_RX);

    uart_set_hw_flow(UART_INST, false, false);
    uart_set_format(UART_INST, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_INST, true);

    // 清空硬件 FIFO
    while (uart_is_readable(UART_INST)) {
        (void)uart_getc(UART_INST);
    }

    // 2. ⚡ 配置并启动硬件 DMA 接收通道 (2048 字节硬件自动环绕)
    s_rx_tail = 0;
    if (s_dma_rx_chan < 0) {
        s_dma_rx_chan = dma_claim_unused_channel(true);
    }

    dma_channel_config c = dma_channel_get_default_config(s_dma_rx_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);                                   // 固定读 UART0 DR
    channel_config_set_write_increment(&c, true);                                  // 写入递增
    channel_config_set_ring(&c, true, 11);                                         // 2^11 = 2048 字节硬件自动环绕
    channel_config_set_dreq(&c, uart_get_dreq(UART_INST, false));                  // UART0 RX 请求信号

    dma_channel_configure(
        s_dma_rx_chan,
        &c,
        s_rx_dma_ring,                                                             // 目标缓冲区
        &uart_get_hw(UART_INST)->dr,                                               // 数据源
        0xFFFFFFFF,                                                                // 持续运行
        true                                                                       // 立即使能
    );

    // 3. ⚡ 点火 Core 1 专核
    multicore_reset_core1();
    multicore_launch_core1(app_uart_core1_worker);
}

static void app_uart_exit(app_t *self) {
    (void)self;

    // 1. 复位 Core 1
    multicore_reset_core1();

    // 2. 释放 DMA 通道
    if (s_dma_rx_chan >= 0) {
        dma_channel_abort(s_dma_rx_chan);
        dma_channel_unclaim(s_dma_rx_chan);
        s_dma_rx_chan = -1;
    }

    // 3. 释放 UART 外设与引脚
    uart_deinit(UART_INST);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_NULL);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_NULL);
}

/* ================= 5. 事件驱动 + 50ms 节流快速刷新 ================= */

static void app_uart_update(app_t *self, uint32_t now_tick) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    bool data_changed = (data->tx_bytes_cnt != data->last_ui_tx) || 
                        (data->rx_bytes_cnt != data->last_ui_rx);

    if (data_changed && (now_tick - data->last_stat_tick >= 50)) {
        data->last_stat_tick = now_tick;
        data->last_ui_tx     = data->tx_bytes_cnt;
        data->last_ui_rx     = data->rx_bytes_cnt;

        if (data->tabview.active_tab_idx == 0) {
            data->dirty_rect = data->stat_card;
        } else if (data->tabview.active_tab_idx == 2) {
            data->dirty_rect = data->echo_card;
        }
    }
}

/* ================= 6. 触控分发 ================= */

static bool app_uart_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    // 1. 侧边栏触控
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect; // 全工作区重绘
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    // 2. 当前 Tab 控件事件
    switch (data->tabview.active_tab_idx) {
        case 0: { // 监视页
            bool sw_act  = ui_switch_process_touch(&data->sw_bridge_en, is_touching, tx, ty, now_tick);
            bool clr_act = ui_btn_process_touch(&data->btn_clear_cnt, is_touching, tx, ty, now_tick);

            if (sw_act || clr_act) {
                ui_rect_t w_dirty = sw_act ? ui_switch_get_dirty_rect(&data->sw_bridge_en) 
                                           : ui_btn_get_dirty_rect(&data->btn_clear_cnt);
                ui_rect_union(&w_dirty, &data->stat_card, &data->dirty_rect);
                return true;
            }
            break;
        }

        case 1: { // 波特率页
            if (ui_btn_process_touch(&data->btn_baud_next, is_touching, tx, ty, now_tick)) {
                ui_rect_t b_dirty = ui_btn_get_dirty_rect(&data->btn_baud_next);
                ui_rect_union(&b_dirty, &data->baud_card, &data->dirty_rect);
                return true;
            }
            break;
        }

        case 2: { // 回显页
            if (ui_switch_process_touch(&data->sw_echo_en, is_touching, tx, ty, now_tick)) {
                ui_rect_t s_dirty = ui_switch_get_dirty_rect(&data->sw_echo_en);
                ui_rect_union(&s_dirty, &data->echo_card, &data->dirty_rect);
                return true;
            }
            break;
        }
    }

    return false;
}

/* ================= 7. 切片光栅化渲染 ================= */

static void app_uart_draw(app_t *self, ui_tile_t *tile) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;
    ui_rect_t overlap;

    // 1. 绘制白底背景
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    // 2. 绘制 TabView 侧边栏
    ui_tabview_draw(tile, &data->tabview);

    // 3. 右侧内容区切片测试
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case 0: { // 监视页
            if (ui_rect_intersect(&tile->rect, &data->stat_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->stat_card, 0xC618);

                ui_draw_string(tile, 140, 56, "双核高频透传 (DMA 硬件零丢包)", FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "TX (USB->GP0): %lu Bytes", data->tx_bytes_cnt);
                ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "RX (GP1->USB): %lu Bytes", data->rx_bytes_cnt);
                ui_draw_string(tile, 140, 104, str_buf, FONT_16, 0x03DF, 0xEF7D);

                uint16_t dot_color = (data->work_mode == UART_MODE_BRIDGE) ? 0x05E0 : 0x8410;
                ui_draw_fill_circle(tile, 445, 64, 6, dot_color);
            }

            int16_t lbl_y = 174;
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 130, lbl_y, "透传引擎使能:", FONT_20, 0x1A7B, 0xFFFF);
            }

            ui_switch_draw(tile, &data->sw_bridge_en);
            ui_btn_draw(tile, &data->btn_clear_cnt);
            break;
        }

        case 1: { // 波特率页
            if (ui_rect_intersect(&tile->rect, &data->baud_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->baud_card, 0xC618);

                ui_draw_string(tile, 140, 58, "当前通信波特率", FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "%lu bps (8-N-1)", s_baud_table[data->baud_idx]);
                ui_draw_string(tile, 140, 86, str_buf, FONT_24, 0x03DF, 0xEF7D);
            }

            int16_t tip_y = 230;
            if ((tip_y + 20) > tile->rect.y && tip_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 140, tip_y, "支持 9600 ~ 2Mbps 极速切换", FONT_16, 0x4208, 0xFFFF);
            }

            ui_btn_draw(tile, &data->btn_baud_next);
            break;
        }

        case 2: { // 回显页
            if (ui_rect_intersect(&tile->rect, &data->echo_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->echo_card, 0xC618);

                ui_draw_string(tile, 140, 56, "硬件自测回显模式", FONT_16, 0x1A7B, 0xEF7D);
                snprintf(str_buf, sizeof(str_buf), "回显吞吐量: %lu Bytes (1:1)", data->tx_bytes_cnt);
                ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);
                ui_draw_string(tile, 140, 104, "USB 输入自动原路回传并打给 GP0", FONT_16, 0x4208, 0xEF7D);

                uint16_t dot_color = (data->work_mode == UART_MODE_ECHO) ? 0x05E0 : 0x8410;
                ui_draw_fill_circle(tile, 445, 64, 6, dot_color);
            }

            int16_t lbl_y = 180;
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 130, lbl_y, "开启自发自收:", FONT_20, 0x1A7B, 0xFFFF);
            }

            ui_switch_draw(tile, &data->sw_echo_en);
            break;
        }
    }
}

/* ================= 8. 导出单例实体 ================= */

app_t *app_uart_term_get_instance(void) {
    s_app_uart.id            = APP_ID_UART_TERM;
    s_app_uart.name          = "串口助手";
    s_app_uart.init          = app_uart_init;
    s_app_uart.enter         = app_uart_enter;
    s_app_uart.exit          = app_uart_exit;
    s_app_uart.update        = app_uart_update;
    s_app_uart.process_touch = app_uart_process_touch;
    s_app_uart.draw          = app_uart_draw;
    return &s_app_uart;
}