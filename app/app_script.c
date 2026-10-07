#include "app_script.h"
#include "app_shared_ram.h"
#include "app_loader.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_tabview.h"
#include "ui_terminal.h"
#include "ui_scrollview.h"
#include "ui_input.h"
#include "ui_keyboard.h"
#include "ui_btn.h"
#include "ui_toast.h"
#include "ui_modal.h"
#include "lcd.h"
#include "ff.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "fun_script_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_TAB_COUNT    4
static const char *s_script_tabs[] = { "执行终端", "脚本管理", "代码编辑", "动作向导" };

// Tab 0: 终端界面布局
#define TERM_X              124
#define TERM_Y              38
#define TERM_W              348
#define TERM_H              224

#define INPUT_CMD_X         124
#define INPUT_CMD_Y         268
#define INPUT_CMD_W         190
#define INPUT_CMD_H         36

#define BTN_EXEC_X          318
#define BTN_EXEC_Y          268
#define BTN_EXEC_W          46
#define BTN_EXEC_H          36

#define BTN_RUN_X           368
#define BTN_RUN_Y           268
#define BTN_RUN_W           50
#define BTN_RUN_H           36

#define BTN_CLEAR_X         422
#define BTN_CLEAR_Y         268
#define BTN_CLEAR_W         50
#define BTN_CLEAR_H         36

// Tab 1: 脚本管理界面布局
#define FILE_LIST_X         124
#define FILE_LIST_Y         72
#define FILE_LIST_W         348
#define FILE_LIST_H         232
#define FILE_ITEM_H         38
#define MAX_SCRIPT_FILES    16

// Tab 2: 代码编辑界面布局
#define EDIT_SCROLL_X       124
#define EDIT_SCROLL_Y       72
#define EDIT_SCROLL_W       348
#define EDIT_SCROLL_H       190
#define EDIT_LINE_H         24

#define EDIT_INPUT_X        124
#define EDIT_INPUT_Y        268
#define EDIT_INPUT_W        268
#define EDIT_INPUT_H        36

#define BTN_UPDATE_X        396
#define BTN_UPDATE_Y        268
#define BTN_UPDATE_W        76
#define BTN_UPDATE_H        36

#define SCRIPT_MAX_LINES    64
#define SCRIPT_LINE_LEN     64

// Tab 3: 动作向导界面布局
#define FLOW_SCROLL_X       124
#define FLOW_SCROLL_Y       72
#define FLOW_SCROLL_W       348
#define FLOW_SCROLL_H       232
#define FLOW_CARD_H         44
#define FLOW_MAX_STEPS      16
#define FLOW_MAX_ARGS       2

#define SCRIPT_QUEUE_SIZE   16
#define SCRIPT_MSG_MAX_LEN  128

static const char *k_fallback_script =
    "# 默认硬件自测脚本\n"
    "print \"=== 脚本引擎启动 ===\"\n"
    "pin.mode 4 out\n"
    "$0 = 3\n"
    ":LOOP\n"
    "print \"倒计时次数:\" $0\n"
    "pin.write 4 1\n"
    "delay 150\n"
    "pin.write 4 0\n"
    "delay 150\n"
    "$0 = $0 - 1\n"
    "if $0 > 0 goto LOOP\n"
    "print \"=== 脚本自检完成 ===\"\n";

typedef struct {
    char text[SCRIPT_LINE_LEN];
} script_line_entry_t;

typedef struct {
    char     text[SCRIPT_MSG_MAX_LEN];
    uint16_t color;
} script_log_msg_t;

typedef struct {
    char     name[32];
    uint32_t size;
} script_file_item_t;

typedef struct {
    char prim_name[16];
    char args[FLOW_MAX_ARGS][24];
} flow_card_step_t;

typedef struct {
    flow_card_step_t steps[FLOW_MAX_STEPS];
    uint8_t          step_count;
    uint8_t          repeat_times;
} flow_card_seq_t;

typedef struct {
    ui_rect_t           dirty_rect;
    ui_tabview_t        tabview;
    uint8_t             last_tab_idx;

    // Tab 0 控件
    ui_terminal_t       terminal;
    ui_input_t          input_cmd;
    ui_keyboard_t       keyboard;
    ui_btn_t            btn_exec;
    ui_btn_t            btn_run;
    ui_btn_t            btn_clear;
    ui_toast_t          toast;

    // Tab 1 控件
    ui_btn_t            btn_new_file;
    ui_btn_t            btn_refresh;
    ui_scrollview_t     file_scroll;
    script_file_item_t  file_list[MAX_SCRIPT_FILES];
    uint8_t             file_count;

    // Tab 2 控件
    ui_btn_t            btn_save_file;
    ui_btn_t            btn_insert_line;
    ui_btn_t            btn_delete_line;
    ui_scrollview_t     edit_scroll;
    ui_input_t          input_edit_line;
    ui_btn_t            btn_update_line;

    char                editing_filename[48];
    uint16_t            edit_line_count;
    int16_t             selected_line_idx;
    bool                is_dirty;

    // Tab 3 动作向导控件
    ui_btn_t            btn_flow_add;
    ui_btn_t            btn_flow_loop;
    ui_btn_t            btn_flow_to_code;
    ui_btn_t            btn_flow_run;
    ui_scrollview_t     flow_scroll;
    flow_card_seq_t     flow_seq;

    ui_input_t          input_flow_arg;
    int8_t              target_step_idx;
    uint8_t             target_arg_idx;

    ui_modal_t          modal_prim;
    char                modal_title_gbk[32];

    char                cur_file_path[64];
    script_vm_t         vm;

    script_log_msg_t    log_queue[SCRIPT_QUEUE_SIZE];
    volatile uint8_t    log_head;
    volatile uint8_t    log_tail;

    volatile bool       core1_running;

    FATFS               fs;
    bool                fs_mounted;
} script_app_data_t;

static app_t s_app_script;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 1. 字符编码转换 ================= */
static void utf8_to_gbk_string(const char *utf8, char *gbk_out, size_t max_out_len) {
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

static void gbk_to_utf8_string(const char *gbk, char *utf8_out, size_t max_out) {
    if (!gbk || !utf8_out || max_out == 0) return;
    size_t in = 0, out = 0;

    while (gbk[in] != '\0' && out < max_out - 4) {
        uint8_t c = (uint8_t)gbk[in];
        if (c < 0x80) {
            utf8_out[out++] = (char)c;
            in++;
        } else if (gbk[in + 1] != '\0') {
            uint8_t c2 = (uint8_t)gbk[in + 1];
            WCHAR oem = (WCHAR)((c << 8) | c2);
            WCHAR uni = ff_oem2uni(oem, 936);
            if (uni == 0) {
                utf8_out[out++] = '?';
            } else if (uni < 0x80) {
                utf8_out[out++] = (char)uni;
            } else if (uni < 0x800) {
                utf8_out[out++] = (char)(0xC0 | (uni >> 6));
                utf8_out[out++] = (char)(0x80 | (uni & 0x3F));
            } else {
                utf8_out[out++] = (char)(0xE0 | (uni >> 12));
                utf8_out[out++] = (char)(0x80 | ((uni >> 6) & 0x3F));
                utf8_out[out++] = (char)(0x80 | (uni & 0x3F));
            }
            in += 2;
        } else {
            in++;
        }
    }
    utf8_out[out] = '\0';
}

static bool check_txt_ext(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return (ext[0] == '.' &&
           (ext[1] == 't' || ext[1] == 'T') &&
           (ext[2] == 'x' || ext[2] == 'X') &&
           (ext[3] == 't' || ext[3] == 'T'));
}

/* ================= 2. 卡片与参数缺省逻辑 ================= */
static void flow_step_set_default_args(flow_card_step_t *st, const char *prim_name) {
    if (!st || !prim_name) return;
    strncpy(st->prim_name, prim_name, sizeof(st->prim_name) - 1);
    st->prim_name[sizeof(st->prim_name) - 1] = '\0';
    memset(st->args, 0, sizeof(st->args));

    if (strcmp(prim_name, "pin.write") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "1");
    } else if (strcmp(prim_name, "delay") == 0) {
        strcpy(st->args[0], "200");
    } else if (strcmp(prim_name, "pin.mode") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "out");
    } else if (strcmp(prim_name, "pin.read") == 0) {
        strcpy(st->args[0], "25");
    } else if (strcmp(prim_name, "pwm.set") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "50");
    } else if (strcmp(prim_name, "ir.send") == 0) {
        strcpy(st->args[0], "0:/ir/p.ir");
    } else if (strcmp(prim_name, "print") == 0) {
        strcpy(st->args[0], "\"执行中...\"");
    }
}

static int flow_export_to_script(const flow_card_seq_t *seq, char *out_buf, size_t max_len) {
    if (!seq || !out_buf || max_len == 0) return 0;
    out_buf[0] = '\0';

    snprintf(out_buf, max_len, "# 动作向导生成脚本\n");

    const char *indent = "";
    if (seq->repeat_times > 1) {
        char rep_hdr[32];
        snprintf(rep_hdr, sizeof(rep_hdr), "repeat %u\n", seq->repeat_times);
        strncat(out_buf, rep_hdr, max_len - strlen(out_buf) - 1);
        indent = "    ";
    }

    char line[96];
    for (uint8_t i = 0; i < seq->step_count; i++) {
        const flow_card_step_t *st = &seq->steps[i];
        if (st->prim_name[0] == '\0') continue;

        if (st->args[1][0] != '\0') {
            snprintf(line, sizeof(line), "%s%s %s %s\n", indent, st->prim_name, st->args[0], st->args[1]);
        } else if (st->args[0][0] != '\0') {
            snprintf(line, sizeof(line), "%s%s %s\n", indent, st->prim_name, st->args[0]);
        } else {
            snprintf(line, sizeof(line), "%s%s\n", indent, st->prim_name);
        }
        strncat(out_buf, line, max_len - strlen(out_buf) - 1);
    }

    if (seq->repeat_times > 1) {
        strncat(out_buf, "end\n", max_len - strlen(out_buf) - 1);
    }
    strncat(out_buf, "print \"=== 向导流程执行完毕 ===\"\n", max_len - strlen(out_buf) - 1);
    return (int)strlen(out_buf);
}

/* ================= 3. 弹窗回调绑定 ================= */
static void on_modal_prim_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    (void)user_data;
    uint8_t count = script_prim_get_count();
    int16_t item_h = 36;

    for (uint8_t i = 0; i < count; i++) {
        int16_t iy = viewport->y + i * item_h - scroll_y;
        if (iy + item_h < tile->rect.y || iy > tile->rect.y + tile->rect.h) continue;

        const script_prim_entry_t *entry = script_prim_get_by_index(i);
        if (!entry) continue;

        ui_rect_t r_item = ui_rect_make(viewport->x + 4, iy + 2, viewport->w - 8, item_h - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &r_item, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xFFFF);
            ui_draw_rect(tile, &r_item, 0xCE59);
        }

        ui_draw_string(tile, r_item.x + 8, r_item.y + 8, entry->name, FONT_16, 0x03DF, 0xFFFF);
        if (entry->help) {
            char gbk_help[64];
            utf8_to_gbk_string(entry->help, gbk_help, sizeof(gbk_help));
            ui_draw_string(tile, r_item.x + 106, r_item.y + 8, gbk_help, FONT_16, 0x8410, 0xFFFF);
        }
    }
}

static bool on_modal_prim_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data) {
    (void)now_tick;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data || data->target_step_idx < 0 || data->target_step_idx >= data->flow_seq.step_count) return false;

    static bool s_m_down = false;
    static int16_t s_mx = 0, s_my = 0;

    if (is_touching && !s_m_down) {
        s_m_down = true; s_mx = tx; s_my = ty;
    }
    if (!is_touching && s_m_down) {
        s_m_down = false;
        int16_t dx = tx - s_mx, dy = ty - s_my;
        if (abs(dx) <= 6 && abs(dy) <= 6 && !modal->sv_body.is_dragged) {
            ui_rect_t vr = modal->sv_body.view_rect;
            if (tx >= vr.x && tx <= vr.x + vr.w && ty >= vr.y && ty <= vr.y + vr.h) {
                int idx = (ty - vr.y + modal->sv_body.scroll_y) / 36;
                uint8_t total = script_prim_get_count();
                if (idx >= 0 && idx < total) {
                    const script_prim_entry_t *entry = script_prim_get_by_index(idx);
                    if (entry) {
                        flow_step_set_default_args(&data->flow_seq.steps[data->target_step_idx], entry->name);
                        ui_modal_hide(modal);
                        data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/* ================= 4. 文件加载与异步执行 ================= */
static void scan_script_files(script_app_data_t *data) {
    data->file_count = 0;
    if (!data->fs_mounted) return;

    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, "0:/scripts") != FR_OK) {
        f_mkdir("0:/scripts");
        if (f_opendir(&dir, "0:/scripts") != FR_OK) return;
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (data->file_count >= MAX_SCRIPT_FILES) break;
        if (!(fno.fattrib & AM_DIR) && check_txt_ext(fno.fname)) {
            strncpy(data->file_list[data->file_count].name, fno.fname, sizeof(data->file_list[data->file_count].name) - 1);
            data->file_list[data->file_count].name[sizeof(data->file_list[data->file_count].name) - 1] = '\0';
            data->file_list[data->file_count].size = (uint32_t)fno.fsize;
            data->file_count++;
        }
    }
    f_closedir(&dir);

    data->file_scroll.content_h = (data->file_count > 0 ? data->file_count : 1) * FILE_ITEM_H + 8;
    data->file_scroll.scroll_y = 0;
}

static bool load_script_file_for_run(script_app_data_t *data, const char *filepath) {
    if (!data || !data->fs_mounted) return false;

    FIL f;
    if (f_open(&f, filepath, FA_READ) != FR_OK) return false;

    UINT br = 0;
    char *buf = (char *)app_loader_get_ram_pool();
    f_read(&f, buf, 4096, &br);
    buf[br] = '\0';
    f_close(&f);

    if (br > 0) {
        script_vm_load(&data->vm, buf);
        strncpy(data->cur_file_path, filepath, sizeof(data->cur_file_path) - 1);
        data->cur_file_path[sizeof(data->cur_file_path) - 1] = '\0';
        return true;
    }
    return false;
}

static void editor_load_file(script_app_data_t *data, const char *filepath) {
    strncpy(data->editing_filename, filepath, sizeof(data->editing_filename) - 1);
    data->editing_filename[sizeof(data->editing_filename) - 1] = '\0';
    data->is_dirty = false;
    data->selected_line_idx = 0;
    data->edit_line_count = 0;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    memset(lines, 0, SCRIPT_MAX_LINES * sizeof(script_line_entry_t));

    FIL f;
    if (f_open(&f, filepath, FA_READ) == FR_OK) {
        char cur_utf8[SCRIPT_LINE_LEN * 2];
        int cur_len = 0;
        UINT br = 0;
        char c = 0;

        while (f_read(&f, &c, 1, &br) == FR_OK && br > 0 && data->edit_line_count < SCRIPT_MAX_LINES) {
            if (c == '\r') continue;
            if (c == '\n') {
                cur_utf8[cur_len] = '\0';
                utf8_to_gbk_string(cur_utf8, lines[data->edit_line_count].text, SCRIPT_LINE_LEN);
                data->edit_line_count++;
                cur_len = 0;
            } else {
                if (cur_len < (int)sizeof(cur_utf8) - 1) {
                    cur_utf8[cur_len++] = c;
                }
            }
        }
        if (cur_len > 0 && data->edit_line_count < SCRIPT_MAX_LINES) {
            cur_utf8[cur_len] = '\0';
            utf8_to_gbk_string(cur_utf8, lines[data->edit_line_count].text, SCRIPT_LINE_LEN);
            data->edit_line_count++;
        }
        f_close(&f);
    }

    if (data->edit_line_count == 0) {
        utf8_to_gbk_string("# 新脚本", lines[0].text, SCRIPT_LINE_LEN);
        data->edit_line_count = 1;
    }

    data->edit_scroll.content_h = (data->edit_line_count > 0 ? data->edit_line_count : 1) * EDIT_LINE_H + 8;
    data->edit_scroll.scroll_y = 0;

    ui_input_set_text(&data->input_edit_line, lines[0].text);
}

static bool editor_save_file(script_app_data_t *data) {
    if (!data->fs_mounted || data->editing_filename[0] == '\0') return false;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    FIL f;
    if (f_open(&f, data->editing_filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return false;
    }

    UINT bw;
    char utf8_buf[SCRIPT_LINE_LEN * 2];
    for (uint16_t i = 0; i < data->edit_line_count; i++) {
        gbk_to_utf8_string(lines[i].text, utf8_buf, sizeof(utf8_buf));
        f_write(&f, utf8_buf, strlen(utf8_buf), &bw);
        f_write(&f, "\n", 1, &bw);
    }
    f_close(&f);

    data->is_dirty = false;
    return true;
}

static void script_log_bridge(const char *text, uint16_t color, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    uint8_t next = (data->log_head + 1) % SCRIPT_QUEUE_SIZE;
    if (next != data->log_tail) {
        utf8_to_gbk_string(text, data->log_queue[data->log_head].text, SCRIPT_MSG_MAX_LEN);
        data->log_queue[data->log_head].color = color;
        data->log_head = next;
    }
}

static void core1_worker(void) {
    script_app_data_t *data = (script_app_data_t *)g_static_app_ram;
    data->core1_running = true;
    script_vm_run(&data->vm);
    data->core1_running = false;
    while (true) tight_loop_contents();
}

static void start_script_exec(script_app_data_t *data) {
    if (!data || data->core1_running) return;

    multicore_reset_core1();
    multicore_launch_core1(core1_worker);

    data->btn_run.text = "中止";
    ui_btn_set_color(&data->btn_run, 0xF800, 0x8000, 0xFFFF);

    char toast_msg[64];
    snprintf(toast_msg, sizeof(toast_msg), "正在运行: %s", data->cur_file_path[0] ? data->cur_file_path : "向导任务");
    ui_toast_show(&data->toast, toast_msg, UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
}

static void stop_script_exec(script_app_data_t *data) {
    if (!data || !data->core1_running) return;

    script_vm_abort(&data->vm);
    sleep_ms(5);
    multicore_reset_core1();
    data->core1_running = false;

    data->btn_run.text = "文件";
    ui_btn_set_color(&data->btn_run, 0xDEFB, 0xCE59, 0x4208);
    ui_terminal_write_line(&data->terminal, "[SYS] 脚本运行已强制中止", 0xF800);
    ui_toast_show(&data->toast, "已强制中止", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
}

/* ================= 5. 交互回调 ================= */
static void on_exec_line_submit(script_app_data_t *data) {
    if (!data || data->input_cmd.text[0] == '\0') return;

    char cmd[SCRIPT_LINE_MAX];
    strncpy(cmd, data->input_cmd.text, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    char echo[SCRIPT_LINE_MAX + 4];
    snprintf(echo, sizeof(echo), "> %s", cmd);
    ui_terminal_write_line(&data->terminal, echo, 0x07FF);

    char out_msg[64] = {0};
    script_vm_exec_line(&data->vm, cmd, out_msg, sizeof(out_msg));
    if (out_msg[0] != '\0') {
        char gbk_out[128];
        utf8_to_gbk_string(out_msg, gbk_out, sizeof(gbk_out));
        ui_terminal_write_line(&data->terminal, gbk_out, 0x5E7F);
    }

    while (data->log_tail != data->log_head) {
        script_log_msg_t *msg = &data->log_queue[data->log_tail];
        ui_terminal_write_line(&data->terminal, msg->text, msg->color);
        data->log_tail = (data->log_tail + 1) % SCRIPT_QUEUE_SIZE;
    }

    ui_input_clear(&data->input_cmd);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_btn_exec_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    on_exec_line_submit((script_app_data_t *)user_data);
}

static void on_run_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (!data->core1_running) {
        if (!data->cur_file_path[0]) {
            strcpy(data->cur_file_path, "0:/scripts/run.txt");
        }

        if (load_script_file_for_run(data, data->cur_file_path)) {
            char log_msg[80];
            snprintf(log_msg, sizeof(log_msg), "[SYS] 成功载入: %s", data->cur_file_path);
            ui_terminal_write_line(&data->terminal, log_msg, 0x07FF);
        } else {
            script_vm_load(&data->vm, k_fallback_script);
            ui_terminal_write_line(&data->terminal, "[SYS] 未找到指定脚本，运行内置自测脚本", 0xFD20);
        }

        start_script_exec(data);
        data->dirty_rect = rect_union_box(data->btn_run.rect, data->terminal.rect);
    } else {
        stop_script_exec(data);
        data->dirty_rect = rect_union_box(data->btn_run.rect, data->terminal.rect);
    }
}

static void on_clear_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    ui_terminal_clear(&data->terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_refresh_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    scan_script_files(data);
    ui_toast_show(&data->toast, "脚本列表已刷新", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = data->tabview.content_rect;
}

static void on_new_file_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    char path[48];
    uint8_t idx = 1;
    FILINFO fno;
    while (idx < 100) {
        snprintf(path, sizeof(path), "0:/scripts/new_%02u.txt", idx);
        if (f_stat(path, &fno) == FR_NO_FILE) break;
        idx++;
    }

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    memset(lines, 0, SCRIPT_MAX_LINES * sizeof(script_line_entry_t));
    utf8_to_gbk_string("# 新脚本", lines[0].text, SCRIPT_LINE_LEN);

    strncpy(data->editing_filename, path, sizeof(data->editing_filename) - 1);
    data->editing_filename[sizeof(data->editing_filename) - 1] = '\0';
    data->edit_line_count = 1;
    data->selected_line_idx = 0;
    data->is_dirty = true;

    data->edit_scroll.content_h = EDIT_LINE_H + 8;
    data->edit_scroll.scroll_y = 0;
    ui_input_set_text(&data->input_edit_line, lines[0].text);

    data->tabview.active_tab_idx = 2;
    data->last_tab_idx = 2;
    ui_toast_show(&data->toast, "已新建脚本草稿", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_save_file_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (editor_save_file(data)) {
        ui_toast_show(&data->toast, "脚本已保存至 TF卡", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    } else {
        ui_toast_show(&data->toast, "保存失败! TF卡不可写", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));
    }
    data->dirty_rect = ui_rect_make(120, 38, 360, 30);
}

static void on_update_line_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data || data->selected_line_idx < 0 || data->selected_line_idx >= data->edit_line_count) return;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    memset(lines[data->selected_line_idx].text, 0, SCRIPT_LINE_LEN);
    strncpy(lines[data->selected_line_idx].text, data->input_edit_line.text, SCRIPT_LINE_LEN - 1);
    lines[data->selected_line_idx].text[SCRIPT_LINE_LEN - 1] = '\0';

    data->is_dirty = true;
    ui_toast_show(&data->toast, "行代码已更新", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = data->edit_scroll.view_rect;
    data->dirty_rect = rect_union_box(data->dirty_rect, ui_rect_make(120, 38, 360, 30));
}

static void on_insert_line_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data || data->edit_line_count >= SCRIPT_MAX_LINES) return;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    int16_t insert_pos = data->selected_line_idx + 1;
    if (insert_pos < 0) insert_pos = 0;
    if (insert_pos > data->edit_line_count) insert_pos = data->edit_line_count;

    for (int16_t i = data->edit_line_count; i > insert_pos; i--) {
        lines[i] = lines[i - 1];
    }

    strcpy(lines[insert_pos].text, "");
    data->edit_line_count++;
    data->selected_line_idx = insert_pos;
    data->is_dirty = true;

    data->edit_scroll.content_h = data->edit_line_count * EDIT_LINE_H + 8;
    ui_input_set_text(&data->input_edit_line, "");

    data->dirty_rect = data->tabview.content_rect;
}

static void on_delete_line_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data || data->edit_line_count <= 1 || data->selected_line_idx < 0) return;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
    int16_t del_pos = data->selected_line_idx;

    for (int16_t i = del_pos; i < data->edit_line_count - 1; i++) {
        lines[i] = lines[i + 1];
    }
    data->edit_line_count--;
    if (data->selected_line_idx >= data->edit_line_count) {
        data->selected_line_idx = data->edit_line_count - 1;
    }
    data->is_dirty = true;

    data->edit_scroll.content_h = data->edit_line_count * EDIT_LINE_H + 8;
    ui_input_set_text(&data->input_edit_line, lines[data->selected_line_idx].text);

    data->dirty_rect = data->tabview.content_rect;
}

static void on_input_focus(ui_input_t *input, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    ui_keyboard_open(&data->keyboard, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

/* ================= 6. 向导专属操作回调 (Touch-Up 驱动) ================= */
static void on_flow_add_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    if (data->flow_seq.step_count < FLOW_MAX_STEPS) {
        flow_step_set_default_args(&data->flow_seq.steps[data->flow_seq.step_count], "delay");
        data->flow_seq.step_count++;
        data->flow_scroll.content_h = data->flow_seq.step_count * FLOW_CARD_H + 8;
        data->dirty_rect = data->tabview.content_rect;
    }
}

static void on_flow_loop_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    static const uint8_t s_repeats[] = { 1, 3, 5, 10, 20 };
    static uint8_t s_r_idx = 0;
    s_r_idx = (s_r_idx + 1) % 5;
    data->flow_seq.repeat_times = s_repeats[s_r_idx];
    if (data->flow_seq.repeat_times == 1) data->btn_flow_loop.text = "单次";
    else if (data->flow_seq.repeat_times == 3) data->btn_flow_loop.text = "3次";
    else if (data->flow_seq.repeat_times == 5) data->btn_flow_loop.text = "5次";
    else if (data->flow_seq.repeat_times == 10) data->btn_flow_loop.text = "10次";
    else data->btn_flow_loop.text = "20次";
    data->dirty_rect = data->btn_flow_loop.rect;
}

static void on_flow_to_code_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    char *buf = (char *)app_loader_get_ram_pool();
    flow_export_to_script(&data->flow_seq, buf, 4096);
    f_mkdir("0:/scripts");
    FIL f;
    if (f_open(&f, "0:/scripts/wizard.txt", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        UINT bw;
        f_write(&f, buf, strlen(buf), &bw);
        f_close(&f);
    }
    editor_load_file(data, "0:/scripts/wizard.txt");
    data->tabview.active_tab_idx = 2;
    data->last_tab_idx = 2;
    ui_toast_show(&data->toast, "已转为脚本并进入编辑器", UI_TOAST_SUCCESS, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_flow_run_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    char *buf = (char *)app_loader_get_ram_pool();
    flow_export_to_script(&data->flow_seq, buf, 4096);
    script_vm_load(&data->vm, buf);
    data->tabview.active_tab_idx = 0;
    data->last_tab_idx = 0;
    ui_terminal_write_line(&data->terminal, "--------------------------------", 0x4208);
    ui_terminal_write_line(&data->terminal, "[SYS] 启动向导任务流执行...", 0x07FF);
    start_script_exec(data);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

// 全键盘提交事件
static void on_keyboard_submit(ui_keyboard_t *kb, const char *text, void *user_data) {
    (void)kb;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (data->tabview.active_tab_idx == 0) {
        on_exec_line_submit(data);
    } else if (data->tabview.active_tab_idx == 2) {
        on_update_line_click(&data->btn_update_line, data);
    } else if (data->tabview.active_tab_idx == 3) {
        if (data->target_step_idx >= 0 && data->target_step_idx < data->flow_seq.step_count) {
            flow_card_step_t *st = &data->flow_seq.steps[data->target_step_idx];
            strncpy(st->args[data->target_arg_idx], text ? text : "", sizeof(st->args[0]) - 1);
            st->args[data->target_arg_idx][sizeof(st->args[0]) - 1] = '\0';
            ui_keyboard_close(&data->keyboard);
            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        }
    }
}

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (data) {
        if (data->keyboard.is_open) ui_keyboard_close(&data->keyboard);
        if (data->modal_prim.is_open) ui_modal_hide(&data->modal_prim);

        // 切换 Tab 时复位向导按钮状态，防止悬空按压标志导致二次虚假触发
        data->btn_flow_run.is_pressed = false;
        data->btn_flow_to_code.is_pressed = false;
        data->btn_flow_add.is_pressed = false;
        data->btn_flow_loop.is_pressed = false;

        if (new_tab == 1) {
            scan_script_files(data);
        }
        data->dirty_rect = tv->content_rect;
    }
}

/* ================= 7. 视口光栅化绘制回调 ================= */
static void on_file_scroll_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (data->file_count == 0) {
        ui_draw_string(tile, viewport->x + 20, viewport->y + 30 - scroll_y, "0:/scripts 目录下暂无 .txt 文件", FONT_16, 0x8410, 0xFFFF);
        ui_draw_string(tile, viewport->x + 20, viewport->y + 60 - scroll_y, "点击右上角 [新建] 创建一个脚本", FONT_16, 0x03DF, 0xFFFF);
        return;
    }

    for (uint8_t i = 0; i < data->file_count; i++) {
        int16_t iy = viewport->y + i * FILE_ITEM_H - scroll_y;
        if (iy + FILE_ITEM_H < tile->rect.y || iy > tile->rect.y + tile->rect.h) continue;

        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, iy + 2, viewport->w - 8, FILE_ITEM_H - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xDEFB);
            ui_draw_rect(tile, &item_rect, 0xCE59);
        }

        ui_draw_string(tile, item_rect.x + 8, item_rect.y + 8, data->file_list[i].name, FONT_16, 0x4208, 0xDEFB);

        char sz_str[24];
        snprintf(sz_str, sizeof(sz_str), "%lu B", (unsigned long)data->file_list[i].size);
        ui_draw_string(tile, item_rect.x + item_rect.w - 146, item_rect.y + 8, sz_str, FONT_16, 0x8410, 0xDEFB);

        ui_draw_string(tile, item_rect.x + item_rect.w - 92, item_rect.y + 8, "[运行]", FONT_16, 0x03DF, 0xDEFB);
        ui_draw_string(tile, item_rect.x + item_rect.w - 46, item_rect.y + 8, "[编辑]", FONT_16, 0x03E0, 0xDEFB);
    }
}

static void on_edit_scroll_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();

    for (uint16_t i = 0; i < data->edit_line_count; i++) {
        int16_t iy = viewport->y + i * EDIT_LINE_H - scroll_y;
        if (iy + EDIT_LINE_H < tile->rect.y || iy > tile->rect.y + tile->rect.h) continue;

        ui_rect_t line_rect = ui_rect_make(viewport->x + 2, iy, viewport->w - 4, EDIT_LINE_H);
        ui_rect_t overlap;

        bool is_selected = (i == (uint16_t)data->selected_line_idx);
        uint16_t bg_col = is_selected ? 0xDEFB : 0x10A2;
        uint16_t num_col = is_selected ? 0x03DF : 0x8410;
        uint16_t text_col = is_selected ? 0x4208 : 0xFFFF;

        if (ui_rect_intersect(&tile->rect, &line_rect, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, bg_col);
            if (is_selected) ui_draw_rect(tile, &line_rect, 0x03DF);
        }

        char num_str[8];
        snprintf(num_str, sizeof(num_str), "%02u ", i + 1);
        ui_draw_string(tile, line_rect.x + 4, line_rect.y + 4, num_str, FONT_16, num_col, bg_col);
        ui_draw_string(tile, line_rect.x + 32, line_rect.y + 4, lines[i].text, FONT_16, text_col, bg_col);
    }
}

static void on_flow_scroll_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (data->flow_seq.step_count == 0) {
        ui_draw_string(tile, viewport->x + 20, viewport->y + 30 - scroll_y, "当前无步骤，点击顶部 [+步骤] 添加", FONT_16, 0x8410, 0xFFFF);
        return;
    }

    for (uint8_t i = 0; i < data->flow_seq.step_count; i++) {
        int16_t iy = viewport->y + i * FLOW_CARD_H - scroll_y;
        if (iy + FLOW_CARD_H < tile->rect.y || iy > tile->rect.y + tile->rect.h) continue;

        ui_rect_t card = ui_rect_make(viewport->x + 2, iy + 2, viewport->w - 4, FLOW_CARD_H - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xDEFB);
            ui_draw_rect(tile, &card, 0xCE59);
        }

        char step_num[8];
        snprintf(step_num, sizeof(step_num), "%02u.", i + 1);
        ui_draw_string(tile, card.x + 6, card.y + 12, step_num, FONT_16, 0x03DF, 0xDEFB);

        flow_card_step_t *st = &data->flow_seq.steps[i];

        // 原语名按钮
        ui_rect_t b_prim = ui_rect_make(card.x + 36, card.y + 7, 86, 26);
        if (ui_rect_intersect(&tile->rect, &b_prim, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xFFFF);
            ui_draw_rect(tile, &b_prim, 0x03DF);
        }
        ui_draw_string(tile, b_prim.x + 4, b_prim.y + 5, st->prim_name, FONT_16, 0x03DF, 0xFFFF);

        // 参数 1 框
        ui_rect_t b_arg0 = ui_rect_make(card.x + 128, card.y + 7, 76, 26);
        if (ui_rect_intersect(&tile->rect, &b_arg0, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xFFFF);
            ui_draw_rect(tile, &b_arg0, 0x8410);
        }
        ui_draw_string(tile, b_arg0.x + 4, b_arg0.y + 5, st->args[0][0] ? st->args[0] : "---", FONT_16, 0x4208, 0xFFFF);

        // 参数 2 框
        ui_rect_t b_arg1 = ui_rect_make(card.x + 210, card.y + 7, 68, 26);
        if (ui_rect_intersect(&tile->rect, &b_arg1, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xFFFF);
            ui_draw_rect(tile, &b_arg1, 0x8410);
        }
        ui_draw_string(tile, b_arg1.x + 4, b_arg1.y + 5, st->args[1][0] ? st->args[1] : "---", FONT_16, 0x4208, 0xFFFF);

        // 删除按钮 [X]
        ui_draw_string(tile, card.x + card.w - 32, card.y + 12, "[X]", FONT_16, 0xF800, 0xDEFB);
    }
}

/* ================= 8. App 标准生命周期 ================= */
static void app_script_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_script_enter(app_t *self) {
    script_app_data_t *data = (script_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(script_app_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);

    if (f_mount(&data->fs, "0:", 1) == FR_OK) {
        data->fs_mounted = true;
    } else {
        data->fs_mounted = false;
    }

    script_vm_init(&data->vm);
    script_vm_set_log(&data->vm, script_log_bridge, data);

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_script_tabs, SCRIPT_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0 控件
    ui_terminal_init(&data->terminal, TERM_X, TERM_Y, TERM_W, TERM_H);
    data->terminal.color_bg = 0x10A2;
    ui_terminal_write_line(&data->terminal, "交互式 DSL 引擎就绪, 输入 help 查看指令", 0x07E0);
    if (!data->fs_mounted) {
        ui_terminal_write_line(&data->terminal, "[WARN] TF卡未挂载，仅支持内置自测与内存指令", 0xFD20);
    }

    ui_input_init(&data->input_cmd, INPUT_CMD_X, INPUT_CMD_Y, INPUT_CMD_W, INPUT_CMD_H, "输入原语命令", on_input_focus);
    data->input_cmd.user_data = data;
    data->input_cmd.color_bg = 0xDEFB;
    data->input_cmd.color_border = 0xCE59;
    data->input_cmd.color_text = 0x4208;

    ui_btn_init(&data->btn_exec, BTN_EXEC_X, BTN_EXEC_Y, BTN_EXEC_W, BTN_EXEC_H, "执行", on_btn_exec_click);
    data->btn_exec.user_data = data;
    data->btn_exec.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_exec, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_run, BTN_RUN_X, BTN_RUN_Y, BTN_RUN_W, BTN_RUN_H, "文件", on_run_click);
    data->btn_run.user_data = data;
    data->btn_run.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_run, 0xDEFB, 0xCE59, 0x4208);

    ui_btn_init(&data->btn_clear, BTN_CLEAR_X, BTN_CLEAR_Y, BTN_CLEAR_W, BTN_CLEAR_H, "清屏", on_clear_click);
    data->btn_clear.user_data = data;
    data->btn_clear.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_clear, 0xDEFB, 0x630C, 0xFFFF);

    ui_keyboard_init(&data->keyboard, on_keyboard_submit);
    data->keyboard.user_data = data;
    ui_input_bind_btn(&data->input_cmd, &data->btn_exec);

    // Tab 1 控件
    ui_btn_init(&data->btn_new_file, 318, 36, 70, 30, "新建", on_new_file_click);
    data->btn_new_file.user_data = data;
    data->btn_new_file.style = UI_BTN_STYLE_ROUND;
    data->btn_new_file.font_size = FONT_16;
    ui_btn_set_color(&data->btn_new_file, 0x03E0, 0x02E0, 0xFFFF);

    ui_btn_init(&data->btn_refresh, 398, 36, 70, 30, "刷新", on_refresh_click);
    data->btn_refresh.user_data = data;
    data->btn_refresh.style = UI_BTN_STYLE_ROUND;
    data->btn_refresh.font_size = FONT_16;
    ui_btn_set_color(&data->btn_refresh, 0x03DF, 0x0275, 0xFFFF);

    ui_scrollview_init(&data->file_scroll, FILE_LIST_X, FILE_LIST_Y, FILE_LIST_W, FILE_LIST_H,
                       FILE_LIST_H, on_file_scroll_draw_content);
    data->file_scroll.user_data = data;
    data->file_scroll.color_bg = 0xFFFF;
    data->file_scroll.color_scrollbar = 0xCE59;
    data->file_scroll.show_scrollbar = true;

    // Tab 2 控件
    ui_btn_init(&data->btn_save_file, 304, 36, 54, 30, "保存", on_save_file_click);
    data->btn_save_file.user_data = data;
    data->btn_save_file.style = UI_BTN_STYLE_ROUND;
    data->btn_save_file.font_size = FONT_16;
    ui_btn_set_color(&data->btn_save_file, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_insert_line, 362, 36, 54, 30, "插入", on_insert_line_click);
    data->btn_insert_line.user_data = data;
    data->btn_insert_line.style = UI_BTN_STYLE_ROUND;
    data->btn_insert_line.font_size = FONT_16;
    ui_btn_set_color(&data->btn_insert_line, 0xDEFB, 0xCE59, 0x4208);

    ui_btn_init(&data->btn_delete_line, 420, 36, 54, 30, "删除", on_delete_line_click);
    data->btn_delete_line.user_data = data;
    data->btn_delete_line.style = UI_BTN_STYLE_ROUND;
    data->btn_delete_line.font_size = FONT_16;
    ui_btn_set_color(&data->btn_delete_line, 0xF800, 0x8000, 0xFFFF);

    ui_scrollview_init(&data->edit_scroll, EDIT_SCROLL_X, EDIT_SCROLL_Y, EDIT_SCROLL_W, EDIT_SCROLL_H,
                       EDIT_SCROLL_H, on_edit_scroll_draw_content);
    data->edit_scroll.user_data = data;
    data->edit_scroll.color_bg = 0x10A2;
    data->edit_scroll.color_scrollbar = 0x03DF;
    data->edit_scroll.show_scrollbar = true;

    ui_input_init(&data->input_edit_line, EDIT_INPUT_X, EDIT_INPUT_Y, EDIT_INPUT_W, EDIT_INPUT_H, "代码行", on_input_focus);
    data->input_edit_line.user_data = data;
    data->input_edit_line.color_bg = 0xDEFB;
    data->input_edit_line.color_border = 0xCE59;
    data->input_edit_line.color_text = 0x4208;

    ui_btn_init(&data->btn_update_line, BTN_UPDATE_X, BTN_UPDATE_Y, BTN_UPDATE_W, BTN_UPDATE_H, "更新行", on_update_line_click);
    data->btn_update_line.user_data = data;
    data->btn_update_line.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_update_line, 0x03E0, 0x02E0, 0xFFFF);
    ui_input_bind_btn(&data->input_edit_line, &data->btn_update_line);

    // Tab 3 动作向导控件初始化 (绑定标准 Touch-Up 回调)
    ui_btn_init(&data->btn_flow_add, 126, 36, 52, 28, "+步骤", on_flow_add_click);
    data->btn_flow_add.user_data = data;
    data->btn_flow_add.style = UI_BTN_STYLE_ROUND;
    data->btn_flow_add.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flow_add, 0x03E0, 0x02E0, 0xFFFF);

    ui_btn_init(&data->btn_flow_loop, 184, 36, 64, 28, "单次", on_flow_loop_click);
    data->btn_flow_loop.user_data = data;
    data->btn_flow_loop.style = UI_BTN_STYLE_ROUND;
    data->btn_flow_loop.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flow_loop, 0xDEFB, 0xCE59, 0x4208);

    ui_btn_init(&data->btn_flow_to_code, 254, 36, 64, 28, "转代码", on_flow_to_code_click);
    data->btn_flow_to_code.user_data = data;
    data->btn_flow_to_code.style = UI_BTN_STYLE_ROUND;
    data->btn_flow_to_code.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flow_to_code, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_flow_run, 324, 36, 56, 28, "运行", on_flow_run_click);
    data->btn_flow_run.user_data = data;
    data->btn_flow_run.style = UI_BTN_STYLE_ROUND;
    data->btn_flow_run.font_size = FONT_16;
    ui_btn_set_color(&data->btn_flow_run, 0xF800, 0x8000, 0xFFFF);

    ui_scrollview_init(&data->flow_scroll, FLOW_SCROLL_X, FLOW_SCROLL_Y, FLOW_SCROLL_W, FLOW_SCROLL_H,
                       FLOW_SCROLL_H, on_flow_scroll_draw_content);
    data->flow_scroll.user_data = data;
    data->flow_scroll.color_bg = 0xFFFF;
    data->flow_scroll.color_scrollbar = 0xCE59;
    data->flow_scroll.show_scrollbar = true;

    // 参数编辑输入框
    ui_input_init(&data->input_flow_arg, 126, 68, 344, 36, "输入参数值", on_input_focus);
    data->input_flow_arg.user_data = data;
    data->input_flow_arg.color_bg = 0xDEFB;
    data->input_flow_arg.color_border = 0x03DF;
    data->input_flow_arg.color_text = 0x4208;

    data->flow_seq.step_count = 2;
    data->flow_seq.repeat_times = 1;
    flow_step_set_default_args(&data->flow_seq.steps[0], "pin.write");
    flow_step_set_default_args(&data->flow_seq.steps[1], "delay");
    data->flow_scroll.content_h = data->flow_seq.step_count * FLOW_CARD_H + 8;

    // 弹窗初始化 (使用常驻 GBK 字符标题)
    utf8_to_gbk_string("选择硬件原语", data->modal_title_gbk, sizeof(data->modal_title_gbk));
    ui_modal_init(&data->modal_prim, LCD_WIDTH, LCD_HEIGHT, 380, 250, 280,
                  data->modal_title_gbk, UI_MODAL_BTN_NONE,
                  on_modal_prim_draw_content, on_modal_prim_touch_content, NULL);
    data->modal_prim.user_data = data;

    editor_load_file(data, "0:/scripts/run.txt");
    ui_toast_init(&data->toast);
}

static void app_script_exit(app_t *self) {
    script_app_data_t *data = (script_app_data_t *)self->user_data;
    if (data) {
        if (data->core1_running) {
            script_vm_abort(&data->vm);
            multicore_reset_core1();
            data->core1_running = false;
        }
        if (data->fs_mounted) {
            f_mount(NULL, "0:", 0);
        }
    }
    self->user_data = NULL;
}

static void app_script_update(app_t *self, uint32_t now_tick) {
    script_app_data_t *data = (script_app_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }

    bool term_updated = false;
    while (data->log_tail != data->log_head) {
        script_log_msg_t *msg = &data->log_queue[data->log_tail];
        ui_terminal_write_line(&data->terminal, msg->text, msg->color);
        data->log_tail = (data->log_tail + 1) % SCRIPT_QUEUE_SIZE;
        term_updated = true;
    }

    if (term_updated && data->tabview.active_tab_idx == 0) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    }

    if (!data->core1_running && strcmp(data->btn_run.text, "文件") != 0) {
        data->btn_run.text = "文件";
        ui_btn_set_color(&data->btn_run, 0xDEFB, 0xCE59, 0x4208);
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_run.rect);
    }
}

static bool app_script_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    script_app_data_t *data = (script_app_data_t *)self->user_data;
    if (!data) return false;

    // 1. 优先捕获原语选择弹窗
    if (data->modal_prim.is_open) {
        bool handled = ui_modal_process_touch(&data->modal_prim, touching, tx, ty, now_tick);
        if (handled || !data->modal_prim.is_open) {
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        return true;
    }

    // 2. 键盘输入捕获
    if (data->keyboard.is_open) {
        bool handled = ui_keyboard_process_touch(&data->keyboard, touching, tx, ty, now_tick);
        if (handled || !data->keyboard.is_open) {
            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        }
        return true;
    }

    // 3. Tab 标签栏切换
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

    // Tab 0
    if (data->tabview.active_tab_idx == 0) {
        if (ui_input_process_touch(&data->input_cmd, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->input_cmd.rect);
            handled = true;
        }
        else if (ui_btn_process_touch(&data->btn_exec, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_exec.rect);
            handled = true;
        }
        else if (ui_btn_process_touch(&data->btn_run, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_run.rect);
            handled = true;
        }
        else if (ui_btn_process_touch(&data->btn_clear, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_clear.rect);
            handled = true;
        }
        else if (ui_terminal_process_touch(&data->terminal, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
            handled = true;
        }
    }
    // Tab 1
    else if (data->tabview.active_tab_idx == 1) {
        if (ui_btn_process_touch(&data->btn_new_file, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_new_file.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_refresh, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_refresh.rect);
            return true;
        }

        bool scroll_handled = ui_scrollview_process_touch(&data->file_scroll, touching, tx, ty, now_tick);
        if (scroll_handled) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->file_scroll.view_rect);
        }

        static bool    s_touch_down = false;
        static int16_t s_down_x = 0, s_down_y = 0;
        static int16_t s_down_scroll_y = 0;

        if (touching && !s_touch_down) {
            s_touch_down = true;
            s_down_x = tx; s_down_y = ty;
            s_down_scroll_y = data->file_scroll.scroll_y;
        }

        if (!touching && s_touch_down) {
            s_touch_down = false;
            int16_t dx = tx - s_down_x;
            int16_t dy = ty - s_down_y;
            int16_t dscroll = data->file_scroll.scroll_y - s_down_scroll_y;

            if (abs(dx) <= 6 && abs(dy) <= 6 && dscroll == 0 && !data->file_scroll.is_dragged) {
                ui_rect_t vr = data->file_scroll.view_rect;
                if (tx >= vr.x && tx <= vr.x + vr.w && ty >= vr.y && ty <= vr.y + vr.h) {
                    int idx = (ty - vr.y + data->file_scroll.scroll_y) / FILE_ITEM_H;
                    if (idx >= 0 && idx < data->file_count) {
                        char path[64];
                        snprintf(path, sizeof(path), "0:/scripts/%s", data->file_list[idx].name);

                        int16_t rel_x = tx - vr.x;
                        if (rel_x >= vr.w - 96 && rel_x <= vr.w - 52) {
                            if (load_script_file_for_run(data, path)) {
                                data->tabview.active_tab_idx = 0;
                                data->last_tab_idx = 0;
                                ui_terminal_write_line(&data->terminal, "--------------------------------", 0x4208);
                                char log_msg[80];
                                snprintf(log_msg, sizeof(log_msg), "[SYS] 执行脚本: %s", data->file_list[idx].name);
                                ui_terminal_write_line(&data->terminal, log_msg, 0x07FF);

                                start_script_exec(data);
                                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                                return true;
                            }
                        }
                        else if (rel_x >= vr.w - 50) {
                            editor_load_file(data, path);
                            data->tabview.active_tab_idx = 2;
                            data->last_tab_idx = 2;
                            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                            return true;
                        }
                    }
                }
            }
        }
        handled = scroll_handled;
    }
    // Tab 2
    else if (data->tabview.active_tab_idx == 2) {
        if (ui_btn_process_touch(&data->btn_save_file, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_save_file.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_insert_line, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_insert_line.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_delete_line, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_delete_line.rect);
            return true;
        }
        if (ui_input_process_touch(&data->input_edit_line, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->input_edit_line.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->btn_update_line, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_update_line.rect);
            return true;
        }

        bool scroll_handled = ui_scrollview_process_touch(&data->edit_scroll, touching, tx, ty, now_tick);
        if (scroll_handled) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->edit_scroll.view_rect);
        }

        static bool    s_ed_down = false;
        static int16_t s_ed_x = 0, s_ed_y = 0;
        static int16_t s_ed_scroll_y = 0;

        if (touching && !s_ed_down) {
            s_ed_down = true;
            s_ed_x = tx; s_ed_y = ty;
            s_ed_scroll_y = data->edit_scroll.scroll_y;
        }

        if (!touching && s_ed_down) {
            s_ed_down = false;
            int16_t dx = tx - s_ed_x;
            int16_t dy = ty - s_ed_y;
            int16_t dscroll = data->edit_scroll.scroll_y - s_ed_scroll_y;

            if (abs(dx) <= 6 && abs(dy) <= 6 && dscroll == 0 && !data->edit_scroll.is_dragged) {
                ui_rect_t vr = data->edit_scroll.view_rect;
                if (tx >= vr.x && tx <= vr.x + vr.w && ty >= vr.y && ty <= vr.y + vr.h) {
                    int idx = (ty - vr.y + data->edit_scroll.scroll_y) / EDIT_LINE_H;
                    if (idx >= 0 && idx < data->edit_line_count) {
                        data->selected_line_idx = idx;
                        script_line_entry_t *lines = (script_line_entry_t *)app_loader_get_ram_pool();
                        ui_input_set_text(&data->input_edit_line, lines[idx].text);

                        data->dirty_rect = data->edit_scroll.view_rect;
                        data->dirty_rect = rect_union_box(data->dirty_rect, data->input_edit_line.rect);
                        return true;
                    }
                }
            }
        }
        handled = scroll_handled;
    }
    // Tab 3 动作向导
    else if (data->tabview.active_tab_idx == 3) {
        if (ui_btn_process_touch(&data->btn_flow_add, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_flow_add.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_flow_loop, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_flow_loop.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_flow_to_code, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_flow_to_code.rect);
            return true;
        }
        if (ui_btn_process_touch(&data->btn_flow_run, touching, tx, ty, now_tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_flow_run.rect);
            return true;
        }

        bool scroll_handled = ui_scrollview_process_touch(&data->flow_scroll, touching, tx, ty, now_tick);
        if (scroll_handled) data->dirty_rect = rect_union_box(data->dirty_rect, data->flow_scroll.view_rect);

        static bool    s_f_down = false;
        static int16_t s_fx = 0, s_fy = 0;
        static int16_t s_f_scroll = 0;

        if (touching && !s_f_down) {
            s_f_down = true; s_fx = tx; s_fy = ty; s_f_scroll = data->flow_scroll.scroll_y;
        }
        if (!touching && s_f_down) {
            s_f_down = false;
            int16_t dx = tx - s_fx, dy = ty - s_fy;
            if (abs(dx) <= 6 && abs(dy) <= 6 && !data->flow_scroll.is_dragged) {
                ui_rect_t vr = data->flow_scroll.view_rect;
                if (tx >= vr.x && tx <= vr.x + vr.w && ty >= vr.y && ty <= vr.y + vr.h) {
                    int idx = (ty - vr.y + data->flow_scroll.scroll_y) / FLOW_CARD_H;
                    if (idx >= 0 && idx < data->flow_seq.step_count) {
                        int16_t rx = tx - vr.x;
                        flow_card_step_t *st = &data->flow_seq.steps[idx];

                        // A. 点击原语名 -> 弹出选择弹窗
                        if (rx >= 36 && rx <= 122) {
                            data->target_step_idx = idx;
                            uint8_t total_prims = script_prim_get_count();
                            data->modal_prim.sv_body.content_h = total_prims * 36 + 8;
                            ui_modal_show(&data->modal_prim, data->modal_title_gbk, data);
                            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                            return true;
                        }
                        // B. 点击参数 1 -> 呼出全键盘
                        else if (rx >= 128 && rx <= 204) {
                            data->target_step_idx = idx;
                            data->target_arg_idx = 0;
                            ui_input_set_text(&data->input_flow_arg, st->args[0]);
                            ui_keyboard_open(&data->keyboard, &data->input_flow_arg);
                            data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                            return true;
                        }
                        // C. 点击参数 2
                        else if (rx >= 210 && rx <= 278) {
                            if (strcmp(st->prim_name, "pin.write") == 0) {
                                strcpy(st->args[1], (strcmp(st->args[1], "1") == 0) ? "0" : "1");
                                data->dirty_rect = data->flow_scroll.view_rect;
                            } else if (strcmp(st->prim_name, "pin.mode") == 0) {
                                strcpy(st->args[1], (strcmp(st->args[1], "out") == 0) ? "in" : "out");
                                data->dirty_rect = data->flow_scroll.view_rect;
                            } else if (st->args[1][0] != '\0' || strcmp(st->prim_name, "pwm.set") == 0) {
                                data->target_step_idx = idx;
                                data->target_arg_idx = 1;
                                ui_input_set_text(&data->input_flow_arg, st->args[1]);
                                ui_keyboard_open(&data->keyboard, &data->input_flow_arg);
                                data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                            }
                            return true;
                        }
                        // D. 点击 [X] 删除本步
                        else if (rx >= vr.w - 44) {
                            for (uint8_t k = idx; k < data->flow_seq.step_count - 1; k++) {
                                data->flow_seq.steps[k] = data->flow_seq.steps[k + 1];
                            }
                            data->flow_seq.step_count--;
                            data->flow_scroll.content_h = data->flow_seq.step_count * FLOW_CARD_H + 8;
                            data->dirty_rect = data->flow_scroll.view_rect;
                            return true;
                        }
                    }
                }
            }
        }
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_script_draw(app_t *self, ui_tile_t *tile) {
    script_app_data_t *data = (script_app_data_t *)self->user_data;
    if (!data) return;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &ov)) {
        switch (data->tabview.active_tab_idx) {
        case 0:
            ui_terminal_draw(tile, &data->terminal);
            ui_input_draw(tile, &data->input_cmd);
            ui_btn_draw(tile, &data->btn_exec);
            ui_btn_draw(tile, &data->btn_run);
            ui_btn_draw(tile, &data->btn_clear);
            break;

        case 1:
            ui_draw_string(tile, 130, 42, "目录: 0:/scripts", FONT_16, 0x4208, 0xFFFF);
            ui_btn_draw(tile, &data->btn_new_file);
            ui_btn_draw(tile, &data->btn_refresh);
            ui_scrollview_draw(tile, &data->file_scroll);
            break;

        case 2: {
            char title_buf[64];
            snprintf(title_buf, sizeof(title_buf), "%s%s",
                     data->is_dirty ? "*" : "",
                     data->editing_filename[0] ? data->editing_filename : "草稿");
            ui_draw_string(tile, 126, 42, title_buf, FONT_16, data->is_dirty ? 0xF800 : 0x03DF, 0xFFFF);

            ui_btn_draw(tile, &data->btn_save_file);
            ui_btn_draw(tile, &data->btn_insert_line);
            ui_btn_draw(tile, &data->btn_delete_line);

            ui_scrollview_draw(tile, &data->edit_scroll);
            ui_input_draw(tile, &data->input_edit_line);
            ui_btn_draw(tile, &data->btn_update_line);
            break;
        }

        case 3:
            ui_btn_draw(tile, &data->btn_flow_add);
            ui_btn_draw(tile, &data->btn_flow_loop);
            ui_btn_draw(tile, &data->btn_flow_to_code);
            ui_btn_draw(tile, &data->btn_flow_run);
            ui_scrollview_draw(tile, &data->flow_scroll);
            if (data->keyboard.is_open) {
                ui_input_draw(tile, &data->input_flow_arg);
            }
            break;

        default:
            break;
        }
    }

    if (data->keyboard.is_open) {
        ui_keyboard_draw(tile, &data->keyboard);
    }

    if (data->modal_prim.is_open) {
        ui_modal_draw(tile, &data->modal_prim);
    }

    ui_toast_draw(tile, &data->toast);
}

app_t *app_script_get_instance(void) {
    s_app_script.id            = APP_ID_CUSTOM_0 + 4;
    s_app_script.name          = "脚本引擎";
    s_app_script.init          = app_script_init;
    s_app_script.enter         = app_script_enter;
    s_app_script.exit          = app_script_exit;
    s_app_script.update        = app_script_update;
    s_app_script.process_touch = app_script_process_touch;
    s_app_script.draw          = app_script_draw;
    return &s_app_script;
}