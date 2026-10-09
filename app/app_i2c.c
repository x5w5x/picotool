#include "app_i2c.h"
#include "app_shared_ram.h"
#include "fun_i2c.h"

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

#define I2C_TAB_COUNT           2
static const char *s_i2c_tabs[] = { "总线扫描", "发包调试" };

#define I2C_COL_CANVAS          0xFFFF
#define I2C_COL_PANEL           0xDEFB
#define I2C_COL_BORDER          0xCE59
#define I2C_COL_TEXT            0x4208
#define I2C_COL_TEXT_MUTED      0x8410
#define I2C_COL_ACCENT          0x03DF
#define I2C_COL_OK              0x03E0
#define I2C_COL_FAIL            0xF800

typedef struct {
    ui_rect_t       dirty_rect;
    ui_tabview_t    tabview;
    uint8_t         last_tab_idx;

    ui_keyboard_t   keyboard;
    ui_toast_t      toast;

    // Tab 0: 扫描页控件
    ui_btn_t        btn_scan;
    ui_btn_t        btn_toggle_baud;
    ui_btn_t        btn_goto_debug;
    bool            scanned_map[128];
    int             found_count;
    int8_t          selected_addr;

    // Tab 1: 通用发包与寄存器调试
    ui_input_t      input_target_addr;
    ui_input_t      input_hex_cmd;
    ui_btn_t        btn_send_hex;

    ui_input_t      input_reg_addr;
    ui_input_t      input_reg_val;
    ui_btn_t        btn_read_reg;
    ui_btn_t        btn_write_reg;

    char            log_msg[64];
    uint16_t        log_color;
} i2c_app_data_t;

static app_t s_app_i2c;

static inline ui_rect_t rect_union(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= Tab 0 交互回调 ================= */

static void do_bus_scan(i2c_app_data_t *data) {
    memset(data->scanned_map, 0, sizeof(data->scanned_map));
    data->found_count = 0;
    data->selected_addr = -1;

    if (!fun_i2c_bus_is_idle()) {
        snprintf(data->log_msg, sizeof(data->log_msg), "[ERR] SDA或SCL被拉低 (未接上拉电阻)");
        data->log_color = I2C_COL_FAIL;
        ui_toast_show(&data->toast, "总线异常: 引脚被拉低", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
        return;
    }

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (fun_i2c_probe(addr)) {
            data->scanned_map[addr] = true;
            if (data->found_count == 0) {
                data->selected_addr = addr;
                char addr_str[8];
                snprintf(addr_str, sizeof(addr_str), "%02X", addr);
                ui_input_set_text(&data->input_target_addr, addr_str);
            }
            data->found_count++;
        }
    }

    if (data->found_count > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "扫描完成: 发现 %d 个器件", data->found_count);
        ui_toast_show(&data->toast, buf, UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
        snprintf(data->log_msg, sizeof(data->log_msg), "[OK] 发现 %d 个从机设备", data->found_count);
        data->log_color = I2C_COL_OK;
    } else {
        ui_toast_show(&data->toast, "未发现任何 I2C 器件", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time()));
        snprintf(data->log_msg, sizeof(data->log_msg), "扫描结束: 无设备应答");
        data->log_color = I2C_COL_TEXT_MUTED;
    }
}

static void on_btn_scan_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;
    do_bus_scan(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_btn_toggle_baud_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;

    uint32_t baud = (fun_i2c_get_baud() == 100000) ? 400000 : 100000;
    fun_i2c_set_baud(baud);

    data->btn_toggle_baud.text = (baud == 100000) ? "100k" : "400k";
    ui_toast_show(&data->toast, (baud == 100000) ? "速率: 100 kHz" : "速率: 400 kHz", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = data->btn_toggle_baud.rect;
}

static void on_btn_goto_debug_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data || data->selected_addr < 0) return;

    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "%02X", (uint8_t)data->selected_addr);
    ui_input_set_text(&data->input_target_addr, addr_str);

    data->tabview.active_tab_idx = 1;
    data->last_tab_idx = 1;
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

/* ================= Tab 1 交互回调 ================= */

static void on_btn_send_hex_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;

    uint8_t addr = (uint8_t)strtoul(data->input_target_addr.text, NULL, 16);
    if (addr == 0) {
        ui_toast_show(&data->toast, "请输入有效从机地址", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
        return;
    }

    int ret = fun_i2c_send_hex(addr, data->input_hex_cmd.text);
    if (ret >= 0) {
        snprintf(data->log_msg, sizeof(data->log_msg), "[OK] 发送 %d 字节成功 (ACK)", ret);
        data->log_color = I2C_COL_OK;
        ui_toast_show(&data->toast, "写入成功", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        snprintf(data->log_msg, sizeof(data->log_msg), "[ERR] 写入失败 (NACK/语法错误)");
        data->log_color = I2C_COL_FAIL;
        ui_toast_show(&data->toast, "无应答 NACK", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_btn_read_reg_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;

    uint8_t addr = (uint8_t)strtoul(data->input_target_addr.text, NULL, 16);
    uint8_t reg  = (uint8_t)strtoul(data->input_reg_addr.text, NULL, 16);
    uint8_t val  = 0;

    int ret = fun_i2c_read_reg(addr, reg, &val);
    if (ret >= 0) {
        char val_str[8];
        snprintf(val_str, sizeof(val_str), "%02X", val);
        ui_input_set_text(&data->input_reg_val, val_str);

        // 仅保留十六进制与十进制对照，去除二进制分解
        snprintf(data->log_msg, sizeof(data->log_msg), "[读成功] Reg 0x%02X = 0x%02X (%d)", reg, val, val);
        data->log_color = I2C_COL_OK;
    } else {
        snprintf(data->log_msg, sizeof(data->log_msg), "[读失败] 从机 0x%02X Reg 0x%02X 无响应", addr, reg);
        data->log_color = I2C_COL_FAIL;
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_btn_write_reg_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;

    uint8_t addr = (uint8_t)strtoul(data->input_target_addr.text, NULL, 16);
    uint8_t reg  = (uint8_t)strtoul(data->input_reg_addr.text, NULL, 16);
    uint8_t val  = (uint8_t)strtoul(data->input_reg_val.text, NULL, 16);

    int ret = fun_i2c_write_reg(addr, reg, val);
    if (ret >= 0) {
        snprintf(data->log_msg, sizeof(data->log_msg), "[写成功] Reg 0x%02X = 0x%02X", reg, val);
        data->log_color = I2C_COL_OK;
        ui_toast_show(&data->toast, "写入成功", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        snprintf(data->log_msg, sizeof(data->log_msg), "[写失败] 写入寄存器无应答");
        data->log_color = I2C_COL_FAIL;
        ui_toast_show(&data->toast, "写入失败", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_input_focus(ui_input_t *input, void *user_data) {
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_keyboard_submit(ui_keyboard_t *kb, const char *text, void *user_data) {
    (void)text;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_close(kb);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    i2c_app_data_t *data = (i2c_app_data_t *)user_data;
    if (data) {
        if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
        data->dirty_rect = tv->content_rect;
    }
}

/* ================= 绘制页面 ================= */

static void draw_tab0_scanner(ui_tile_t *tile, i2c_app_data_t *data) {
    ui_rect_t ov;

    ui_btn_draw(tile, &data->btn_scan);
    ui_btn_draw(tile, &data->btn_toggle_baud);

    char stat_str[32];
    snprintf(stat_str, sizeof(stat_str), "已发现: %d 个器件", data->found_count);
    ui_draw_string(tile, 280, 44, stat_str, FONT_16, I2C_COL_TEXT, I2C_COL_CANVAS);

    int16_t mx = 122, my = 72;
    int16_t cw = 21, ch = 20;

    ui_rect_t r_matrix = ui_rect_make(mx - 2, my - 2, 16 * cw + 4, 8 * ch + 4);
    if (ui_rect_intersect(&tile->rect, &r_matrix, &ov)) {
        ui_draw_fill_rect(tile, &ov, I2C_COL_PANEL);
        ui_draw_rect(tile, &r_matrix, I2C_COL_BORDER);
    }

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 16; col++) {
            uint8_t addr = (uint8_t)(row * 16 + col);
            int16_t cx = mx + col * cw;
            int16_t cy = my + row * ch;
            ui_rect_t r_cell = ui_rect_make(cx, cy, cw - 1, ch - 1);

            if (!ui_rect_intersect(&tile->rect, &r_cell, &ov)) continue;

            if (addr < 0x08 || addr > 0x77) {
                ui_draw_fill_rect(tile, &ov, I2C_COL_PANEL);
                ui_draw_string(tile, cx + 7, cy + 3, ".", FONT_16, I2C_COL_TEXT_MUTED, I2C_COL_PANEL);
            } else if (data->scanned_map[addr]) {
                bool is_sel = (data->selected_addr == addr);
                uint16_t bg = is_sel ? I2C_COL_ACCENT : I2C_COL_OK;
                ui_draw_fill_rect(tile, &ov, bg);

                char hx[4];
                snprintf(hx, sizeof(hx), "%02X", addr);
                ui_draw_string(tile, cx + 2, cy + 2, hx, FONT_16, 0xFFFF, bg);
            } else {
                ui_draw_fill_rect(tile, &ov, 0xFFFF);
                ui_draw_string(tile, cx + 4, cy + 2, "--", FONT_16, I2C_COL_TEXT_MUTED, 0xFFFF);
            }
        }
    }

    ui_rect_t r_card = ui_rect_make(120, 240, 350, 68);
    if (ui_rect_intersect(&tile->rect, &r_card, &ov)) {
        ui_draw_fill_rect(tile, &ov, I2C_COL_PANEL);
        ui_draw_rect(tile, &r_card, I2C_COL_BORDER);

        if (data->selected_addr >= 0) {
            char desc[64];
            snprintf(desc, sizeof(desc), "选中: 0x%02X (%s)",
                     (uint8_t)data->selected_addr,
                     fun_i2c_lookup_name((uint8_t)data->selected_addr));
            ui_draw_string(tile, r_card.x + 10, r_card.y + 10, desc, FONT_16, I2C_COL_TEXT, I2C_COL_PANEL);

            char all_devs[64] = "在线: ";
            for (uint8_t a = 0x08; a <= 0x77; a++) {
                if (data->scanned_map[a]) {
                    char item[12];
                    snprintf(item, sizeof(item), "0x%02X ", a);
                    if (strlen(all_devs) + strlen(item) < sizeof(all_devs) - 1) {
                        strcat(all_devs, item);
                    }
                }
            }
            ui_draw_string(tile, r_card.x + 10, r_card.y + 36, all_devs, FONT_16, I2C_COL_OK, I2C_COL_PANEL);
        } else {
            ui_draw_string(tile, r_card.x + 10, r_card.y + 16, "点击上方 [扫描总线] 探测器件", FONT_16, I2C_COL_TEXT, I2C_COL_PANEL);
            ui_draw_string(tile, r_card.x + 10, r_card.y + 38, "引脚: GP4(SDA) / GP5(SCL)", FONT_16, I2C_COL_TEXT_MUTED, I2C_COL_PANEL);
        }
    }

    if (data->selected_addr >= 0) {
        ui_btn_draw(tile, &data->btn_goto_debug);
    }
}

static void draw_tab1_debugger(ui_tile_t *tile, i2c_app_data_t *data) {
    ui_rect_t ov;

    // 1. 从机地址设置
    ui_draw_string(tile, 124, 42, "从机地址 (HEX):", FONT_16, I2C_COL_TEXT, I2C_COL_CANVAS);
    ui_input_draw(tile, &data->input_target_addr);

    // 2. 通用 HEX 指令输入与发送区
    ui_draw_string(tile, 124, 82, "HEX 指令序列:", FONT_16, I2C_COL_TEXT, I2C_COL_CANVAS);
    ui_input_draw(tile, &data->input_hex_cmd);
    ui_btn_draw(tile, &data->btn_send_hex);

    // 3. 寄存器读写交互
    ui_draw_string(tile, 124, 154, "寄存器(Reg):", FONT_16, I2C_COL_TEXT, I2C_COL_CANVAS);
    ui_input_draw(tile, &data->input_reg_addr);

    ui_draw_string(tile, 280, 154, "数值(Val):", FONT_16, I2C_COL_TEXT, I2C_COL_CANVAS);
    ui_input_draw(tile, &data->input_reg_val);

    ui_btn_draw(tile, &data->btn_read_reg);
    ui_btn_draw(tile, &data->btn_write_reg);

    // 4. 通信响应监视区 (单行清晰状态回显)
    ui_rect_t r_log = ui_rect_make(122, 238, 348, 68);
    if (ui_rect_intersect(&tile->rect, &r_log, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0x10A2);
        ui_draw_rect(tile, &r_log, 0x31A6);

        ui_draw_string(tile, r_log.x + 8, r_log.y + 6, "I2C 总线响应状态:", FONT_16, 0x8410, 0x10A2);
        ui_draw_string(tile, r_log.x + 8, r_log.y + 34, data->log_msg[0] ? data->log_msg : "就绪，等待发包...", FONT_16, data->log_color, 0x10A2);
    }
}

/* ================= App 生命周期 ================= */

static void app_i2c_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_i2c_enter(app_t *self) {
    i2c_app_data_t *data = (i2c_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(i2c_app_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->selected_addr = -1;
    data->log_color = 0xDEFB;
    strcpy(data->log_msg, "就绪，点击 [扫描总线] 探测");

    fun_i2c_init(I2C_DEFAULT_BAUD);

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_i2c_tabs, I2C_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0
    ui_btn_init(&data->btn_scan, 122, 38, 72, 28, "扫描总线", on_btn_scan_click);
    data->btn_scan.user_data = data; data->btn_scan.style = UI_BTN_STYLE_ROUND; data->btn_scan.font_size = FONT_16;
    ui_btn_set_color(&data->btn_scan, I2C_COL_ACCENT, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_toggle_baud, 200, 38, 64, 28, "100k", on_btn_toggle_baud_click);
    data->btn_toggle_baud.user_data = data; data->btn_toggle_baud.style = UI_BTN_STYLE_ROUND; data->btn_toggle_baud.font_size = FONT_16;
    ui_btn_set_color(&data->btn_toggle_baud, I2C_COL_PANEL, I2C_COL_BORDER, I2C_COL_TEXT);

    ui_btn_init(&data->btn_goto_debug, 396, 256, 68, 36, "调试 ->", on_btn_goto_debug_click);
    data->btn_goto_debug.user_data = data; data->btn_goto_debug.style = UI_BTN_STYLE_ROUND; data->btn_goto_debug.font_size = FONT_16;
    ui_btn_set_color(&data->btn_goto_debug, I2C_COL_ACCENT, 0x0275, 0xFFFF);

    // Tab 1
    ui_input_init(&data->input_target_addr, 240, 38, 70, 30, "3C", on_input_focus);
    data->input_target_addr.user_data = data; data->input_target_addr.color_bg = I2C_COL_PANEL;
    ui_input_set_text(&data->input_target_addr, "3C");

    ui_input_init(&data->input_hex_cmd, 122, 104, 270, 32, "HEX (如: 00 8D 14 AF A5)", on_input_focus);
    data->input_hex_cmd.user_data = data; data->input_hex_cmd.color_bg = I2C_COL_PANEL;
    ui_input_set_text(&data->input_hex_cmd, "00 8D 14 AF A5");

    ui_btn_init(&data->btn_send_hex, 400, 104, 70, 32, "发送", on_btn_send_hex_click);
    data->btn_send_hex.user_data = data; data->btn_send_hex.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_send_hex, I2C_COL_ACCENT, 0x0275, 0xFFFF);

    ui_input_init(&data->input_reg_addr, 216, 148, 56, 32, "00", on_input_focus);
    data->input_reg_addr.user_data = data; data->input_reg_addr.color_bg = I2C_COL_PANEL;
    ui_input_set_text(&data->input_reg_addr, "75");

    ui_input_init(&data->input_reg_val, 360, 148, 56, 32, "00", on_input_focus);
    data->input_reg_val.user_data = data; data->input_reg_val.color_bg = I2C_COL_PANEL;

    ui_btn_init(&data->btn_read_reg, 216, 190, 70, 32, "读取", on_btn_read_reg_click);
    data->btn_read_reg.user_data = data; data->btn_read_reg.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_read_reg, I2C_COL_PANEL, I2C_COL_BORDER, I2C_COL_TEXT);

    ui_btn_init(&data->btn_write_reg, 346, 190, 70, 32, "写入", on_btn_write_reg_click);
    data->btn_write_reg.user_data = data; data->btn_write_reg.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_write_reg, I2C_COL_ACCENT, 0x0275, 0xFFFF);

    ui_keyboard_init(&data->keyboard, on_keyboard_submit);
    data->keyboard.user_data = data;
    ui_toast_init(&data->toast);
}

static void app_i2c_exit(app_t *self) {
    self->user_data = NULL;
}

static void app_i2c_update(app_t *self, uint32_t now_tick) {
    i2c_app_data_t *data = (i2c_app_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }
}

static bool app_i2c_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    i2c_app_data_t *data = (i2c_app_data_t *)self->user_data;
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
        if (ui_btn_process_touch(&data->btn_scan, touching, tx, ty, now_tick)) return true;
        if (ui_btn_process_touch(&data->btn_toggle_baud, touching, tx, ty, now_tick)) return true;
        if (data->selected_addr >= 0 && ui_btn_process_touch(&data->btn_goto_debug, touching, tx, ty, now_tick)) return true;

        if (!touching) {
            int16_t mx = 122, my = 72;
            int16_t cw = 21, ch = 20;
            if (tx >= mx && tx < mx + 16 * cw && ty >= my && ty < my + 8 * ch) {
                int col = (tx - mx) / cw;
                int row = (ty - my) / ch;
                uint8_t addr = (uint8_t)(row * 16 + col);
                if (addr >= 0x08 && addr <= 0x77 && data->scanned_map[addr]) {
                    data->selected_addr = addr;
                    char addr_str[8];
                    snprintf(addr_str, sizeof(addr_str), "%02X", addr);
                    ui_input_set_text(&data->input_target_addr, addr_str);
                    data->dirty_rect = data->tabview.content_rect;
                    handled = true;
                }
            }
        }
    } else {
        if (ui_input_process_touch(&data->input_target_addr, touching, tx, ty, now_tick)) handled = true;
        if (ui_input_process_touch(&data->input_hex_cmd, touching, tx, ty, now_tick)) handled = true;
        if (ui_btn_process_touch(&data->btn_send_hex, touching, tx, ty, now_tick)) handled = true;

        if (ui_input_process_touch(&data->input_reg_addr, touching, tx, ty, now_tick)) handled = true;
        if (ui_input_process_touch(&data->input_reg_val, touching, tx, ty, now_tick)) handled = true;
        if (ui_btn_process_touch(&data->btn_read_reg, touching, tx, ty, now_tick)) handled = true;
        if (ui_btn_process_touch(&data->btn_write_reg, touching, tx, ty, now_tick)) handled = true;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_i2c_draw(app_t *self, ui_tile_t *tile) {
    i2c_app_data_t *data = (i2c_app_data_t *)self->user_data;
    if (!data) return;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, I2C_COL_CANVAS);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &ov)) {
        if (data->tabview.active_tab_idx == 0) {
            draw_tab0_scanner(tile, data);
        } else {
            draw_tab1_debugger(tile, data);
        }
    }

    if (data->keyboard.is_open) {
        ui_keyboard_draw(tile, &data->keyboard);
    }
    ui_toast_draw(tile, &data->toast);
}

app_t *app_i2c_get_instance(void) {
    s_app_i2c.id            = APP_ID_CUSTOM_0 + 5;
    s_app_i2c.name          = "I2C总线";
    s_app_i2c.init          = app_i2c_init;
    s_app_i2c.enter         = app_i2c_enter;
    s_app_i2c.exit          = app_i2c_exit;
    s_app_i2c.update        = app_i2c_update;
    s_app_i2c.process_touch = app_i2c_process_touch;
    s_app_i2c.draw          = app_i2c_draw;
    return &s_app_i2c;
}