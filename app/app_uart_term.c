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
// }#include "app_uart_term.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "ui_terminal.h"
#include "ui_dropdown.h"
#include "ui_input.h"
#include "ui_keyboard.h"
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
#include <string.h>
#include <stdlib.h>

#define UART_INST           uart0
#define PIN_UART_TX         0   // GP0 (TX) -> 接外部模组 RX//e: 15]
#define PIN_UART_RX         1   // GP1 (RX) -> 接外部模组 TX//e: 15]

// ⚡ 2048 字节硬件 DMA 环形接收缓冲区//e: 15]
#define RX_DMA_RING_SIZE    2048
static uint8_t              s_rx_dma_ring[RX_DMA_RING_SIZE] __attribute__((aligned(RX_DMA_RING_SIZE)));
static volatile uint16_t    s_rx_tail = 0;
static int                  s_dma_rx_chan = -1;

// ⚡ 跨核终端通信安全队列 (Core 1 写入 -> Core 0 消费)
#define TERM_QUEUE_SIZE     16
typedef struct {
    char     text[UI_TERM_LINE_LEN];//e: 20]
    uint16_t color;
} term_msg_t;

static term_msg_t           s_term_queue[TERM_QUEUE_SIZE];
static volatile uint8_t     s_tq_head = 0;
static volatile uint8_t     s_tq_tail = 0;

// 侧边栏 Tab 标签 (4 个独立功能页)
static const char *s_uart_tabs[] = {
    "透传监视",
    "数据终端",
    "波特配置",
    "硬件回显"
};
#define TAB_COUNT (sizeof(s_uart_tabs) / sizeof(s_uart_tabs[0]))

// 下拉菜单预设选项//e: 17]
static const char *s_dest_options[] = {"目标:GP0", "目标:USB", "目标:双发"};
#define DEST_OPTION_COUNT 3

static const char *s_mode_options[] = {"ASCII", "HEX"};
#define MODE_OPTION_COUNT 2

// 常用波特率预设表//e: 15]
static const uint32_t s_baud_table[] = {9600, 115200, 460800, 921600, 2000000};//e: 15]
#define BAUD_TABLE_SIZE (sizeof(s_baud_table) / sizeof(s_baud_table[0]))//e: 15]

typedef enum {
    UART_MODE_IDLE = 0,   // 0: 挂起待机//e: 15]
    UART_MODE_BRIDGE,     // 1: USB ↔ GP0/GP1 双向透传//e: 15]
    UART_MODE_ECHO        // 2: 硬件回显测试//e: 15]
} uart_work_mode_t;

/* ================= 1. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t               dirty_rect;     // ⚠️ 必须放在第 1 位//e: 15]
    ui_tabview_t            tabview;        // 侧边栏//e: 15]
    uint8_t                 last_tab_idx;   // 上次 Tab 索引//e: 15]

    // ⚡ 双核跨核共享状态//e: 15]
    volatile uart_work_mode_t work_mode;//e: 15]
    volatile uint8_t        baud_idx;//e: 15]
    volatile uint32_t       tx_bytes_cnt;   // USB/屏幕输入累计//e: 15]
    volatile uint32_t       rx_bytes_cnt;   // USB 输出/回传累计//e: 15]

    // UI 刷新比对缓存//e: 15]
    uint32_t                last_ui_tx;//e: 15]
    uint32_t                last_ui_rx;//e: 15]
    uint32_t                last_stat_tick;//e: 15]

    // Tab 0: 经典透传监视
    ui_switch_t             sw_bridge_en;//e: 15]
    ui_btn_t                btn_clear_cnt;//e: 15]
    ui_rect_t               stat_card;//e: 15]

    // Tab 1: 全功能交互数据终端
    ui_terminal_t           terminal;       // 滚动控制台终端//e: 20]
    ui_dropdown_t           dropdown_dest;  // 输出目标下拉//e: 17]
    ui_dropdown_t           dropdown_mode;  // 格式模式下拉//e: 17]
    ui_btn_t                btn_clear_term; // 清屏按钮
    ui_btn_t                btn_quick_at;   // 快捷 AT
    ui_input_t              input_cmd;      // 指令输入框//e: 18]
    ui_btn_t                btn_send_cmd;   // 发送按钮
    ui_keyboard_t           kb;             // 浮动 QWERTY 软键盘//e: 19]

    // Tab 2: 波特率配置
    ui_btn_t                btn_baud_next;//e: 15]
    ui_rect_t               baud_card;//e: 15]

    // Tab 3: 硬件回显
    ui_switch_t             sw_echo_en;//e: 15]
    ui_rect_t               echo_card;//e: 15]
} app_uart_data_t;

static app_uart_data_t s_uart_data;
static app_t           s_app_uart;

// 矩形安全合并助手
static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

// 跨核推入待显示报文
static void queue_push_line(const char *tag, const uint8_t *buf, size_t len, uint16_t color) {
    if (len == 0) return;

    bool has_printable = false;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] >= 32 && buf[i] <= 126) {
            has_printable = true;
            break;
        }
    }
    if (!has_printable) return;

    uint8_t next_head = (s_tq_head + 1) % TERM_QUEUE_SIZE;
    if (next_head == s_tq_tail) return; // 队列满则安全丢弃旧帧

    term_msg_t *msg = &s_term_queue[s_tq_head];
    msg->color = color;

    char temp[UI_TERM_LINE_LEN - 8];//e: 20]
    size_t max_copy = len < (sizeof(temp) - 1) ? len : (sizeof(temp) - 1);
    size_t out_idx = 0;

    for (size_t i = 0; i < max_copy; i++) {
        uint8_t c = buf[i];
        if (c == '\r' || c == '\n') continue;
        if (c >= 32 && c <= 126) {
            temp[out_idx++] = (char)c;
        } else {
            temp[out_idx++] = '.';
        }
    }
    temp[out_idx] = '\0';
    if (out_idx == 0) return;

    snprintf(msg->text, UI_TERM_LINE_LEN, "%s %s", tag, temp);//e: 20]
    s_tq_head = next_head;
}

// 统一指令发送出口 (支持 GP0 / USB / 双发)
static void send_uart_command(app_uart_data_t *data, const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;

    int8_t dest = data->dropdown_dest.selected_idx;//e: 17]
    size_t len = strlen(cmd);

    // 1. 发送给 GP0 (UART)
    if (dest == 0 || dest == 2) {
        for (size_t i = 0; i < len; i++) {
            while (!uart_is_writable(UART_INST)) tight_loop_contents();
            uart_putc_raw(UART_INST, cmd[i]);
            data->tx_bytes_cnt++;//e: 15]
        }
        while (!uart_is_writable(UART_INST)) tight_loop_contents();
        uart_putc_raw(UART_INST, '\r');
        while (!uart_is_writable(UART_INST)) tight_loop_contents();
        uart_putc_raw(UART_INST, '\n');
        data->tx_bytes_cnt += 2;//e: 15]
    }

    // 2. 发送给 USB (PC 上位机)
    if (dest == 1 || dest == 2) {
        for (size_t i = 0; i < len; i++) {
            putchar_raw(cmd[i]);
            data->rx_bytes_cnt++;//e: 15]
        }
        putchar_raw('\r');
        putchar_raw('\n');
        data->rx_bytes_cnt += 2;//e: 15]
        stdio_flush();
    }

    // 3. 在本地终端打印已发送回显
    const char *tag = (dest == 0) ? "[TX->GP0]" : ((dest == 1) ? "[TX->USB]" : "[TX->ALL]");
    char log_buf[UI_TERM_LINE_LEN];//e: 20]
    snprintf(log_buf, sizeof(log_buf), "%s %s", tag, cmd);
    ui_terminal_write_line(&data->terminal, log_buf, 0xFD20);//e: 20]

    data->dirty_rect = ui_terminal_get_dirty_rect(&data->terminal);//e: 20]
}

/* ================= 2. Core 1 专核业务主线程 (带帧组包) ================= */

static void app_uart_core1_worker(void) {
    app_uart_data_t *data = &s_uart_data;

    uint8_t tx_line_buf[64];
    size_t  tx_line_len = 0;
    uint64_t tx_last_us = 0;

    uint8_t rx_line_buf[64];
    size_t  rx_line_len = 0;
    uint64_t rx_last_us = 0;

    while (1) {
        uint64_t now_us = time_us_64();

        switch (data->work_mode) {//e: 15]
            case UART_MODE_BRIDGE: {//e: 15]
                // ================= 通道 A: PC (USB CDC) -> 模组 (GP0 TX) =================//e: 15]
                int usb_ch;
                while ((usb_ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
                    while (!uart_is_writable(UART_INST)) {
                        tight_loop_contents();
                    }
                    uart_putc_raw(UART_INST, (char)usb_ch);
                    data->tx_bytes_cnt++;//e: 15]

                    tx_last_us = now_us;
                    if (tx_line_len < sizeof(tx_line_buf) - 1) {
                        tx_line_buf[tx_line_len++] = (uint8_t)usb_ch;
                    }

                    if (usb_ch == '\n' || tx_line_len >= 48) {
                        queue_push_line("[TX]", tx_line_buf, tx_line_len, 0x05E0);
                        tx_line_len = 0;
                    }
                }

                if (tx_line_len > 0 && (now_us - tx_last_us > 25000)) {
                    queue_push_line("[TX]", tx_line_buf, tx_line_len, 0x05E0);
                    tx_line_len = 0;
                }

                // ================= 通道 B: 硬件 DMA 环形队列 -> PC (USB CDC) =================//e: 15]
                uint16_t head = (uint16_t)((uintptr_t)dma_hw->ch[s_dma_rx_chan].write_addr - (uintptr_t)s_rx_dma_ring) % RX_DMA_RING_SIZE;//e: 15]
                bool has_rx = false;

                while (s_rx_tail != head) {
                    uint8_t b = s_rx_dma_ring[s_rx_tail];
                    putchar_raw((char)b);
                    s_rx_tail = (s_rx_tail + 1) % RX_DMA_RING_SIZE;
                    data->rx_bytes_cnt++;//e: 15]
                    has_rx = true;

                    rx_last_us = now_us;
                    if (rx_line_len < sizeof(rx_line_buf) - 1) {
                        rx_line_buf[rx_line_len++] = b;
                    }

                    if (b == '\n' || rx_line_len >= 48) {
                        queue_push_line("[RX]", rx_line_buf, rx_line_len, 0x07FF);
                        rx_line_len = 0;
                    }
                }

                if (has_rx) {
                    stdio_flush(); // 强制刷出 USB 发送缓冲//e: 15]
                }

                if (rx_line_len > 0 && (now_us - rx_last_us > 25000)) {
                    queue_push_line("[RX]", rx_line_buf, rx_line_len, 0x07FF);
                    rx_line_len = 0;
                }
                break;
            }

            case UART_MODE_ECHO: {//e: 15]
                int ch;
                bool has_echo = false;
                while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
                    putchar_raw((char)ch);
                    if (uart_is_writable(UART_INST)) {
                        uart_putc_raw(UART_INST, (char)ch);
                    }
                    data->tx_bytes_cnt++;//e: 15]
                    data->rx_bytes_cnt++;//e: 15]
                    has_echo = true;
                }
                if (has_echo) {
                    stdio_flush();
                }
                break;
            }

            case UART_MODE_IDLE://e: 15]
            default:
                tight_loop_contents();
                break;
        }
    }
}

/* ================= 3. 交互回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab; (void)new_tab;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (data->kb.is_open) {//e: 19]
        ui_keyboard_close(&data->kb);//e: 19]
    }
    // 切换 Tab 时强制收起展开中的下拉菜单//e: 17]
    data->dropdown_dest.is_open = false;//e: 17]
    data->dropdown_mode.is_open = false;//e: 17]
}

static void on_bridge_switch(ui_switch_t *sw, bool state, void *user_data) {
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (state) {
        data->work_mode = UART_MODE_BRIDGE;//e: 15]
        ui_switch_set_state(&data->sw_echo_en, false);
    } else {
        if (data->work_mode == UART_MODE_BRIDGE) {//e: 15]
            data->work_mode = UART_MODE_IDLE;//e: 15]
        }
    }
}

static void on_echo_switch(ui_switch_t *sw, bool state, void *user_data) {
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (state) {
        data->work_mode = UART_MODE_ECHO;//e: 15]
        ui_switch_set_state(&data->sw_bridge_en, false);
    } else {
        if (data->work_mode == UART_MODE_ECHO) {//e: 15]
            data->work_mode = UART_MODE_IDLE;//e: 15]
        }
    }
}

static void on_clear_cnt_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    data->tx_bytes_cnt = 0;//e: 15]
    data->rx_bytes_cnt = 0;//e: 15]
    data->last_ui_tx   = 0;//e: 15]
    data->last_ui_rx   = 0;//e: 15]
    data->dirty_rect   = data->stat_card;//e: 15]
}

static void on_clear_term_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    ui_terminal_clear(&data->terminal);//e: 20]
    data->dirty_rect = ui_terminal_get_dirty_rect(&data->terminal);//e: 20]
}

static void on_quick_at_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    send_uart_command(data, "AT");
}

static void on_send_cmd_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (data->input_cmd.text[0] != '\0') {//e: 18]
        send_uart_command(data, data->input_cmd.text);//e: 18]
        ui_input_clear(&data->input_cmd);//e: 18]
        data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);//e: 18]
    }
}

// 输入框聚焦回调：唤起软键盘//e: 18, 19]
static void on_input_focus(ui_input_t *input, void *user_data) {
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    // 唤起软键盘前收起下拉菜单//e: 17, 19]
    data->dropdown_dest.is_open = false;//e: 17]
    data->dropdown_mode.is_open = false;//e: 17]
    ui_keyboard_open(&data->kb, input);//e: 19]
    data->dirty_rect = data->kb.rect;//e: 19]
}

// 软键盘提交回调//e: 19]
static void on_keyboard_submit(ui_keyboard_t *kb, const char *text, void *user_data) {
    (void)kb;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    if (text && text[0] != '\0') {
        send_uart_command(data, text);
        ui_input_clear(&data->input_cmd);//e: 18]
    }
    ui_keyboard_close(&data->kb);//e: 19]
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

// 终端格式切换回调//e: 17, 20]
static void on_mode_dropdown_change(ui_dropdown_t *dd, int8_t selected_idx, void *user_data) {
    (void)dd;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    ui_terminal_set_mode(&data->terminal, (selected_idx == 1) ? UI_TERM_MODE_HEX : UI_TERM_MODE_ASCII);//e: 20]
    data->dirty_rect = ui_terminal_get_dirty_rect(&data->terminal);//e: 20]
}

static void on_baud_next_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_uart_data_t *data = (app_uart_data_t *)user_data;
    data->baud_idx = (data->baud_idx + 1) % BAUD_TABLE_SIZE;//e: 15]
    uart_set_baudrate(UART_INST, s_baud_table[data->baud_idx]);//e: 15]
    data->dirty_rect = rect_union_box(data->dirty_rect, data->baud_card);
}

/* ================= 4. 生命周期管理与组件初始化 ================= */

static void app_uart_init(app_t *self) {
    self->user_data = &s_uart_data;
    app_uart_data_t *data = &s_uart_data;

    memset(data, 0, sizeof(app_uart_data_t));
    s_tq_head = 0;
    s_tq_tail = 0;

    data->dirty_rect      = ui_rect_make(0, 0, 0, 0);//e: 15]
    data->work_mode       = UART_MODE_BRIDGE;//e: 15]
    data->baud_idx        = 1; // 默认 115200//e: 15]

    // 1. 初始化 TabView (4 个 Tab)
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_uart_tabs, TAB_COUNT, 0, on_tab_changed);//e: 5, 6]
    data->tabview.user_data = data;

    // 2. Tab 0: 经典透传监视页
    data->stat_card = ui_rect_make(125, 48, 340, 100);//e: 15]
    ui_switch_init(&data->sw_bridge_en, 385, 168, 64, 32, true, on_bridge_switch);//e: 15]
    data->sw_bridge_en.user_data = data;

    ui_btn_init(&data->btn_clear_cnt, 175, 225, 240, 42, "清空吞吐统计", on_clear_cnt_click);//e: 15]
    data->btn_clear_cnt.user_data = data;
    data->btn_clear_cnt.style     = UI_BTN_STYLE_ROUND;//e: 15]
    data->btn_clear_cnt.font_size = FONT_20;//e: 15]
    ui_btn_set_color(&data->btn_clear_cnt, 0xBF5E, 0x03DF, 0x1A7B);//e: 15]

    // 3. Tab 1: 全功能交互数据终端
    // 顶部工具条 (Y: 36, H: 28)//e: 17]
    ui_dropdown_init(&data->dropdown_dest, 115, 36, 105, 28, s_dest_options, DEST_OPTION_COUNT, 0, NULL);//e: 17]
    data->dropdown_dest.user_data = data;//e: 17]

    ui_dropdown_init(&data->dropdown_mode, 226, 36, 80, 28, s_mode_options, MODE_OPTION_COUNT, 0, on_mode_dropdown_change);//e: 17]
    data->dropdown_mode.user_data = data;//e: 17]

    ui_btn_init(&data->btn_clear_term, 312, 36, 68, 28, "清屏", on_clear_term_click);
    data->btn_clear_term.user_data = data;
    data->btn_clear_term.style     = UI_BTN_STYLE_ROUND;
    data->btn_clear_term.font_size = FONT_16;
    ui_btn_set_color(&data->btn_clear_term, 0x8410, 0x4208, 0xFFFF);

    ui_btn_init(&data->btn_quick_at, 386, 36, 84, 28, "AT握手", on_quick_at_click);
    data->btn_quick_at.user_data = data;
    data->btn_quick_at.style     = UI_BTN_STYLE_ROUND;
    data->btn_quick_at.font_size = FONT_16;
    ui_btn_set_color(&data->btn_quick_at, 0x05E0, 0x03E0, 0xFFFF);

    // 中部控制台终端 (Y: 68, H: 198)//e: 20]
    ui_terminal_init(&data->terminal, 115, 68, 355, 198);//e: 20]

    // 底部输入框与发送按钮 (Y: 272, H: 36)//e: 18]
    ui_input_init(&data->input_cmd, 115, 272, 285, 36, "点击输入指令...", on_input_focus);//e: 18]
    data->input_cmd.user_data = data;

    ui_btn_init(&data->btn_send_cmd, 406, 272, 64, 36, "发送", on_send_cmd_click);
    data->btn_send_cmd.user_data = data;
    data->btn_send_cmd.style     = UI_BTN_STYLE_ROUND;
    data->btn_send_cmd.font_size = FONT_16;
    ui_btn_set_color(&data->btn_send_cmd, 0x03DF, 0x0275, 0xFFFF);

    // 虚拟软键盘//e: 19]
    ui_keyboard_init(&data->kb, on_keyboard_submit);//e: 19]
    data->kb.user_data = data;//e: 19]

    // 4. Tab 2: 波特率配置//e: 15]
    data->baud_card = ui_rect_make(125, 48, 340, 90);//e: 15]
    ui_btn_init(&data->btn_baud_next, 175, 170, 240, 44, "切换下一档波特率", on_baud_next_click);//e: 15]
    data->btn_baud_next.user_data = data;
    data->btn_baud_next.style     = UI_BTN_STYLE_ROUND;//e: 15]
    data->btn_baud_next.font_size = FONT_20;//e: 15]
    ui_btn_set_color(&data->btn_baud_next, 0xBF5E, 0x03DF, 0x1A7B);//e: 15]

    // 5. Tab 3: 硬件回显//e: 15]
    data->echo_card = ui_rect_make(125, 48, 340, 100);//e: 15]
    ui_switch_init(&data->sw_echo_en, 385, 175, 64, 32, false, on_echo_switch);//e: 15]
    data->sw_echo_en.user_data = data;
}

static void app_uart_enter(app_t *self) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    stdio_set_translate_crlf(&stdio_usb, false);//e: 15]

    uart_init(UART_INST, s_baud_table[data->baud_idx]);//e: 15]
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);//e: 15]
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);//e: 15]
    gpio_pull_up(PIN_UART_RX);//e: 15]

    uart_set_hw_flow(UART_INST, false, false);//e: 15]
    uart_set_format(UART_INST, 8, 1, UART_PARITY_NONE);//e: 15]
    uart_set_fifo_enabled(UART_INST, true);//e: 15]

    while (uart_is_readable(UART_INST)) (void)uart_getc(UART_INST);//e: 15]

    s_rx_tail = 0;//e: 15]
    if (s_dma_rx_chan < 0) {
        s_dma_rx_chan = dma_claim_unused_channel(true);//e: 15]
    }

    dma_channel_config c = dma_channel_get_default_config(s_dma_rx_chan);//e: 15]
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);//e: 15]
    channel_config_set_read_increment(&c, false);//e: 15]
    channel_config_set_write_increment(&c, true);//e: 15]
    channel_config_set_ring(&c, true, 11);//e: 15]
    channel_config_set_dreq(&c, uart_get_dreq(UART_INST, false));//e: 15]

    dma_channel_configure(
        s_dma_rx_chan,
        &c,
        s_rx_dma_ring,//e: 15]
        &uart_get_hw(UART_INST)->dr,//e: 15]
        0xFFFFFFFF,//e: 15]
        true//e: 15]
    );

    multicore_reset_core1();//e: 15]
    multicore_launch_core1(app_uart_core1_worker);//e: 15]
}

static void app_uart_exit(app_t *self) {
    (void)self;
    multicore_reset_core1();//e: 15]

    if (s_dma_rx_chan >= 0) {
        dma_channel_abort(s_dma_rx_chan);//e: 15]
        dma_channel_unclaim(s_dma_rx_chan);//e: 15]
        s_dma_rx_chan = -1;//e: 15]
    }

    uart_deinit(UART_INST);//e: 15]
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_NULL);//e: 15]
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_NULL);//e: 15]
}

/* ================= 5. ⚡ 硬件时间戳 100ms 节流更新与队列消费 ================= */

static void app_uart_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 1. 消费跨核队列并输出至终端
    bool term_updated = false;
    while (s_tq_tail != s_tq_head) {
        term_msg_t *msg = &s_term_queue[s_tq_tail];
        ui_terminal_write_line(&data->terminal, msg->text, msg->color);//e: 20]
        s_tq_tail = (s_tq_tail + 1) % TERM_QUEUE_SIZE;
        term_updated = true;
    }

    if (term_updated && data->tabview.active_tab_idx == 1 && !data->kb.is_open) {//e: 19]
        data->dirty_rect = ui_terminal_get_dirty_rect(&data->terminal);//e: 20]
    }

    // 2. 100ms 节流更新经典指标
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - data->last_stat_tick >= 100) {
        bool data_changed = (data->tx_bytes_cnt != data->last_ui_tx) ||//e: 15]
                            (data->rx_bytes_cnt != data->last_ui_rx);//e: 15]

        if (data_changed) {
            data->last_stat_tick = now_ms;
            data->last_ui_tx     = data->tx_bytes_cnt;//e: 15]
            data->last_ui_rx     = data->rx_bytes_cnt;//e: 15]

            if (data->tabview.active_tab_idx == 0) {
                data->dirty_rect = data->stat_card;//e: 15]
            } else if (data->tabview.active_tab_idx == 3) {
                data->dirty_rect = data->echo_card;//e: 15]
            }
        }
    }
}

/* ================= 6. ⚡ 触控分发（核心修复：合并展开与收起脏矩形） ================= */

static bool app_uart_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;

    // 1. 软键盘打开时优先捕获//e: 19]
    if (data->kb.is_open) {//e: 19]
        if (ui_keyboard_process_touch(&data->kb, is_touching, tx, ty, now_tick)) {//e: 19]
            data->dirty_rect = data->kb.rect;//e: 19]
            return true;
        }
        if (is_touching && !ui_rect_contains_point(&data->kb.rect, tx, ty)) {//e: 19]
            ui_keyboard_close(&data->kb);//e: 19]
            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            return true;
        }
        return false;
    }

    // 2. ⚡ 下拉菜单展开时优先全局捕获（合并 old_rect 与 new_rect 彻底擦除菜单项）//e: 17]
    if (data->dropdown_dest.is_open) {//e: 17]
        ui_rect_t old_r = ui_dropdown_get_total_rect(&data->dropdown_dest);//e: 17]
        bool changed = ui_dropdown_process_touch(&data->dropdown_dest, is_touching, tx, ty, now_tick);//e: 17]
        if (changed) {
            ui_rect_t new_r = ui_dropdown_get_total_rect(&data->dropdown_dest);//e: 17]
            data->dirty_rect = rect_union_box(old_r, new_r);
            return true;
        }
        // 点击外部空白区域自动收起//e: 17]
        if (is_touching && !ui_rect_contains_point(&old_r, tx, ty)) {
            data->dropdown_dest.is_open = false;//e: 17]
            data->dirty_rect = old_r;
            return true;
        }
        return false;
    }

    if (data->dropdown_mode.is_open) {//e: 17]
        ui_rect_t old_r = ui_dropdown_get_total_rect(&data->dropdown_mode);//e: 17]
        bool changed = ui_dropdown_process_touch(&data->dropdown_mode, is_touching, tx, ty, now_tick);//e: 17]
        if (changed) {
            ui_rect_t new_r = ui_dropdown_get_total_rect(&data->dropdown_mode);//e: 17]
            data->dirty_rect = rect_union_box(old_r, new_r);
            return true;
        }
        // 点击外部空白区域自动收起//e: 17]
        if (is_touching && !ui_rect_contains_point(&old_r, tx, ty)) {
            data->dropdown_mode.is_open = false;//e: 17]
            data->dirty_rect = old_r;
            return true;
        }
        return false;
    }

    // 3. 侧边栏触控//e: 5, 6]
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {//e: 5, 6]
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;//e: 5, 6]
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);//e: 6]
        }
        return true;
    }

    // 4. 当前 Tab 控件事件
    switch (data->tabview.active_tab_idx) {
        case 0: { // 经典透传监视页//e: 15]
            bool sw_act  = ui_switch_process_touch(&data->sw_bridge_en, is_touching, tx, ty, now_tick);//e: 15]
            bool clr_act = ui_btn_process_touch(&data->btn_clear_cnt, is_touching, tx, ty, now_tick);//e: 15]
            if (sw_act || clr_act) {
                ui_rect_t w_dirty = sw_act ? ui_switch_get_dirty_rect(&data->sw_bridge_en) 
                                           : ui_btn_get_dirty_rect(&data->btn_clear_cnt);//e: 15]
                ui_rect_union(&w_dirty, &data->stat_card, &data->dirty_rect);//e: 15]
                return true;
            }
            break;
        }

        case 1: { // 全功能数据终端页
            // ⚡ 打开目标下拉菜单（捕获展开前后的矩形）//e: 17]
            ui_rect_t dest_old = ui_dropdown_get_total_rect(&data->dropdown_dest);//e: 17]
            if (ui_dropdown_process_touch(&data->dropdown_dest, is_touching, tx, ty, now_tick)) {//e: 17]
                ui_rect_t dest_new = ui_dropdown_get_total_rect(&data->dropdown_dest);//e: 17]
                data->dirty_rect = rect_union_box(dest_old, dest_new);
                if (data->dropdown_dest.is_open && data->dropdown_mode.is_open) {//e: 17]
                    ui_rect_t mode_r = ui_dropdown_get_total_rect(&data->dropdown_mode);//e: 17]
                    data->dropdown_mode.is_open = false;//e: 17]
                    data->dirty_rect = rect_union_box(data->dirty_rect, mode_r);
                }
                return true;
            }

            // ⚡ 打开格式下拉菜单//e: 17]
            ui_rect_t mode_old = ui_dropdown_get_total_rect(&data->dropdown_mode);//e: 17]
            if (ui_dropdown_process_touch(&data->dropdown_mode, is_touching, tx, ty, now_tick)) {//e: 17]
                ui_rect_t mode_new = ui_dropdown_get_total_rect(&data->dropdown_mode);//e: 17]
                data->dirty_rect = rect_union_box(mode_old, mode_new);
                if (data->dropdown_mode.is_open && data->dropdown_dest.is_open) {//e: 17]
                    ui_rect_t dest_r = ui_dropdown_get_total_rect(&data->dropdown_dest);//e: 17]
                    data->dropdown_dest.is_open = false;//e: 17]
                    data->dirty_rect = rect_union_box(data->dirty_rect, dest_r);
                }
                return true;
            }

            if (ui_btn_process_touch(&data->btn_clear_term, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_quick_at, is_touching, tx, ty, now_tick)) return true;
            if (ui_input_process_touch(&data->input_cmd, is_touching, tx, ty, now_tick)) return true;//e: 18]
            if (ui_btn_process_touch(&data->btn_send_cmd, is_touching, tx, ty, now_tick)) return true;
            if (ui_terminal_process_touch(&data->terminal, is_touching, tx, ty, now_tick)) {//e: 20]
                data->dirty_rect = ui_terminal_get_dirty_rect(&data->terminal);//e: 20]
                return true;
            }
            break;
        }

        case 2: { // 波特率页//e: 15]
            if (ui_btn_process_touch(&data->btn_baud_next, is_touching, tx, ty, now_tick)) {//e: 15]
                ui_rect_t b_dirty = ui_btn_get_dirty_rect(&data->btn_baud_next);//e: 15]
                data->dirty_rect = rect_union_box(data->dirty_rect, b_dirty);
                return true;
            }
            break;
        }

        case 3: { // 硬件回显页//e: 15]
            if (ui_switch_process_touch(&data->sw_echo_en, is_touching, tx, ty, now_tick)) {//e: 15]
                ui_rect_t s_dirty = ui_switch_get_dirty_rect(&data->sw_echo_en);//e: 15]
                data->dirty_rect = rect_union_box(data->dirty_rect, s_dirty);
                return true;
            }
            break;
        }
    }

    if (data->dirty_rect.w > 0 && data->dirty_rect.h > 0) {
        return true;
    }

    return false;
}

/* ================= 7. 切片光栅化渲染 ================= */

static void app_uart_draw(app_t *self, ui_tile_t *tile) {
    app_uart_data_t *data = (app_uart_data_t *)self->user_data;
    ui_rect_t overlap;

    // 1. 绘制白底背景//e: 15]
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);//e: 15]
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {//e: 15]
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);//e: 15]
    }

    // 2. 绘制 TabView 侧边栏//e: 5, 6, 15]
    ui_tabview_draw(tile, &data->tabview);//e: 5, 6, 15]

    // 3. 右侧内容区切片测试//e: 15]
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {//e: 15]
        return;
    }

    char str_buf[64];//e: 15]

    switch (data->tabview.active_tab_idx) {
        case 0: { // ===== Tab 0: 经典透传监视页 =====//e: 15]
            if (ui_rect_intersect(&tile->rect, &data->stat_card, &overlap)) {//e: 15]
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);//e: 15]
                ui_draw_rect(tile, &data->stat_card, 0xC618);//e: 15]

                ui_draw_string(tile, 140, 56, "双核高频透传 (DMA 硬件零丢包)", FONT_16, 0x1A7B, 0xEF7D);//e: 15]

                snprintf(str_buf, sizeof(str_buf), "TX (USB->GP0): %lu Bytes", data->tx_bytes_cnt);//e: 15]
                ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);//e: 15]

                snprintf(str_buf, sizeof(str_buf), "RX (GP1->USB): %lu Bytes", data->rx_bytes_cnt);//e: 15]
                ui_draw_string(tile, 140, 104, str_buf, FONT_16, 0x03DF, 0xEF7D);//e: 15]

                uint16_t dot_color = (data->work_mode == UART_MODE_BRIDGE) ? 0x05E0 : 0x8410;//e: 15]
                ui_draw_fill_circle(tile, 445, 64, 6, dot_color);//e: 15]
            }

            int16_t lbl_y = 174;//e: 15]
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {//e: 15]
                ui_draw_string(tile, 130, lbl_y, "透传引擎使能:", FONT_20, 0x1A7B, 0xFFFF);//e: 15]
            }

            ui_switch_draw(tile, &data->sw_bridge_en);//e: 15]
            ui_btn_draw(tile, &data->btn_clear_cnt);//e: 15]
            break;
        }

        case 1: { // ===== Tab 1: 全功能交互数据终端 =====
            // A. 中部终端控制台//e: 20]
            ui_terminal_draw(tile, &data->terminal);//e: 20]

            // B. 顶部与底部按钮 / 输入框//e: 18]
            ui_btn_draw(tile, &data->btn_clear_term);
            ui_btn_draw(tile, &data->btn_quick_at);
            ui_input_draw(tile, &data->input_cmd);//e: 18]
            ui_btn_draw(tile, &data->btn_send_cmd);

            // C. ⚡ 下拉菜单位于当前工作区最顶层绘制 (防止展开列表被终端覆盖)//e: 17]
            ui_dropdown_draw(tile, &data->dropdown_dest);//e: 17]
            ui_dropdown_draw(tile, &data->dropdown_mode);//e: 17]
            break;
        }

        case 2: { // ===== Tab 2: 波特率页 =====//e: 15]
            if (ui_rect_intersect(&tile->rect, &data->baud_card, &overlap)) {//e: 15]
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);//e: 15]
                ui_draw_rect(tile, &data->baud_card, 0xC618);//e: 15]

                ui_draw_string(tile, 140, 58, "当前通信波特率", FONT_16, 0x1A7B, 0xEF7D);//e: 15]

                snprintf(str_buf, sizeof(str_buf), "%lu bps (8-N-1)", s_baud_table[data->baud_idx]);//e: 15]
                ui_draw_string(tile, 140, 86, str_buf, FONT_24, 0x03DF, 0xEF7D);//e: 15]
            }

            int16_t tip_y = 230;//e: 15]
            if ((tip_y + 20) > tile->rect.y && tip_y < (tile->rect.y + tile->rect.h)) {//e: 15]
                ui_draw_string(tile, 140, tip_y, "支持 9600 ~ 2Mbps 极速切换", FONT_16, 0x4208, 0xFFFF);//e: 15]
            }

            ui_btn_draw(tile, &data->btn_baud_next);//e: 15]
            break;
        }

        case 3: { // ===== Tab 3: 硬件回显 =====//e: 15]
            if (ui_rect_intersect(&tile->rect, &data->echo_card, &overlap)) {//e: 15]
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);//e: 15]
                ui_draw_rect(tile, &data->echo_card, 0xC618);//e: 15]

                ui_draw_string(tile, 140, 56, "硬件自测回显模式", FONT_16, 0x1A7B, 0xEF7D);//e: 15]
                snprintf(str_buf, sizeof(str_buf), "回显吞吐量: %lu Bytes (1:1)", data->tx_bytes_cnt);//e: 15]
                ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x05E0, 0xEF7D);//e: 15]
                ui_draw_string(tile, 140, 104, "USB 输入自动原路回传并打给 GP0", FONT_16, 0x4208, 0xEF7D);//e: 15]

                uint16_t dot_color = (data->work_mode == UART_MODE_ECHO) ? 0x05E0 : 0x8410;//e: 15]
                ui_draw_fill_circle(tile, 445, 64, 6, dot_color);//e: 15]
            }

            int16_t lbl_y = 180;//e: 15]
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {//e: 15]
                ui_draw_string(tile, 130, lbl_y, "开启自发自收:", FONT_20, 0x1A7B, 0xFFFF);//e: 15]
            }

            ui_switch_draw(tile, &data->sw_echo_en);//e: 15]
            break;
        }
    }

    // 4. ⚡ 软键盘作为全局顶层浮层绘制//e: 19]
    if (data->kb.is_open) {//e: 19]
        ui_keyboard_draw(tile, &data->kb);//e: 19]
    }
}

/* ================= 8. 导出单例实体 ================= */

app_t *app_uart_term_get_instance(void) {
    s_app_uart.id            = APP_ID_UART_TERM;//e: 13, 15]
    s_app_uart.name          = "串口助手";//e: 15]
    s_app_uart.init          = app_uart_init;//e: 15]
    s_app_uart.enter         = app_uart_enter;//e: 15]
    s_app_uart.exit          = app_uart_exit;//e: 15]
    s_app_uart.update        = app_uart_update;//e: 15]
    s_app_uart.process_touch = app_uart_process_touch;//e: 15]
    s_app_uart.draw          = app_uart_draw;//e: 15]
    return &s_app_uart;
}