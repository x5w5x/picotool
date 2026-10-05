#include "app_esp.h"
#include "app_shared_ram.h"
#include "app_base.h"
#include "fun_esp.h"
#include "ff.h"

#include "ui_draw.h"
#include "ui_tabview.h"
#include "ui_terminal.h"
#include "ui_btn.h"
#include "ui_input.h"
#include "ui_keyboard.h"
#include "ui_switch.h"
#include "ui_toast.h"
#include "lcd.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * 1. 用户专属专属 AP 配置 (在此写死你的固定名字与密码)
 * ============================================================================ */
#define ESP_AP_SSID             "RP2040_DAP_LINK"   // ⚡ 你的专属热点名称
#define ESP_AP_PASSWORD         "11111111"          // ⚡ 你的专属热点密码 (≥8位)

/* ============================================================================
 * 2. 调色板定义 (纯白画布 0xFFFF + 浅银灰卡片 0xDEFB + 深炭灰文字 0x4208)
 * ============================================================================ */
#define ESP_COL_CANVAS          0xFFFF  // 全局纯白底板
#define ESP_COL_PANEL           0xDEFB  // 卡片/按键/键帽浅银灰底色
#define ESP_COL_BORDER          0xCE59  // 浅灰柔和描边
#define ESP_COL_TEXT            0x4208  // 统一深炭灰文字 (杜绝一切藏青)
#define ESP_COL_TEXT_MUTED      0x8410  // 辅助暗灰字符
#define ESP_COL_ACCENT          0x03DF  // 科技青蓝 (连接/高亮确认键)
#define ESP_COL_ACCENT_HOVER    0x0275  // 深青
#define ESP_COL_SIGNAL_OK       0x03E0  // 信号与成功状态绿

#define ESP_QUEUE_SIZE          16      // 16 帧缓冲已足够高频消费
#define ESP_LINE_SIZE           48      // 单行 48 字节完全满足 350px 宽度
#define MAX_WIFI_SCAN           5

#define ESP_TERM_X             120
#define ESP_TERM_Y             38
#define ESP_TERM_W             350
#define ESP_TERM_H             226

#define ESP_INPUT_X            120
#define ESP_INPUT_Y            272
#define ESP_INPUT_W            226
#define ESP_INPUT_H            36

#define ESP_CLEAR_X            352
#define ESP_CLEAR_Y            272
#define ESP_CLEAR_W            56
#define ESP_CLEAR_H            36

#define ESP_SEND_X             414
#define ESP_SEND_Y             272
#define ESP_SEND_W             56
#define ESP_SEND_H             36

static const char *s_esp_tabs[] = { "无线管理", "调试终端", "网络状态" };
#define ESP_TAB_COUNT           ((uint8_t)(sizeof(s_esp_tabs) / sizeof(s_esp_tabs[0])))

typedef struct {
    char     text[ESP_LINE_SIZE];
    uint16_t color;
} esp_msg_t;

typedef struct {
    char    ssid[32];
    int8_t  rssi;
    bool    valid;
} wifi_scan_item_t;

/* ============================================================================
 * 3. 静态 RAM 共享数据结构 (队列内嵌，0 额外全局 BSS 开销)
 * ============================================================================ */
typedef struct {
    ui_rect_t       dirty_rect;
    ui_tabview_t    tabview;
    uint8_t         last_tab_idx;

    /* ⚡ 移入共享内存的跨核队列 (立省 2KB 全局物理 RAM) */
    esp_msg_t       queue[ESP_QUEUE_SIZE];
    volatile uint8_t head;
    volatile uint8_t tail;

    /* 网络状态参数 */
    char            ip_addr[24];
    bool            ap_enabled;
    bool            sta_enabled;
    bool            web_enabled;

    /* Tab 0: 服务控制卡片 (AP / STA / Web) */
    ui_rect_t       service_card;
    ui_switch_t     sw_ap;
    ui_switch_t     sw_sta;
    ui_switch_t     sw_web;
    ui_btn_t        btn_refresh_ip;

    /* Tab 0: 扫描列表与连接交互 */
    ui_rect_t       scan_card;
    ui_btn_t        btn_scan_wifi;
    wifi_scan_item_t scan_list[MAX_WIFI_SCAN];
    uint8_t         scan_count;

    /* Wi-Fi 密码连接弹窗 */
    bool            connect_modal_open;
    char            connecting_ssid[32];
    ui_input_t      input_wifi_pass;
    ui_btn_t        btn_do_connect;
    ui_btn_t        btn_cancel_connect;

    /* Tab 1: 调试终端 */
    ui_terminal_t   terminal;
    ui_input_t      input_cmd;
    ui_keyboard_t   keyboard;
    ui_btn_t        btn_send;
    ui_btn_t        btn_clear;

    /* Tab 2: 硬件状态 */
    ui_rect_t       status_card;
    ui_toast_t      toast;
    volatile uint32_t rx_count;
    volatile uint32_t tx_count;
    uint32_t        last_rx_count;

    volatile bool   core1_run;
} esp_app_data_t;

static app_t s_app_esp;

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

/* 官方 FatFs 代码页 936 汉字转换 */
static void utf8_to_gbk_string(const char *utf8, char *gbk_out, size_t max_out_len)
{
    if (!utf8 || !gbk_out || max_out_len == 0) return;
    size_t in_idx = 0, out_idx = 0;

    while (utf8[in_idx] != '\0' && out_idx < max_out_len - 1) {
        uint8_t c = (uint8_t)utf8[in_idx];
        if (c < 0x80) {
            gbk_out[out_idx++] = (char)c;
            in_idx++;
        } else if ((c & 0xF0) == 0xE0 && utf8[in_idx + 1] && utf8[in_idx + 2]) {
            uint8_t c2 = (uint8_t)utf8[in_idx + 1];
            uint8_t c3 = (uint8_t)utf8[in_idx + 2];
            WCHAR unicode = (WCHAR)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
            DWORD gbk_code = ff_uni2oem(unicode, 936);
            if (gbk_code > 0xFF) {
                if (out_idx + 2 < max_out_len) {
                    gbk_out[out_idx++] = (char)(gbk_code >> 8);
                    gbk_out[out_idx++] = (char)(gbk_code & 0xFF);
                }
            } else if (gbk_code > 0) {
                gbk_out[out_idx++] = (char)gbk_code;
            } else {
                gbk_out[out_idx++] = '?';
            }
            in_idx += 3;
        } else if ((c & 0xE0) == 0xC0 && utf8[in_idx + 1]) {
            uint8_t c2 = (uint8_t)utf8[in_idx + 1];
            WCHAR unicode = (WCHAR)(((c & 0x1F) << 6) | (c2 & 0x3F));
            DWORD gbk_code = ff_uni2oem(unicode, 936);
            if (gbk_code > 0 && out_idx < max_out_len - 1) {
                gbk_out[out_idx++] = (char)gbk_code;
            }
            in_idx += 2;
        } else {
            in_idx++;
        }
    }
    gbk_out[out_idx] = '\0';
}

static inline void esp_queue_push(esp_app_data_t *data, const char *text, uint16_t color)
{
    uint8_t next = (uint8_t)((data->head + 1) % ESP_QUEUE_SIZE);
    if (next == data->tail) return; // 满队列丢弃
    strncpy(data->queue[data->head].text, text, ESP_LINE_SIZE - 1);
    data->queue[data->head].text[ESP_LINE_SIZE - 1] = '\0';
    data->queue[data->head].color = color;
    data->head = next;
}

/* ============================================================================
 * 4. Core 1: 921600 高速串口抽采线程
 * ============================================================================ */
static void esp_core1_worker(void)
{
    esp_app_data_t *data = (esp_app_data_t *)g_static_app_ram;
    uint8_t rx_buf[128];
    char    line_accum[ESP_LINE_SIZE];
    size_t  line_len = 0;
    uint64_t last_rx_us = 0;

    while (data->core1_run) {
        size_t len = esp_read(rx_buf, sizeof(rx_buf));
        uint64_t now_us = time_us_64();

        if (len > 0) {
            data->rx_count += len;
            for (size_t i = 0; i < len; i++) {
                uint8_t c = rx_buf[i];
                if (c == '\r') continue;
                if (c == '\n' || line_len >= ESP_LINE_SIZE - 2) {
                    line_accum[line_len] = '\0';
                    char gbk_converted[ESP_LINE_SIZE];
                    utf8_to_gbk_string(line_accum, gbk_converted, sizeof(gbk_converted));
                    esp_queue_push(data, gbk_converted, 0x5E7F);
                    line_len = 0;
                } else {
                    line_accum[line_len++] = (char)c;
                }
            }
            last_rx_us = now_us;
        } else {
            if (line_len > 0 && (now_us - last_rx_us > 30000u)) {
                line_accum[line_len] = '\0';
                char gbk_converted[ESP_LINE_SIZE];
                utf8_to_gbk_string(line_accum, gbk_converted, sizeof(gbk_converted));
                esp_queue_push(data, gbk_converted, 0x5E7F);
                line_len = 0;
            }
            sleep_us(500);
        }
    }
}

/* ============================================================================
 * 5. 指令发射与协议嗅探器
 * ============================================================================ */
static void esp_send_cmd_line(esp_app_data_t *data, const char *cmd)
{
    if (!data || !cmd || cmd[0] == '\0') return;

    char send_buf[96];
    snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);

    esp_send_string(send_buf);
    data->tx_count += strlen(send_buf);

    char echo[96];
    snprintf(echo, sizeof(echo), "> %s", cmd);
    ui_terminal_write_line(&data->terminal, echo, 0x07FF);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void esp_sniff_protocol_line(esp_app_data_t *data, const char *line)
{
    if (!data || !line) return;

    // 1. 嗅探 IP 地址
    const char *p_ip = strstr(line, "IP:");
    if (!p_ip) p_ip = strstr(line, "ip:");
    if (p_ip) {
        p_ip += 3;
        while (*p_ip == ' ') p_ip++;
        size_t len = 0;
        while (p_ip[len] && p_ip[len] != ' ' && p_ip[len] != '\r' && p_ip[len] != '\n' && len < 20) {
            data->ip_addr[len] = p_ip[len];
            len++;
        }
        data->ip_addr[len] = '\0';
        data->dirty_rect = rect_union_box(data->dirty_rect, data->service_card);
        return;
    }

    // 2. 嗅探 Wi-Fi 扫描结果
    const char *p_scan = strstr(line, "+SCAN:");
    if (!p_scan) p_scan = strstr(line, "#AP:");
    if (p_scan) {
        p_scan += (line[1] == 'S') ? 6 : 4;
        if (data->scan_count < MAX_WIFI_SCAN) {
            wifi_scan_item_t *item = &data->scan_list[data->scan_count];
            const char *comma = strchr(p_scan, ',');
            if (comma) {
                size_t s_len = comma - p_scan;
                if (s_len >= sizeof(item->ssid)) s_len = sizeof(item->ssid) - 1;
                strncpy(item->ssid, p_scan, s_len);
                item->ssid[s_len] = '\0';
                item->rssi = (int8_t)atoi(comma + 1);
            } else {
                strncpy(item->ssid, p_scan, sizeof(item->ssid) - 1);
                item->rssi = -60;
            }
            item->valid = true;
            data->scan_count++;
            data->dirty_rect = rect_union_box(data->dirty_rect, data->scan_card);
        }
    }
}

/* ============================================================================
 * 6. Tab 0: AP / STA / Web 三大网络服务控制回调
 * ============================================================================ */
static void on_ap_switch_change(ui_switch_t *sw, bool state, void *user_data)
{
    (void)sw;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->ap_enabled = state;

    // ⚡ 开启 AP 时直接注入专属热点名称与密码
    if (state) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "#WIFI:AP,1,\"%s\",\"%s\"", ESP_AP_SSID, ESP_AP_PASSWORD);
        esp_send_cmd_line(data, cmd);
        ui_toast_show(&data->toast, "已启动专属 AP 热点", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        esp_send_cmd_line(data, "#WIFI:AP,0");
        ui_toast_show(&data->toast, "AP 热点已关闭", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = rect_union_box(data->dirty_rect, data->service_card);
}

static void on_sta_switch_change(ui_switch_t *sw, bool state, void *user_data)
{
    (void)sw;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->sta_enabled = state;
    esp_send_cmd_line(data, state ? "#WIFI:STA,1" : "#WIFI:STA,0");
    ui_toast_show(&data->toast, state ? "STA 联网开启" : "STA 联网关闭", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = rect_union_box(data->dirty_rect, data->service_card);
}

static void on_web_switch_change(ui_switch_t *sw, bool state, void *user_data)
{
    (void)sw;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->web_enabled = state;
    esp_send_cmd_line(data, state ? "#WEB:1" : "#WEB:0");
    ui_toast_show(&data->toast, state ? "Web 服务已启动" : "Web 服务已停止", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = rect_union_box(data->dirty_rect, data->service_card);
}

static void on_refresh_ip_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    esp_send_cmd_line(data, "#IP?");
    ui_toast_show(&data->toast, "正在刷新 IP...", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
}

static void on_scan_wifi_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->scan_count = 0;
    esp_send_cmd_line(data, "#WIFI:SCAN");
    ui_toast_show(&data->toast, "正在扫描热点...", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = rect_union_box(data->dirty_rect, data->scan_card);
}

static void on_do_connect_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "#WIFI:CONNECT,\"%s\",\"%s\"", data->connecting_ssid, data->input_wifi_pass.text);
    esp_send_cmd_line(data, cmd);

    data->connect_modal_open = false;
    ui_keyboard_close(&data->keyboard);
    ui_toast_show(&data->toast, "下发连接指令", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_cancel_connect_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->connect_modal_open = false;
    ui_keyboard_close(&data->keyboard);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_pass_input_focus(ui_input_t *input, void *user_data)
{
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    data->keyboard.mode = UI_KB_MODE_LOWER;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

/* ============================================================================
 * 7. Tab 0: 绘制主界面 (纯白底板 + 浅银灰卡片 + 深炭灰文字)
 * ============================================================================ */
static void draw_wifi_management_page(ui_tile_t *tile, esp_app_data_t *data)
{
    ui_rect_t ov;

    /* 1. 顶部网络服务卡片 */
    if (ui_rect_intersect(&tile->rect, &data->service_card, &ov)) {
        ui_draw_fill_rect(tile, &ov, ESP_COL_PANEL);
        ui_draw_rect(tile, &data->service_card, ESP_COL_BORDER);

        ui_draw_string(tile, data->service_card.x + 12, data->service_card.y + 10, "IP 地址:", FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
        ui_draw_string(tile, data->service_card.x + 80, data->service_card.y + 8,
                       data->ip_addr[0] ? data->ip_addr : "0.0.0.0 (未联网)", FONT_20, ESP_COL_TEXT, ESP_COL_PANEL);

        // 提示专属 AP 参数
        char ap_info[64];
        snprintf(ap_info, sizeof(ap_info), "预设AP: %s  密: %s", ESP_AP_SSID, ESP_AP_PASSWORD);
        ui_draw_string(tile, data->service_card.x + 12, data->service_card.y + 32, ap_info, FONT_16, ESP_COL_TEXT_MUTED, ESP_COL_PANEL);

        // 3 大开关标签
        ui_draw_string(tile, data->service_card.x + 14,  data->service_card.y + 54, "AP热点", FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
        ui_draw_string(tile, data->service_card.x + 130, data->service_card.y + 54, "STA模式", FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
        ui_draw_string(tile, data->service_card.x + 246, data->service_card.y + 54, "Web服务", FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
    }

    ui_btn_draw(tile, &data->btn_refresh_ip);
    ui_switch_draw(tile, &data->sw_ap);
    ui_switch_draw(tile, &data->sw_sta);
    ui_switch_draw(tile, &data->sw_web);

    /* 2. 下部 Wi-Fi 扫描热点列表卡片 */
    if (ui_rect_intersect(&tile->rect, &data->scan_card, &ov)) {
        ui_draw_fill_rect(tile, &ov, ESP_COL_PANEL);
        ui_draw_rect(tile, &data->scan_card, ESP_COL_BORDER);
        ui_draw_string(tile, data->scan_card.x + 12, data->scan_card.y + 10, "附近 Wi-Fi 列表", FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
    }
    ui_btn_draw(tile, &data->btn_scan_wifi);

    // 渲染扫描到的 Wi-Fi 条目
    // 渲染扫描到的 Wi-Fi 条目 (5条刚好 125px，完美嵌入 162px 卡片)
    int16_t item_y_base = data->scan_card.y + 34;
    if (data->scan_count == 0) {
        ui_draw_string(tile, data->scan_card.x + 50, item_y_base + 35, "暂无列表，请点击 [扫描热点]", FONT_16, ESP_COL_TEXT_MUTED, ESP_COL_PANEL);
    } else {
        for (uint8_t i = 0; i < data->scan_count && i < MAX_WIFI_SCAN; i++) {
            int16_t iy = item_y_base + i * 25; // 25px 步进
            ui_rect_t item_r = ui_rect_make(data->scan_card.x + 8, iy, data->scan_card.w - 16, 23);
            if (!ui_rect_intersect(&tile->rect, &item_r, &ov)) continue;

            ui_draw_fill_rect(tile, &ov, ESP_COL_PANEL);
            ui_draw_rect(tile, &item_r, ESP_COL_BORDER);

            // SSID 名称
            ui_draw_string(tile, item_r.x + 8, item_r.y + 4, data->scan_list[i].ssid, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);

            // RSSI 信号数值
            char rssi_str[16];
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm", data->scan_list[i].rssi);
            ui_draw_string(tile, item_r.x + item_r.w - 76, item_r.y + 4, rssi_str, FONT_16, ESP_COL_SIGNAL_OK, ESP_COL_PANEL);
        }
    }

    /* 3. 密码输入弹窗 */
    if (data->connect_modal_open) {
        ui_rect_t modal_r = ui_rect_make(120, 38, 350, 100);
        if (ui_rect_intersect(&tile->rect, &modal_r, &ov)) {
            ui_draw_fill_rect(tile, &ov, ESP_COL_PANEL);
            ui_draw_rect(tile, &modal_r, ESP_COL_BORDER);

            char title[64];
            snprintf(title, sizeof(title), "连接至: %s", data->connecting_ssid);
            ui_draw_string(tile, modal_r.x + 10, modal_r.y + 8, title, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
        }
        ui_input_draw(tile, &data->input_wifi_pass);
        ui_btn_draw(tile, &data->btn_do_connect);
        ui_btn_draw(tile, &data->btn_cancel_connect);
    }
}

/* ============================================================================
 * 8. Tab 1 & Tab 2: 终端与状态界面
 * ============================================================================ */
static void on_term_input_focus(ui_input_t *input, void *user_data)
{
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_btn_send_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data || data->input_cmd.text[0] == '\0') return;

    esp_send_cmd_line(data, data->input_cmd.text);
    ui_input_clear(&data->input_cmd);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
}

static void on_btn_clear_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;
    ui_terminal_clear(&data->terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_keyboard_submit(ui_keyboard_t *kb, const char *text, void *user_data)
{
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (!data) return;

    if (data->connect_modal_open) {
        on_do_connect_click(&data->btn_do_connect, data);
    } else {
        if (text && text[0] != '\0') {
            esp_send_cmd_line(data, text);
        }
        ui_input_clear(&data->input_cmd);
        ui_keyboard_close(kb);
        data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    }
}
/* ============================================================================
 * 蓝牙键盘事件解析与动作分发
 * ============================================================================ */
static void esp_handle_keyboard_line(esp_app_data_t *data, const char *line)
{
    if (!data || !line) return;

    // 1. 按键全部释放
    if (strncmp(line, "#KB:U", 5) == 0) {
        return;
    }

    // 2. 连接与断开状态通知
    if (strstr(line, "#KB:CONN_OK")) {
        ui_toast_show(&data->toast, "蓝牙键盘已就绪", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
        return;
    }
    if (strstr(line, "#KB:DISCONN")) {
        ui_toast_show(&data->toast, "蓝牙键盘已断开", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time()));
        return;
    }

    // 3. 按键按下事件: #KB:D,<ascii>,<hid_code>,<mod>
    int ascii = 0;
    unsigned int hid_code = 0, mod = 0;
    if (sscanf(line, "#KB:D,%d,0x%x,0x%x", &ascii, &hid_code, &mod) >= 2) {
        // 当前焦点输入框：若 Wi-Fi 密码弹窗打开则操作密码框，否则操作终端指令框
        ui_input_t *target_input = (data->connect_modal_open) ? &data->input_wifi_pass : &data->input_cmd;

        // 【回车键 Enter】-> 触发发送命令或确认连接
        if (ascii == '\n' || hid_code == 0x28) {
            if (data->connect_modal_open) {
                on_do_connect_click(&data->btn_do_connect, data);
            } else if (data->tabview.active_tab_idx == 1) {
                on_btn_send_click(&data->btn_send, data);
            }
            return;
        }

        // 【退格键 Backspace】-> 删除最后一个字符
        if (ascii == '\b' || hid_code == 0x2A) {
            size_t len = strlen(target_input->text);
            if (len > 0) {
                target_input->text[len - 1] = '\0';
                data->dirty_rect = rect_union_box(data->dirty_rect, target_input->rect);
            }
            return;
        }

        // 【ESC 键】-> 关闭连接弹窗或软键盘
        if (hid_code == 0x29) {
            if (data->connect_modal_open) {
                on_cancel_connect_click(&data->btn_cancel_connect, data);
            }
            if (data->keyboard.is_open) {
                ui_keyboard_close(&data->keyboard);
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            }
            return;
        }

        // 【方向键 LEFT / RIGHT】-> 无弹窗时直接横向切换 Tab 页面
        if (hid_code == 0x50 && !data->connect_modal_open) { // 方向左键
            if (data->tabview.active_tab_idx > 0) {
                data->tabview.active_tab_idx--;
                data->last_tab_idx = data->tabview.active_tab_idx;
                if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            }
            return;
        }
        if (hid_code == 0x4F && !data->connect_modal_open) { // 方向右键
            if (data->tabview.active_tab_idx < ESP_TAB_COUNT - 1) {
                data->tabview.active_tab_idx++;
                data->last_tab_idx = data->tabview.active_tab_idx;
                if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
            }
            return;
        }

        // 【可见字符打字】ASCII 32~126: a-z, A-Z, 0-9, 符号, 空格
        if (ascii >= 32 && ascii <= 126) {
            size_t len = strlen(target_input->text);
            if (len + 1 < sizeof(target_input->text)) {
                target_input->text[len] = (char)ascii;
                target_input->text[len + 1] = '\0';
                data->dirty_rect = rect_union_box(data->dirty_rect, target_input->rect);
            }
            return;
        }
    }
}
static void draw_status_page(ui_tile_t *tile, const esp_app_data_t *data)
{
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &data->status_card, &ov)) {
        ui_draw_fill_rect(tile, &ov, ESP_COL_PANEL);
        ui_draw_rect(tile, &data->status_card, ESP_COL_BORDER);

        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 12, "ESP 硬件与网络状态", FONT_20, ESP_COL_TEXT, ESP_COL_PANEL);

        char buf[64];
        snprintf(buf, sizeof(buf), "通信接口: UART0 (GP%d/GP%d) @ %lu", ESP_TX, ESP_RX, (unsigned long)ESP_BAUD);
        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 46, buf, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);

        snprintf(buf, sizeof(buf), "AP: %s | STA: %s | Web: %s",
                 data->ap_enabled ? "ON" : "OFF",
                 data->sta_enabled ? "ON" : "OFF",
                 data->web_enabled ? "ON" : "OFF");
        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 74, buf, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);

        snprintf(buf, sizeof(buf), "当前 IP: %s", data->ip_addr[0] ? data->ip_addr : "0.0.0.0");
        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 102, buf, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);

        snprintf(buf, sizeof(buf), "RX 吞吐: %lu 字节", (unsigned long)data->rx_count);
        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 130, buf, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);

        snprintf(buf, sizeof(buf), "TX 吞吐: %lu 字节", (unsigned long)data->tx_count);
        ui_draw_string(tile, data->status_card.x + 14, data->status_card.y + 158, buf, FONT_16, ESP_COL_TEXT, ESP_COL_PANEL);
    }
}

/* ============================================================================
 * 9. app_t 生命周期与触控调度
 * ============================================================================ */
static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data)
{
    (void)old_tab; (void)new_tab;
    esp_app_data_t *data = (esp_app_data_t *)user_data;
    if (data) {
        if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
        data->connect_modal_open = false;
        data->dirty_rect = tv->content_rect;
    }
}

static void app_esp_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void app_esp_enter(app_t *self)
{
    esp_app_data_t *data = (esp_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(esp_app_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);

    // 1. 初始化底层硬件串口
    fun_esp_init();

    // 2. 侧边栏初始化
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32, 110, 48, s_esp_tabs, ESP_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // 3. Tab 0: 服务控制卡片 (高度扩展以容纳 AP 专属信息展示)
    data->service_card = ui_rect_make(120, 38, 350, 102);
    ui_btn_init(&data->btn_refresh_ip, 400, 42, 60, 26, "刷新", on_refresh_ip_click);
    data->btn_refresh_ip.user_data = data; data->btn_refresh_ip.font_size = FONT_16; data->btn_refresh_ip.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_refresh_ip, ESP_COL_PANEL, ESP_COL_BORDER, ESP_COL_TEXT);

    ui_switch_init(&data->sw_ap, 14, 72, 50, 26, false, on_ap_switch_change);
    data->sw_ap.rect = ui_rect_make(data->service_card.x + 14, data->service_card.y + 70, 52, 26);
    data->sw_ap.user_data = data;

    ui_switch_init(&data->sw_sta, 130, 72, 50, 26, false, on_sta_switch_change);
    data->sw_sta.rect = ui_rect_make(data->service_card.x + 130, data->service_card.y + 70, 52, 26);
    data->sw_sta.user_data = data;

    ui_switch_init(&data->sw_web, 246, 72, 50, 26, false, on_web_switch_change);
    data->sw_web.rect = ui_rect_make(data->service_card.x + 246, data->service_card.y + 70, 52, 26);
    data->sw_web.user_data = data;

    // 4. Tab 0: Wi-Fi 扫描列表与弹窗
    data->scan_card = ui_rect_make(120, 146, 350, 162);
    ui_btn_init(&data->btn_scan_wifi, 386, 150, 76, 26, "扫描热点", on_scan_wifi_click);
    data->btn_scan_wifi.user_data = data; data->btn_scan_wifi.font_size = FONT_16; data->btn_scan_wifi.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_scan_wifi, ESP_COL_PANEL, ESP_COL_BORDER, ESP_COL_TEXT);

    ui_input_init(&data->input_wifi_pass, 130, 68, 200, 32, "输入 Wi-Fi 密码", on_pass_input_focus);
    data->input_wifi_pass.user_data = data; data->input_wifi_pass.color_bg = ESP_COL_PANEL; data->input_wifi_pass.color_text = ESP_COL_TEXT;

    ui_btn_init(&data->btn_do_connect, 336, 68, 60, 32, "连接", on_do_connect_click);
    data->btn_do_connect.user_data = data; data->btn_do_connect.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_do_connect, ESP_COL_ACCENT, ESP_COL_ACCENT_HOVER, 0xFFFF);

    ui_btn_init(&data->btn_cancel_connect, 402, 68, 60, 32, "取消", on_cancel_connect_click);
    data->btn_cancel_connect.user_data = data; data->btn_cancel_connect.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_cancel_connect, ESP_COL_PANEL, ESP_COL_BORDER, ESP_COL_TEXT);

    // 5. Tab 1: 终端交互组件
    ui_terminal_init(&data->terminal, ESP_TERM_X, ESP_TERM_Y, ESP_TERM_W, ESP_TERM_H);
    data->terminal.color_bg        = 0x10A2;
    data->terminal.color_border    = 0x31A6;
    data->terminal.color_text_def  = 0xDEFB;
    data->terminal.color_scrollbar = 0x05DF;
    ui_terminal_write_line(&data->terminal, "ESP 交互终端就绪 [921600]", 0x5E7F);

    ui_input_init(&data->input_cmd, ESP_INPUT_X, ESP_INPUT_Y, ESP_INPUT_W, ESP_INPUT_H, "输入串口指令", on_term_input_focus);
    data->input_cmd.user_data = data; data->input_cmd.color_bg = ESP_COL_PANEL; data->input_cmd.color_border = ESP_COL_BORDER; data->input_cmd.color_text = ESP_COL_TEXT;

    ui_btn_init(&data->btn_send, ESP_SEND_X, ESP_SEND_Y, ESP_SEND_W, ESP_SEND_H, "发送", on_btn_send_click);
    data->btn_send.user_data = data; data->btn_send.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_send, ESP_COL_ACCENT, ESP_COL_ACCENT_HOVER, 0xFFFF);

    ui_btn_init(&data->btn_clear, ESP_CLEAR_X, ESP_CLEAR_Y, ESP_CLEAR_W, ESP_CLEAR_H, "清屏", on_btn_clear_click);
    data->btn_clear.user_data = data; data->btn_clear.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_clear, ESP_COL_PANEL, ESP_COL_BORDER, ESP_COL_TEXT);

    ui_keyboard_init(&data->keyboard, on_keyboard_submit);
    data->keyboard.user_data = data;
    ui_input_bind_btn(&data->input_cmd, &data->btn_send);
    ui_input_bind_btn(&data->input_wifi_pass, &data->btn_do_connect);
    // 6. Tab 2: 状态展示
    data->status_card = ui_rect_make(130, 48, 330, 220);
    ui_toast_init(&data->toast);

    // 7. 启动 Core 1
    data->head = 0; data->tail = 0;
    data->core1_run = true;
    multicore_reset_core1();
    multicore_launch_core1(esp_core1_worker);
}

static void app_esp_exit(app_t *self)
{
    esp_app_data_t *data = (esp_app_data_t *)self->user_data;
    if (data) {
        data->core1_run = false;
        multicore_reset_core1();
    }
    self->user_data = NULL;
}

static void app_esp_update(app_t *self, uint32_t now_tick)
{
    esp_app_data_t *data = (esp_app_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }

    bool has_new = false;
    while (data->tail != data->head) {
        esp_msg_t *msg = &data->queue[data->tail];
        if (strncmp(msg->text, "#KB:", 4) == 0) {
            esp_handle_keyboard_line(data, msg->text);
        } else {
            ui_terminal_write_line(&data->terminal, msg->text, msg->color);
            esp_sniff_protocol_line(data, msg->text);
            has_new = true;
        }
        data->tail = (uint8_t)((data->tail + 1) % ESP_QUEUE_SIZE);
    }
    if (has_new && data->tabview.active_tab_idx == 1) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    }

    if (data->tabview.active_tab_idx == 2 && data->rx_count != data->last_rx_count) {
        data->last_rx_count = data->rx_count;
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_card);
    }
}

static bool app_esp_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    esp_app_data_t *data = (esp_app_data_t *)self->user_data;
    if (!data) return false;

    if (data->keyboard.is_open) {
        bool handled = ui_keyboard_process_touch(&data->keyboard, touching, tx, ty, now_tick);
        if (handled || !data->keyboard.is_open) {
            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        }
        return true;
    }

    if (ui_tabview_process_touch(&data->tabview, touching, tx, ty, now_tick)) {
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
        if (data->connect_modal_open) {
            if (ui_input_process_touch(&data->input_wifi_pass, touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_do_connect, touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_cancel_connect, touching, tx, ty, now_tick)) handled = true;
            return true;
        }

        if (ui_btn_process_touch(&data->btn_refresh_ip, touching, tx, ty, now_tick)) handled = true;
        if (ui_switch_process_touch(&data->sw_ap, touching, tx, ty, now_tick)) handled = true;
        if (ui_switch_process_touch(&data->sw_sta, touching, tx, ty, now_tick)) handled = true;
        if (ui_switch_process_touch(&data->sw_web, touching, tx, ty, now_tick)) handled = true;
        if (ui_btn_process_touch(&data->btn_scan_wifi, touching, tx, ty, now_tick)) handled = true;

        if (!touching && data->scan_count > 0) {
            int16_t item_y_base = data->scan_card.y + 34;
            for (uint8_t i = 0; i < data->scan_count && i < MAX_WIFI_SCAN; i++) {
                int16_t iy = item_y_base + i * 25;
                ui_rect_t item_r = ui_rect_make(data->scan_card.x + 8, iy, data->scan_card.w - 16, 23);
                if (ui_rect_contains_point(&item_r, tx, ty)) {
                    strncpy(data->connecting_ssid, data->scan_list[i].ssid, sizeof(data->connecting_ssid) - 1);
                    data->connect_modal_open = true;
                    ui_input_clear(&data->input_wifi_pass);
                    data->keyboard.mode = UI_KB_MODE_LOWER;
                    ui_keyboard_open(&data->keyboard, &data->input_wifi_pass);
                    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                    handled = true;
                    break;
                }
            }
        }
        break;

    case 1:
        if (ui_terminal_process_touch(&data->terminal, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
            handled = true;
        }
        if (ui_input_process_touch(&data->input_cmd, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->btn_send, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_send.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->btn_clear, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_clear.rect);
            handled = true;
        }
        break;

    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_esp_draw(app_t *self, ui_tile_t *tile)
{
    esp_app_data_t *data = (esp_app_data_t *)self->user_data;
    if (!data) return;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, ESP_COL_CANVAS);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &ov)) {
        switch (data->tabview.active_tab_idx) {
        case 0:
            draw_wifi_management_page(tile, data);
            break;
        case 1:
            ui_terminal_draw(tile, &data->terminal);
            ui_input_draw(tile, &data->input_cmd);
            ui_btn_draw(tile, &data->btn_send);
            ui_btn_draw(tile, &data->btn_clear);
            break;
        case 2:
            draw_status_page(tile, data);
            break;
        default:
            break;
        }
    }

    if (data->keyboard.is_open) {
        ui_keyboard_draw(tile, &data->keyboard);
    }
    ui_toast_draw(tile, &data->toast);
}

app_t *app_esp_get_instance(void)
{
    s_app_esp.id            = APP_ID_CUSTOM_0 + 3;
    s_app_esp.name          = "ESP调试";
    s_app_esp.init          = app_esp_init;
    s_app_esp.enter         = app_esp_enter;
    s_app_esp.exit          = app_esp_exit;
    s_app_esp.update        = app_esp_update;
    s_app_esp.process_touch = app_esp_process_touch;
    s_app_esp.draw          = app_esp_draw;
    return &s_app_esp;
}