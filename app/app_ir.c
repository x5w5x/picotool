#include "app_ir.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "fun_ir.h"
#include "ui_draw.h"
#include "ui_terminal.h"
#include "ui_btn.h"
#include "ui_modal.h"
#include "ui_toast.h"
#include "lcd.h"
#include "ff.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern app_manager_t g_app_mgr;

#define TERM_X          14
#define TERM_Y          40
#define TERM_W          330
#define TERM_H          260

#define BTN_X           360
#define BTN_W           105
#define BTN_H           36

#define MSG_BUF_MAX     128
#define MAX_IR_FILES    16

typedef struct {
    char name[32];
} ir_file_item_t;

typedef struct {
    ui_rect_t       dirty_rect;
    ui_terminal_t   terminal;

    /* 右侧 5 项垂直控制按钮 */
    ui_btn_t        btn_learn;
    ui_btn_t        btn_send;
    ui_btn_t        btn_play_file;
    ui_btn_t        btn_save;
    ui_btn_t        btn_clear;

    /* 历史波形文件选择模态弹窗 */
    ui_modal_t      modal_file;
    ir_file_item_t  file_list[MAX_IR_FILES];
    uint8_t         file_count;

    ui_toast_t      toast;

    bool            is_learning;
    bool            has_packet;
    ir_packet_t     last_packet;

    /* ⚡ 借用共享 RAM 缓冲 (零全局 BSS 开销) */
    uint16_t        sample_buf[IR_MAX_PULSES];
    ir_packet_t     temp_packet;

    /* ⚡ 移入共享内存的 FatFs 实例，解决扫描层未挂载问题[cite: 7] */
    FATFS           fs;
    bool            fs_mounted;
} ir_app_data_t;

static app_t s_app_ir;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* UTF-8 转 GBK 编码转换器 (保障终端中文正常显示)[cite: 7] */
static void utf8_to_gbk_string(const char *utf8, char *gbk_out, size_t max_out_len) {
    if (!utf8 || !gbk_out || max_out_len == 0) return;

    size_t in_idx = 0;
    size_t out_idx = 0;

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

static void term_write_utf8(ui_terminal_t *term, const char *utf8_text, uint16_t color) {
    char gbk_buf[MSG_BUF_MAX];
    utf8_to_gbk_string(utf8_text, gbk_buf, sizeof(gbk_buf));
    ui_terminal_write_line(term, gbk_buf, color);
}

/* 扩展名校验器 (支持大小写)[cite: 7] */
static bool check_ir_ext(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 3;
    return (ext[0] == '.' &&
           (ext[1] == 'i' || ext[1] == 'I') &&
           (ext[2] == 'r' || ext[2] == 'R'));
}

/* ================= 目录扫描与模态弹窗回调 ================= */

static void scan_ir_files(ir_app_data_t *data) {
    data->file_count = 0;
    if (!data->fs_mounted) {
        term_write_utf8(&data->terminal, ">> [错误] TF卡未挂载，无法扫描", 0xF800);
        return;
    }

    DIR dir;
    FILINFO fno;

    // 1. 优先扫描 0:/ir
    if (f_opendir(&dir, "0:/ir") == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            if (data->file_count >= MAX_IR_FILES) break;
            if (!(fno.fattrib & AM_DIR) && check_ir_ext(fno.fname)) {
                snprintf(data->file_list[data->file_count].name, 32, "ir/%s", fno.fname);
                data->file_count++;
            }
        }
        f_closedir(&dir);
    }

    // 2. 补充扫描根目录 0:/ 提高兼容性
    if (data->file_count < MAX_IR_FILES) {
        if (f_opendir(&dir, "0:") == FR_OK) {
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                if (data->file_count >= MAX_IR_FILES) break;
                if (!(fno.fattrib & AM_DIR) && check_ir_ext(fno.fname)) {
                    strncpy(data->file_list[data->file_count].name, fno.fname, 31);
                    data->file_list[data->file_count].name[31] = '\0';
                    data->file_count++;
                }
            }
            f_closedir(&dir);
        }
    }

    char log_buf[64];
    snprintf(log_buf, sizeof(log_buf), ">> 扫描完成: 找到 %u 个波形文件", data->file_count);
    term_write_utf8(&data->terminal, log_buf, 0x07E0);

    data->modal_file.sv_body.content_h = (data->file_count > 0 ? data->file_count : 1) * 34 + 8;
    data->modal_file.sv_body.scroll_y = 0;
}

static void on_modal_file_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;

    if (data->file_count == 0) {
        ui_draw_string(tile, viewport->x + 20, viewport->y + 30 - scroll_y, "未找到 .ir 格式文件", FONT_16, 0x8410, 0xFFFF);
        return;
    }

    int16_t item_h = 34;
    for (uint8_t i = 0; i < data->file_count; i++) {
        int16_t item_y = viewport->y + i * item_h - scroll_y;
        if (item_y + item_h < tile->rect.y || item_y > tile->rect.y + tile->rect.h) {
            continue;
        }

        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, item_y + 2, viewport->w - 8, item_h - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xDEFB);
            ui_draw_rect(tile, &item_rect, 0xCE59);
        }

        ui_draw_string(tile, item_rect.x + 8, item_rect.y + 6, data->file_list[i].name, FONT_16, 0x4208, 0xDEFB);
        ui_draw_string(tile, item_rect.x + item_rect.w - 56, item_rect.y + 6, "[发射]", FONT_16, 0x03DF, 0xDEFB);
    }
}

static bool on_modal_file_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data) {
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data || !modal) return false;

    ui_rect_t vr = modal->sv_body.view_rect;
    bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) && ty >= vr.y && ty < (vr.y + vr.h));

    static bool    s_was_touching = false;
    static int16_t s_down_idx = -1;
    static int16_t s_down_y = 0;
    static int16_t s_down_scroll_y = 0;

    int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
    int idx = (in_viewport && rel_y >= 0) ? (rel_y / 34) : -1;

    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_down_idx = idx;
        s_down_y = ty;
        s_down_scroll_y = modal->sv_body.scroll_y;
        return false;
    }

    if (!is_touching && s_was_touching) {
        s_was_touching = false;
        int16_t dy = ty - s_down_y;
        int16_t dscroll = modal->sv_body.scroll_y - s_down_scroll_y;

        if (dy < -6 || dy > 6 || dscroll != 0 || modal->sv_body.is_dragged) {
            s_down_idx = -1;
            return false;
        }

        if (s_down_idx >= 0 && s_down_idx == idx && s_down_idx < data->file_count) {
            char path[64];
            snprintf(path, sizeof(path), "0:/%s", data->file_list[s_down_idx].name);
            s_down_idx = -1;

            if (fun_ir_load_file(path, &data->last_packet)) {
                data->has_packet = true;
                fun_ir_tx_send(&data->last_packet);

                char log_msg[64];
                snprintf(log_msg, sizeof(log_msg), ">> 选码重放: %s (%u脉冲)",
                         data->file_list[idx].name, data->last_packet.pulse_count);
                term_write_utf8(&data->terminal, log_msg, 0x07E0);
                ui_toast_show(&data->toast, "波形重放发射完成!", UI_TOAST_SUCCESS, now_tick);
            } else {
                term_write_utf8(&data->terminal, ">> [错误] 无法加载该波形文件", 0xF800);
                ui_toast_show(&data->toast, "波形文件损坏或为空", UI_TOAST_ERROR, now_tick);
            }

            ui_modal_hide(modal);
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
            return true;
        }
        s_down_idx = -1;
    }
    return false;
}

/* ================= 交互回调 ================= */

static void on_learn_click(ui_btn_t *btn, void *user_data) {
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;

    data->is_learning = !data->is_learning;
    if (data->is_learning) {
        fun_ir_rx_start(data->sample_buf, &data->temp_packet);
        btn->text = "停止学习";
        ui_btn_set_color(btn, 0xF800, 0x8000, 0xFFFF);
        term_write_utf8(&data->terminal, ">> Core1 监听中: 对准接收头按遥控器...", 0xFDA0);
    } else {
        fun_ir_rx_stop();
        btn->text = "开始学习";
        ui_btn_set_color(btn, 0x03DF, 0x0275, 0xFFFF);
        term_write_utf8(&data->terminal, ">> 学习已停止", 0x8410);
    }
    data->dirty_rect = rect_union_box(data->dirty_rect, btn->rect);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_send_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;

    if (!data->has_packet) {
        ui_toast_show(&data->toast, "请先捕获或选择信号", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
        return;
    }

    term_write_utf8(&data->terminal, ">> 正在从 GP5 发射 38kHz 调制载波...", 0x07FF);
    fun_ir_tx_send(&data->last_packet);
    term_write_utf8(&data->terminal, ">> 当前信号重放发射完成!", 0x07E0);

    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_play_file_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;

    scan_ir_files(data);
    ui_modal_show(&data->modal_file, "选择红外波形 (.ir)", data);
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

static void on_save_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;

    if (!data->has_packet) {
        ui_toast_show(&data->toast, "请先捕获信号", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
        return;
    }

    char saved_path[48];
    if (fun_ir_save_auto(&data->last_packet, saved_path, sizeof(saved_path))) {
        char msg[64];
        snprintf(msg, sizeof(msg), ">> 已保存: %s", saved_path);
        term_write_utf8(&data->terminal, msg, 0x07E0);
        ui_toast_show(&data->toast, "波形保存成功!", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        term_write_utf8(&data->terminal, ">> [错误] TF卡未挂载或写入失败", 0xF800);
        ui_toast_show(&data->toast, "TF卡写入失败", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }

    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
}

static void on_clear_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    ir_app_data_t *data = (ir_app_data_t *)user_data;
    if (!data) return;
    ui_terminal_clear(&data->terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

/* ================= 6 项标准生命周期 ================= */

static void app_ir_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_ir_enter(app_t *self) {
    ir_app_data_t *data = (ir_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(*data));
    self->user_data = data;

    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    g_app_mgr.needs_full_redraw = true;

    // ⚡ 借用共享 RAM 挂载 TF 卡，解决扫描层未挂载问题[cite: 7]
    if (f_mount(&data->fs, "0:", 1) == FR_OK) {
        data->fs_mounted = true;
    } else {
        data->fs_mounted = false;
    }

    fun_ir_init(IR_DEFAULT_RX_PIN, IR_DEFAULT_TX_PIN);

    ui_terminal_init(&data->terminal, TERM_X, TERM_Y, TERM_W, TERM_H);
    data->terminal.color_bg = 0x10A2;
    term_write_utf8(&data->terminal, "双核红外遥控系统准备就绪", 0x07E0);
    if (!data->fs_mounted) {
        term_write_utf8(&data->terminal, ">> [警告] TF卡未挂载，波形无法读写", 0xF800);
    }

    /* 5 按钮均匀垂直排布 */
    ui_btn_init(&data->btn_learn, BTN_X, TERM_Y + 4, BTN_W, BTN_H, "开始学习", on_learn_click);
    data->btn_learn.user_data = data;
    data->btn_learn.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_learn, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_send, BTN_X, TERM_Y + 52, BTN_W, BTN_H, "重放当前", on_send_click);
    data->btn_send.user_data = data;
    data->btn_send.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_send, 0xDEFB, 0xCE59, 0x4208);

    ui_btn_init(&data->btn_play_file, BTN_X, TERM_Y + 100, BTN_W, BTN_H, "选码重放", on_play_file_click);
    data->btn_play_file.user_data = data;
    data->btn_play_file.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_play_file, 0x05DF, 0x03DF, 0xFFFF);

    ui_btn_init(&data->btn_save, BTN_X, TERM_Y + 148, BTN_W, BTN_H, "保存波形", on_save_click);
    data->btn_save.user_data = data;
    data->btn_save.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_save, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_clear, BTN_X, TERM_Y + 196, BTN_W, BTN_H, "清屏", on_clear_click);
    data->btn_clear.user_data = data;
    data->btn_clear.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_clear, 0xDEFB, 0x630C, 0xFFFF);

    // 初始化 380x240 模态文件选择弹窗
    ui_modal_init(&data->modal_file, LCD_WIDTH, LCD_HEIGHT, 380, 240, 200,
                  "选择红外波形 (.ir)", UI_MODAL_BTN_NONE,
                  on_modal_file_draw_content, on_modal_file_touch_content, NULL);
    data->modal_file.user_data = data;

    ui_toast_init(&data->toast);
}

static void app_ir_exit(app_t *self) {
    ir_app_data_t *data = (ir_app_data_t *)self->user_data;
    if (data) {
        fun_ir_deinit();
        if (data->fs_mounted) {
            f_mount(NULL, "0:", 0);
        }
    }
    self->user_data = NULL;
}

static void app_ir_update(app_t *self, uint32_t now_tick) {
    ir_app_data_t *data = (ir_app_data_t *)self->user_data;
    if (!data) return;

    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }

    if (data->is_learning) {
        ir_packet_t pkt;
        if (fun_ir_rx_poll(&pkt)) {
            memcpy(&data->last_packet, &pkt, sizeof(ir_packet_t));
            data->has_packet = true;

            char log_msg[96];
            if (pkt.protocol == IR_PROTO_NEC && pkt.nec_valid) {
                snprintf(log_msg, sizeof(log_msg), "[NEC] 用户:0x%02X 指令:0x%02X (%u脉冲)",
                         pkt.nec_addr, pkt.nec_cmd, pkt.pulse_count);
                term_write_utf8(&data->terminal, log_msg, 0x07E0);
            } else {
                snprintf(log_msg, sizeof(log_msg), "[RAW] 原始脉宽捕获成功! 共 %u 个脉冲", pkt.pulse_count);
                term_write_utf8(&data->terminal, log_msg, 0x5E7F);
            }

            ui_toast_show(&data->toast, "红外码捕获成功!", UI_TOAST_SUCCESS, now_tick);
            data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
        }
    }
}

static bool app_ir_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    ir_app_data_t *data = (ir_app_data_t *)self->user_data;
    if (!data) return false;

    // 模态弹窗拦截触控
    if (data->modal_file.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->modal_file, touching, tx, ty, now_tick);
        if (need_refresh) data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        return true;
    }

    bool handled = false;
    if (ui_btn_process_touch(&data->btn_learn, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_learn.rect);
        handled = true;
    } else if (ui_btn_process_touch(&data->btn_send, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_send.rect);
        handled = true;
    } else if (ui_btn_process_touch(&data->btn_play_file, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_play_file.rect);
        handled = true;
    } else if (ui_btn_process_touch(&data->btn_save, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_save.rect);
        handled = true;
    } else if (ui_btn_process_touch(&data->btn_clear, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_clear.rect);
        handled = true;
    } else if (ui_terminal_process_touch(&data->terminal, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
        handled = true;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_ir_draw(app_t *self, ui_tile_t *tile) {
    ir_app_data_t *data = (ir_app_data_t *)self->user_data;
    if (!data) return;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xFFFF);
    }

    ui_terminal_draw(tile, &data->terminal);
    ui_btn_draw(tile, &data->btn_learn);
    ui_btn_draw(tile, &data->btn_send);
    ui_btn_draw(tile, &data->btn_play_file);
    ui_btn_draw(tile, &data->btn_save);
    ui_btn_draw(tile, &data->btn_clear);

    ui_modal_draw(tile, &data->modal_file);
    ui_toast_draw(tile, &data->toast);

    if (tile->rect.y + tile->rect.h >= LCD_HEIGHT) {
        data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    }
}

app_t *app_ir_get_instance(void) {
    s_app_ir.id            = APP_ID_CUSTOM_0 + 5;
    s_app_ir.name          = "红外遥控";
    s_app_ir.init          = app_ir_init;
    s_app_ir.enter         = app_ir_enter;
    s_app_ir.exit          = app_ir_exit;
    s_app_ir.update        = app_ir_update;
    s_app_ir.process_touch = app_ir_process_touch;
    s_app_ir.draw          = app_ir_draw;
    return &s_app_ir;
}