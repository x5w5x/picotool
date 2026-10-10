#include "app_spi.h"
#include "app_shared_ram.h"
#include "fun_spi.h"

#include "ui_draw.h"
#include "ui_tabview.h"
#include "ui_btn.h"
#include "ui_input.h"
#include "ui_keyboard.h"
#include "ui_toast.h"
#include "lcd.h"

#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPI_TAB_COUNT           2
static const char *s_spi_tabs[] = { "Flash诊断", "收发调试" };

#define SPI_COL_CANVAS          0xFFFF
#define SPI_COL_PANEL           0xDEFB
#define SPI_COL_BORDER          0xCE59
#define SPI_COL_TEXT            0x4208
#define SPI_COL_TEXT_MUTED      0x8410
#define SPI_COL_ACCENT          0x03DF
#define SPI_COL_OK              0x03E0
#define SPI_COL_FAIL            0xF800
#define SPI_COL_TERM_BG         0x10A2
#define SPI_COL_TERM_BORDER     0x31A6

typedef struct {
    ui_rect_t           dirty_rect;
    ui_tabview_t        tabview;
    uint8_t             last_tab_idx;

    ui_keyboard_t       keyboard;
    ui_toast_t          toast;

    // Tab 0: Flash 诊断控件与数据
    ui_btn_t            btn_flash_probe;
    ui_btn_t            btn_flash_dump;
    spi_flash_info_t    flash_info;
    uint8_t             dump_buf[32]; // 展示 32 字节 (4行 x 8字节)
    bool                has_dump;

    // Tab 1: 通用全双工收发控件
    ui_btn_t            btn_toggle_baud;
    ui_btn_t            btn_toggle_mode;
    ui_btn_t            btn_toggle_cs;
    bool                cs_manual_low;

    ui_input_t          input_hex_tx;
    ui_btn_t            btn_send_hex;

    char                log_msg[64];
    char                log_detail[64];
    uint16_t            log_color;
    uint32_t            tx_seq;       // 发包流水序号
} spi_app_data_t;

static app_t s_app_spi;

static inline ui_rect_t rect_union(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= Tab 0 回调 ================= */

static void on_btn_flash_probe_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    int ret = fun_spi_flash_probe(&data->flash_info);
    if (ret == 0) {
        ui_toast_show(&data->toast, "读取 Flash ID 成功", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        ui_toast_show(&data->toast, "未识别到芯片 (无响应)", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_btn_flash_dump_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    memset(data->dump_buf, 0, sizeof(data->dump_buf));
    int ret = fun_spi_flash_read_block(0x000000, data->dump_buf, sizeof(data->dump_buf));
    if (ret > 0) {
        data->has_dump = true;
        ui_toast_show(&data->toast, "固件首部读取完毕", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        ui_toast_show(&data->toast, "读取失败", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= Tab 1 回调 ================= */

static void on_btn_toggle_baud_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    uint32_t b = fun_spi_get_baud();
    if (b == 10000000) {
        fun_spi_set_baud(20000000);
        data->btn_toggle_baud.text = "20M";
    } else if (b == 20000000) {
        fun_spi_set_baud(100000);
        data->btn_toggle_baud.text = "100k";
    } else if (b == 100000) {
        fun_spi_set_baud(1000000);
        data->btn_toggle_baud.text = "1M";
    } else {
        fun_spi_set_baud(10000000);
        data->btn_toggle_baud.text = "10M";
    }
    data->dirty_rect = data->btn_toggle_baud.rect;
}

static void on_btn_toggle_mode_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    uint8_t m = (fun_spi_get_mode() + 1) % 4;
    fun_spi_set_mode(m);

    static const char *s_mode_texts[] = { "Mod0", "Mod1", "Mod2", "Mod3" };
    data->btn_toggle_mode.text = s_mode_texts[m];
    data->dirty_rect = data->btn_toggle_mode.rect;
}

static void on_btn_toggle_cs_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    data->cs_manual_low = !data->cs_manual_low;
    fun_spi_cs_put(!data->cs_manual_low);

    data->btn_toggle_cs.text = data->cs_manual_low ? "CS:0" : "CS:1";
    ui_btn_set_color(&data->btn_toggle_cs, data->cs_manual_low ? SPI_COL_FAIL : SPI_COL_PANEL,
                     SPI_COL_BORDER, data->cs_manual_low ? 0xFFFF : SPI_COL_TEXT);
    data->dirty_rect = data->btn_toggle_cs.rect;
}

static void on_btn_send_hex_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;

    // 1. 本地解析 HEX 报文字符串
    uint8_t tx_buf[32];
    size_t tx_len = 0;
    const char *p = data->input_hex_tx.text;

    while (*p && tx_len < sizeof(tx_buf)) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *endp = NULL;
        unsigned long val = strtoul(p, &endp, 16);
        if (endp == p) break;
        tx_buf[tx_len++] = (uint8_t)(val & 0xFF);
        p = endp;
    }

    if (tx_len == 0) {
        snprintf(data->log_msg, sizeof(data->log_msg), "[ERR] 传输失败/指令为空");
        data->log_detail[0] = '\0';
        data->log_color = SPI_COL_FAIL;
        data->dirty_rect = data->tabview.content_rect;
        return;
    }

    // 2. 根据手动片选状态决定发送时序 (避免冲掉用户的手动 CS 状态)
    if (!data->cs_manual_low) {
        fun_spi_cs_put(false);
    }

    uint8_t rx_raw[32];
    int len = fun_spi_transfer_raw(tx_buf, rx_raw, tx_len);

    if (!data->cs_manual_low) {
        fun_spi_cs_put(true);
    }

    // 3. 结果回显与动态序列刷新
    if (len > 0) {
        data->tx_seq++;
        snprintf(data->log_msg, sizeof(data->log_msg), "[#%lu OK] 交换 %d 字节", (unsigned long)data->tx_seq, len);

        char rx_hex_str[64] = "RX: ";
        int show_len = (len > 10) ? 10 : len;
        for (int i = 0; i < show_len; i++) {
            char item[8];
            snprintf(item, sizeof(item), "%02X ", rx_raw[i]);
            strcat(rx_hex_str, item);
        }
        if (len > 10) {
            char extra[16];
            snprintf(extra, sizeof(extra), "...(+%d)", len - 10);
            strcat(rx_hex_str, extra);
        }

        strncpy(data->log_detail, rx_hex_str, sizeof(data->log_detail) - 1);
        data->log_color = SPI_COL_OK;
        ui_toast_show(&data->toast, "发包完成", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        snprintf(data->log_msg, sizeof(data->log_msg), "[ERR] 传输无响应");
        data->log_detail[0] = '\0';
        data->log_color = SPI_COL_FAIL;
    }

    data->dirty_rect = data->tabview.content_rect;
}

static void on_input_focus(ui_input_t *input, void *user_data) {
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_keyboard_submit(ui_keyboard_t *kb, const char *text, void *user_data) {
    (void)text;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_close(kb);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    spi_app_data_t *data = (spi_app_data_t *)user_data;
    if (data) {
        if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
        data->dirty_rect = tv->content_rect;
    }
}

/* ================= 绘制页面 ================= */

static void draw_tab0_flash(ui_tile_t *tile, spi_app_data_t *data) {
    ui_rect_t ov;

    ui_btn_draw(tile, &data->btn_flash_probe);
    ui_btn_draw(tile, &data->btn_flash_dump);

    // 状态与信息面板 (单层标准绘制)
    ui_rect_t r_info = ui_rect_make(122, 72, 348, 86);
    if (ui_rect_intersect(&tile->rect, &r_info, &ov)) {
        ui_draw_fill_rect(tile, &ov, SPI_COL_PANEL);
        ui_draw_rect(tile, &r_info, SPI_COL_BORDER);

        if (data->flash_info.detected) {
            char l1[48], l2[48], l3[48];
            snprintf(l1, sizeof(l1), "芯片: %s %s", data->flash_info.manuf_name, data->flash_info.model_name);
            snprintf(l2, sizeof(l2), "JEDEC: %02X %02X %02X | 状态: %02X %02X",
                     data->flash_info.jedec[0], data->flash_info.jedec[1], data->flash_info.jedec[2],
                     data->flash_info.status1, data->flash_info.status2);
            snprintf(l3, sizeof(l3), "UID: %02X%02X%02X%02X%02X%02X%02X%02X",
                     data->flash_info.uid[0], data->flash_info.uid[1], data->flash_info.uid[2], data->flash_info.uid[3],
                     data->flash_info.uid[4], data->flash_info.uid[5], data->flash_info.uid[6], data->flash_info.uid[7]);

            ui_draw_string(tile, r_info.x + 8, r_info.y + 8,  l1, FONT_16, SPI_COL_OK, SPI_COL_PANEL);
            ui_draw_string(tile, r_info.x + 8, r_info.y + 34, l2, FONT_16, SPI_COL_TEXT, SPI_COL_PANEL);
            ui_draw_string(tile, r_info.x + 8, r_info.y + 60, l3, FONT_16, SPI_COL_TEXT_MUTED, SPI_COL_PANEL);
        } else {
            ui_draw_string(tile, r_info.x + 8, r_info.y + 14, "点击上方 [读取 Flash ID] 探测", FONT_16, SPI_COL_TEXT, SPI_COL_PANEL);
            ui_draw_string(tile, r_info.x + 8, r_info.y + 42, "引脚: GP4:RX GP5:CS GP6:CK GP7:TX", FONT_16, SPI_COL_TEXT_MUTED, SPI_COL_PANEL);
        }
    }

    // 扇区 0 HexDump 区域 (标准 8 字节/行)
    ui_rect_t r_dump = ui_rect_make(122, 168, 348, 138);
    if (ui_rect_intersect(&tile->rect, &r_dump, &ov)) {
        ui_draw_fill_rect(tile, &ov, SPI_COL_TERM_BG);
        ui_draw_rect(tile, &r_dump, SPI_COL_TERM_BORDER);

        ui_draw_string(tile, r_dump.x + 8, r_dump.y + 6, "Sector 0 数据预览 (前 32 字节):", FONT_16, 0x8410, SPI_COL_TERM_BG);

        if (data->has_dump) {
            for (int r = 0; r < 4; r++) {
                char line[40];
                int offset = snprintf(line, sizeof(line), "%04X: ", r * 8);
                for (int c = 0; c < 8; c++) {
                    offset += snprintf(line + offset, sizeof(line) - offset, "%02X ", data->dump_buf[r * 8 + c]);
                }
                ui_draw_string(tile, r_dump.x + 8, r_dump.y + 30 + r * 25, line, FONT_16, 0xFFFF, SPI_COL_TERM_BG);
            }
        } else {
            ui_draw_string(tile, r_dump.x + 8, r_dump.y + 48, "点击 [读取扇区0] 预览固件头", FONT_16, SPI_COL_TEXT_MUTED, SPI_COL_TERM_BG);
        }
    }
}

static void draw_tab1_debugger(ui_tile_t *tile, spi_app_data_t *data) {
    ui_rect_t ov;

    ui_btn_draw(tile, &data->btn_toggle_baud);
    ui_btn_draw(tile, &data->btn_toggle_mode);
    ui_btn_draw(tile, &data->btn_toggle_cs);

    ui_draw_string(tile, 320, 44, "模式: 全双工透传", FONT_16, SPI_COL_TEXT_MUTED, SPI_COL_CANVAS);

    // TX 指令输入与发送
    ui_draw_string(tile, 124, 84, "TX 报文序列 (HEX):", FONT_16, SPI_COL_TEXT, SPI_COL_CANVAS);
    ui_input_draw(tile, &data->input_hex_tx);
    ui_btn_draw(tile, &data->btn_send_hex);

    // 终端监视区
    ui_rect_t r_log = ui_rect_make(122, 168, 348, 138);
    if (ui_rect_intersect(&tile->rect, &r_log, &ov)) {
        ui_draw_fill_rect(tile, &ov, SPI_COL_TERM_BG);
        ui_draw_rect(tile, &r_log, SPI_COL_TERM_BORDER);

        ui_draw_string(tile, r_log.x + 8, r_log.y + 8, "全双工响应状态:", FONT_16, 0x8410, SPI_COL_TERM_BG);
        ui_draw_string(tile, r_log.x + 8, r_log.y + 36, data->log_msg[0] ? data->log_msg : "就绪，等待发包...", FONT_16, data->log_color, SPI_COL_TERM_BG);
        ui_draw_string(tile, r_log.x + 8, r_log.y + 68, data->log_detail[0] ? data->log_detail : "RX: --", FONT_16, 0xFFFF, SPI_COL_TERM_BG);
        ui_draw_string(tile, r_log.x + 8, r_log.y + 104, "参考: 9F 00 00 00 / 05 00", FONT_16, 0x5E7F, SPI_COL_TERM_BG);
    }
}

/* ================= App 生命周期 ================= */

static void app_spi_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_spi_enter(app_t *self) {
    spi_app_data_t *data = (spi_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(spi_app_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->log_color  = 0xDEFB;
    data->tx_seq     = 0;
    strcpy(data->log_msg, "就绪，点击 [发送] 发起传输");

    fun_spi_init(0, SPI_DEFAULT_BAUD);

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_spi_tabs, SPI_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0 控件
    ui_btn_init(&data->btn_flash_probe, 122, 36, 110, 28, "读取 Flash ID", on_btn_flash_probe_click);
    data->btn_flash_probe.user_data = data;
    data->btn_flash_probe.style = UI_BTN_STYLE_ROUND;
    data->btn_flash_probe.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flash_probe, SPI_COL_ACCENT, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_flash_dump, 240, 36, 100, 28, "读取扇区0", on_btn_flash_dump_click);
    data->btn_flash_dump.user_data = data;
    data->btn_flash_dump.style = UI_BTN_STYLE_ROUND;
    data->btn_flash_dump.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flash_dump, SPI_COL_OK, 0x02E0, 0xFFFF);

    // Tab 1 控件
    ui_btn_init(&data->btn_toggle_baud, 122, 36, 56, 28, "10M", on_btn_toggle_baud_click);
    data->btn_toggle_baud.user_data = data;
    data->btn_toggle_baud.style = UI_BTN_STYLE_ROUND;
    data->btn_toggle_baud.font_size = FONT_16;
    ui_btn_set_color(&data->btn_toggle_baud, SPI_COL_PANEL, SPI_COL_BORDER, SPI_COL_TEXT);

    ui_btn_init(&data->btn_toggle_mode, 184, 36, 56, 28, "Mod0", on_btn_toggle_mode_click);
    data->btn_toggle_mode.user_data = data;
    data->btn_toggle_mode.style = UI_BTN_STYLE_ROUND;
    data->btn_toggle_mode.font_size = FONT_16;
    ui_btn_set_color(&data->btn_toggle_mode, SPI_COL_PANEL, SPI_COL_BORDER, SPI_COL_TEXT);

    ui_btn_init(&data->btn_toggle_cs, 246, 36, 56, 28, "CS:1", on_btn_toggle_cs_click);
    data->btn_toggle_cs.user_data = data;
    data->btn_toggle_cs.style = UI_BTN_STYLE_ROUND;
    data->btn_toggle_cs.font_size = FONT_16;
    ui_btn_set_color(&data->btn_toggle_cs, SPI_COL_PANEL, SPI_COL_BORDER, SPI_COL_TEXT);

    ui_input_init(&data->input_hex_tx, 122, 110, 260, 32, "HEX (如: 9F 00 00 00)", on_input_focus);
    data->input_hex_tx.user_data = data;
    data->input_hex_tx.color_bg = SPI_COL_PANEL;
    ui_input_set_text(&data->input_hex_tx, "9F 00 00 00");

    ui_btn_init(&data->btn_send_hex, 390, 110, 78, 32, "发送", on_btn_send_hex_click);
    data->btn_send_hex.user_data = data;
    data->btn_send_hex.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_send_hex, SPI_COL_ACCENT, 0x0275, 0xFFFF);

    ui_keyboard_init(&data->keyboard, on_keyboard_submit);
    data->keyboard.user_data = data;
    ui_toast_init(&data->toast);
}

static void app_spi_exit(app_t *self) {
    fun_spi_deinit();
    self->user_data = NULL;
}

static void app_spi_update(app_t *self, uint32_t now_tick) {
    spi_app_data_t *data = (spi_app_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }
}

static bool app_spi_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    spi_app_data_t *data = (spi_app_data_t *)self->user_data;
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

    if (data->tabview.active_tab_idx == 0) {
        if (ui_btn_process_touch(&data->btn_flash_probe, touching, tx, ty, now_tick)) return true;
        if (ui_btn_process_touch(&data->btn_flash_dump, touching, tx, ty, now_tick)) return true;
    } else {
        if (ui_btn_process_touch(&data->btn_toggle_baud, touching, tx, ty, now_tick)) return true;
        if (ui_btn_process_touch(&data->btn_toggle_mode, touching, tx, ty, now_tick)) return true;
        if (ui_btn_process_touch(&data->btn_toggle_cs, touching, tx, ty, now_tick)) return true;
        if (ui_input_process_touch(&data->input_hex_tx, touching, tx, ty, now_tick)) handled = true;
        if (ui_btn_process_touch(&data->btn_send_hex, touching, tx, ty, now_tick)) handled = true;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_spi_draw(app_t *self, ui_tile_t *tile) {
    spi_app_data_t *data = (spi_app_data_t *)self->user_data;
    if (!data) return;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, SPI_COL_CANVAS);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &ov)) {
        if (data->tabview.active_tab_idx == 0) {
            draw_tab0_flash(tile, data);
        } else {
            draw_tab1_debugger(tile, data);
        }
    }

    if (data->keyboard.is_open) {
        ui_keyboard_draw(tile, &data->keyboard);
    }
    ui_toast_draw(tile, &data->toast);
}

app_t *app_spi_get_instance(void) {
    s_app_spi.id            = APP_ID_CUSTOM_0 + 6;
    s_app_spi.name          = "SPI总线";
    s_app_spi.init          = app_spi_init;
    s_app_spi.enter         = app_spi_enter;
    s_app_spi.exit          = app_spi_exit;
    s_app_spi.update        = app_spi_update;
    s_app_spi.process_touch = app_spi_process_touch;
    s_app_spi.draw          = app_spi_draw;
    return &s_app_spi;
}