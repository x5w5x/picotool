#include "app_script.h"
#include "app_shared_ram.h"
#include "app_loader.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_tabview.h"
#include "ui_terminal.h"
#include "ui_btn.h"
#include "ui_toast.h"
#include "lcd.h"
#include "ff.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "fun_script_engine.h"
#include <stdio.h>
#include <string.h>

#define SCRIPT_TAB_COUNT    2
static const char *s_script_tabs[] = { "执行终端", "脚本管理" };

#define TERM_X              124
#define TERM_Y              38
#define TERM_W              348
#define TERM_H              224

#define BTN_RUN_X           124
#define BTN_RUN_Y           270
#define BTN_RUN_W           80
#define BTN_RUN_H           36

#define BTN_CLEAR_X         214
#define BTN_CLEAR_Y         270
#define BTN_CLEAR_W         80
#define BTN_CLEAR_H         36

#define SCRIPT_QUEUE_SIZE   16
#define SCRIPT_MSG_MAX_LEN  128

// 默认内置的硬件自测脚本（UTF-8 编码，入队时会自动转为 GBK）
static const char *k_fallback_script =
    "# 默认硬件自测脚本\n"
    "print \"=== 脚本引擎启动 ===\"\n"
    "pin.mode 4 out\n"
    "$0 = 5\n"
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
    char     text[SCRIPT_MSG_MAX_LEN];
    uint16_t color;
} script_log_msg_t;

typedef struct {
    ui_rect_t           dirty_rect;
    ui_tabview_t        tabview;
    uint8_t             last_tab_idx;

    ui_terminal_t       terminal;
    ui_btn_t            btn_run;
    ui_btn_t            btn_clear;
    ui_toast_t          toast;

    script_vm_t         vm;

    // Core1 -> Core0 无锁日志环形缓冲队列
    script_log_msg_t    log_queue[SCRIPT_QUEUE_SIZE];
    volatile uint8_t    log_head;
    volatile uint8_t    log_tail;

    volatile bool       core1_running;
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

/* ================= 1. UTF-8 转 GBK 编码转换器 ================= */
static void utf8_to_gbk_string(const char *utf8, char *gbk_out, size_t max_out_len) {
    if (!utf8 || !gbk_out || max_out_len == 0) return;

    size_t in_idx = 0;
    size_t out_idx = 0;

    while (utf8[in_idx] != '\0' && out_idx < max_out_len - 1) {
        uint8_t c = (uint8_t)utf8[in_idx];

        // ASCII (0x00 ~ 0x7F)[cite: 13]
        if (c < 0x80) {
            gbk_out[out_idx++] = (char)c;
            in_idx++;
        }
        // 3 字节标准汉字 UTF-8 (0xE0 ~ 0xEF)[cite: 13]
        else if ((c & 0xF0) == 0xE0 && utf8[in_idx + 1] && utf8[in_idx + 2]) {
            uint8_t c2 = (uint8_t)utf8[in_idx + 1];
            uint8_t c3 = (uint8_t)utf8[in_idx + 2];

            WCHAR unicode = (WCHAR)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
            DWORD gbk_code = ff_uni2oem(unicode, 936); // 调用 FatFs 内置字码表[cite: 13]

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
        }
        // 2 字节特殊符号 (0xC0 ~ 0xDF)[cite: 13]
        else if ((c & 0xE0) == 0xC0 && utf8[in_idx + 1]) {
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
    char gbk_buf[SCRIPT_MSG_MAX_LEN];
    utf8_to_gbk_string(utf8_text, gbk_buf, sizeof(gbk_buf));
    ui_terminal_write_line(term, gbk_buf, color);
}

/* ================= 2. Core1 工作线程与日志桥接 ================= */
static void script_log_bridge(const char *text, uint16_t color, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    uint8_t next = (data->log_head + 1) % SCRIPT_QUEUE_SIZE;
    if (next != data->log_tail) {
        // 入队前统一转为屏幕字库支持的 GBK
        utf8_to_gbk_string(text, data->log_queue[data->log_head].text, SCRIPT_MSG_MAX_LEN);
        data->log_queue[data->log_head].color = color;
        data->log_head = next;
    }
}

static void core1_worker(void) {
    script_app_data_t *data = (script_app_data_t *)g_static_app_ram;
    data->core1_running = true;

    // Core1 独立执行，delay 与死循环绝不阻塞 Core0 屏幕渲染
    script_vm_run(&data->vm);

    data->core1_running = false;
    while (true) {
        tight_loop_contents();
    }
}

/* ================= 3. UI 交互回调 ================= */
static void on_run_click(ui_btn_t *btn, void *user_data) {
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;

    if (!data->core1_running) {
        static FATFS s_fs;
        bool loaded = false;

        // 优先挂载 TF 卡并加载 0:/scripts/run.txt
        if (f_mount(&s_fs, "0:", 1) == FR_OK) {
            FIL f;
            if (f_open(&f, "0:/scripts/run.txt", FA_READ) == FR_OK) {
                UINT br = 0;
                char *buf = (char *)app_loader_get_ram_pool();
                f_read(&f, buf, 4096, &br);
                buf[br] = '\0';
                f_close(&f);

                if (br > 0) {
                    script_vm_load(&data->vm, buf);
                    term_write_utf8(&data->terminal, "[SYS] 成功加载 TF 卡脚本", 0x07FF);
                    loaded = true;
                }
            }
        }

        // 兜底方案：自动加载静态自测脚本，确保 vm.src 不为 NULL
        if (!loaded) {
            script_vm_load(&data->vm, k_fallback_script);
            term_write_utf8(&data->terminal, "[SYS] 运行内置自测脚本", 0xFD20);
        }

        // 启动 Core1 线程
        multicore_reset_core1();
        multicore_launch_core1(core1_worker);

        btn->text = "中止";
        ui_btn_set_color(btn, 0xF800, 0x8000, 0xFFFF);
        ui_toast_show(&data->toast, "Core1 引擎启动", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));

        data->dirty_rect = rect_union_box(btn->rect, data->terminal.rect);
    } else {
        // 强行中止并物理重置 Core1
        script_vm_abort(&data->vm);
        sleep_ms(5);
        multicore_reset_core1();
        data->core1_running = false;

        btn->text = "运行";
        ui_btn_set_color(btn, 0x03DF, 0x0275, 0xFFFF);
        term_write_utf8(&data->terminal, "[SYS] 运行已被用户中止", 0xF800);
        ui_toast_show(&data->toast, "已强制中止", UI_TOAST_ERROR, to_ms_since_boot(get_absolute_time()));

        data->dirty_rect = rect_union_box(btn->rect, data->terminal.rect);
    }
}

static void on_clear_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (!data) return;
    ui_terminal_clear(&data->terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
}

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    script_app_data_t *data = (script_app_data_t *)user_data;
    if (data) data->dirty_rect = tv->content_rect;
}

/* ================= 4. App 标准生命周期 ================= */
static void app_script_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_script_enter(app_t *self) {
    script_app_data_t *data = (script_app_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(script_app_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);

    script_vm_init(&data->vm);
    script_vm_set_log(&data->vm, script_log_bridge, data);

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_script_tabs, SCRIPT_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    ui_terminal_init(&data->terminal, TERM_X, TERM_Y, TERM_W, TERM_H);
    data->terminal.color_bg = 0x10A2;
    term_write_utf8(&data->terminal, "DSL 脚本引擎就绪", 0x07E0);

    ui_btn_init(&data->btn_run, BTN_RUN_X, BTN_RUN_Y, BTN_RUN_W, BTN_RUN_H, "运行", on_run_click);
    data->btn_run.user_data = data;
    data->btn_run.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_run, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_clear, BTN_CLEAR_X, BTN_CLEAR_Y, BTN_CLEAR_W, BTN_CLEAR_H, "清屏", on_clear_click);
    data->btn_clear.user_data = data;
    data->btn_clear.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_clear, 0xDEFB, 0x630C, 0xFFFF);

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

    // 从无锁环形队列中消费 Core1 传递过来的日志
    bool term_updated = false;
    while (data->log_tail != data->log_head) {
        script_log_msg_t *msg = &data->log_queue[data->log_tail];
        // 队列里的文本已经转为 GBK，直接写入终端
        ui_terminal_write_line(&data->terminal, msg->text, msg->color);
        data->log_tail = (data->log_tail + 1) % SCRIPT_QUEUE_SIZE;
        term_updated = true;
    }

    if (term_updated && data->tabview.active_tab_idx == 0) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->terminal.rect);
    }

    // 脚本自然执行结束后，将按键文字自动还原为“运行”
    if (!data->core1_running && strcmp(data->btn_run.text, "运行") != 0) {
        data->btn_run.text = "运行";
        ui_btn_set_color(&data->btn_run, 0x03DF, 0x0275, 0xFFFF);
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_run.rect);
    }
}

static bool app_script_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    script_app_data_t *data = (script_app_data_t *)self->user_data;
    if (!data) return false;

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
        if (ui_btn_process_touch(&data->btn_run, touching, tx, ty, now_tick)) {
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
        if (data->tabview.active_tab_idx == 0) {
            ui_terminal_draw(tile, &data->terminal);
            ui_btn_draw(tile, &data->btn_run);
            ui_btn_draw(tile, &data->btn_clear);
        } else if (data->tabview.active_tab_idx == 1) {
            ui_draw_string(tile, 130, 60, "脚本存放路径: 0:/scripts/run.txt", FONT_16, 0x4208, 0xFFFF);
            ui_draw_string(tile, 130, 90, "支持原语: pin.write, delay, goto", FONT_16, 0x8410, 0xFFFF);
            ui_draw_string(tile, 130, 120, "支持变量: $0 ~ $7, $?", FONT_16, 0x8410, 0xFFFF);
        }
    }

    ui_toast_draw(tile, &data->toast);
}

/* ================= 5. 实例导出 ================= */
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