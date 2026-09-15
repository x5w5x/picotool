#include "app_sd_msc.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "ui_file_browser.h"
#include "ui_hex_view.h"
#include "ui_progressbar.h"
#include "lcd.h"
#include "chinese.h"
#include "tf.h"
#include "ff.h"
#include "f_util.h"
#include "swd_flasher.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "app_loader.h"
extern void usb_switch_mode(uint8_t mode);
extern void msc_set_enabled(bool enabled);
extern bool msc_disk_check_write_completed(uint32_t quiet_ms);
extern void msc_disk_reset_write_state(void);

enum {
    TAB_SD_BROWSER = 0,
    TAB_SD_TEST    = 1,
    TAB_USB_MSC    = 2,
    TAB_PINOUT     = 3,
    TAB_SD_COUNT   = 4
};

static const char *s_sd_tabs[] = {
    "文件浏览",
    "TF卡测试",
    "虚拟U盘",
    "引脚说明"
};

typedef enum {
    VIEW_MODE_BROWSER = 0,
    VIEW_MODE_HEX_PREVIEW,
    VIEW_MODE_FLASHING
} sub_view_mode_t;

typedef struct {
    bool     is_detected;
    uint32_t sector_count;
    uint32_t capacity_mb;
    char     card_type[16];
    uint32_t write_speed_kbs;
    uint32_t read_speed_kbs;
    bool     test_done;
    char     status_msg[32];
} sd_test_state_t;

typedef struct {
    ui_rect_t          dirty_rect;
    ui_tabview_t       tabview;
    sd_test_state_t    state;

    FATFS              fs;
    bool               fs_mounted;
    char               current_path[256];
    char               selected_file_path[280];
    char               selected_file_name[64];
    char               matched_algo_path[280];

    sub_view_mode_t    view_mode;

    // Tab 0: 浏览器与 Hex 查看器
    ui_file_browser_t  file_browser;
    ui_btn_t           btn_fs_refresh;
    ui_hex_view_t      hex_view;
    ui_btn_t           btn_close_hex;
    uint8_t            hex_buf[512];
    UINT               hex_buf_len;

    // 烧录面板
    ui_rect_t          flash_card;
    ui_progressbar_t   prog_bar;
    ui_btn_t           btn_start_flash;
    ui_btn_t           btn_close_flash;

    // Tab 1: 测速与探测
    ui_rect_t          info_card;
    ui_btn_t           btn_detect;
    ui_btn_t           btn_benchmark;
    
    // Tab 2: 虚拟 U 盘与两层开关
    bool               msc_active;            // 第 1 层：U 盘使能
    bool               auto_flash_en;         // 第 2 层：拖拽自动烧录使能
    bool               msc_was_auto_dropped;  // ⚡ 记录是否为拖拽触发的烧录（用于烧录成功后自动恢复 U 盘）
    bool               auto_result_pending;
    uint32_t           auto_result_tick;
    ui_rect_t          msc_card;
    ui_switch_t        sw_msc_en;             // 开关 1
    ui_switch_t        sw_auto_flash;         // 开关 2
} app_sd_data_t;

static app_t         s_app_sd;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 目录扫描与查找最新固件 ================= */

static void load_directory(app_sd_data_t *data, const char *path) {
    if (!data) return;
    ui_file_browser_clear(&data->file_browser);

    if (!data->fs_mounted) {
        if (f_mount(&data->fs, "0:", 1) != FR_OK) return;
        data->fs_mounted = true;
    }

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK) return;

    if (strcmp(path, "0:") != 0 && strcmp(path, "0:/") != 0) {
        ui_file_browser_add_item(&data->file_browser, "..", 0, true);
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        ui_file_browser_add_item(&data->file_browser, fno.fname, (uint32_t)fno.fsize, is_dir);
    }
    f_closedir(&dir);
}

static bool find_latest_firmware(app_sd_data_t *data, char *out_path, char *out_name) {
    (void)data;
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "0:/") != FR_OK) return false;

    DWORD max_fdate = 0;
    DWORD max_ftime = 0;
    bool found = false;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (!(fno.fattrib & AM_DIR)) {
            if (strstr(fno.fname, ".hex") || strstr(fno.fname, ".HEX") ||
                strstr(fno.fname, ".bin") || strstr(fno.fname, ".BIN")) {
                
                if (!found || (fno.fdate > max_fdate) || (fno.fdate == max_fdate && fno.ftime >= max_ftime)) {
                    max_fdate = fno.fdate;
                    max_ftime = fno.ftime;
                    snprintf(out_path, 280, "0:/%s", fno.fname);
                    strncpy(out_name, fno.fname, 63);
                    found = true;
                }
            }
        }
    }
    f_closedir(&dir);
    return found;
}

static void restore_auto_msc(app_sd_data_t *data) {
    if (!data) return;
    data->msc_was_auto_dropped = false;
    if (data->fs_mounted) {
        f_mount(NULL, "0:", 1);
        data->fs_mounted = false;
    }
    data->msc_active = true;
    msc_disk_reset_write_state();
    msc_set_enabled(true);
    usb_switch_mode(1);
    ui_switch_set_state(&data->sw_msc_en, true);
}

static void on_file_item_click(ui_file_browser_t *fb, int8_t index, const ui_file_item_t *item, void *user_data) {
    (void)fb; (void)index;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    const char *name = item->name;
    bool is_dir = (item->type == UI_FILE_TYPE_DIR);

    if (is_dir) {
        if (strcmp(name, "..") == 0) {
            char *last_slash = strrchr(data->current_path, '/');
            if (last_slash && last_slash != data->current_path + 2) *last_slash = '\0';
            else strcpy(data->current_path, "0:/");
        } else {
            if (data->current_path[strlen(data->current_path) - 1] != '/') strcat(data->current_path, "/");
            strcat(data->current_path, name);
        }
        load_directory(data, data->current_path);
        data->dirty_rect = data->tabview.content_rect;
    } else {
        if (strcmp(data->current_path, "0:/") == 0) {
            snprintf(data->selected_file_path, sizeof(data->selected_file_path), "0:/%s", name);
        } else {
            snprintf(data->selected_file_path, sizeof(data->selected_file_path), "%s/%s", data->current_path, name);
        }
        strncpy(data->selected_file_name, name, sizeof(data->selected_file_name) - 1);

        if (strstr(name, ".papp") || strstr(name, ".PAPP")) {
            printf("[PAPP] click: \"%s\" path=\"%s\"\r\n",
                   name ? name : "(null)",
                   data->selected_file_path);
            if (app_loader_load_papp(data->selected_file_path)) {
                for (uint8_t i = 0; i < g_app_mgr.app_count; i++) {
                    if (g_app_mgr.apps[i] == app_loader_get_instance()) {
                        app_manager_switch_to(&g_app_mgr, i);
                        break;
                    }
                }
            }
            return;
        }

        if (strstr(name, ".bin") || strstr(name, ".BIN") || strstr(name, ".hex") || strstr(name, ".HEX")) {
            strcpy(data->matched_algo_path, "0:/algo/STM32F10x_128.algo");
            data->view_mode = VIEW_MODE_FLASHING;
            data->msc_was_auto_dropped = false;
            ui_progressbar_set_progress(&data->prog_bar, 0);
            data->dirty_rect = data->tabview.content_rect;
        } else {
            FIL fil;
            if (f_open(&fil, data->selected_file_path, FA_READ) == FR_OK) {
                memset(data->hex_buf, 0, sizeof(data->hex_buf));
                f_read(&fil, data->hex_buf, sizeof(data->hex_buf), &data->hex_buf_len);
                f_close(&fil);

                ui_hex_view_bind(&data->hex_view, data->hex_buf, data->hex_buf_len, 0x0000);
                data->view_mode = VIEW_MODE_HEX_PREVIEW;
                data->dirty_rect = data->tabview.content_rect;
            }
        }
    }
}

static void on_start_flash_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    swd_flasher_start(data->selected_file_path, data->matched_algo_path);
    data->dirty_rect = data->flash_card;
}

static void on_close_flash_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    swd_flasher_abort();
    data->msc_was_auto_dropped = false;
    data->view_mode = VIEW_MODE_BROWSER;
    data->dirty_rect = data->tabview.content_rect;
}

static void on_close_hex_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    data->view_mode = VIEW_MODE_BROWSER;
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= 探测与测速 ================= */

static void do_card_detect(sd_test_state_t *st) {
    memset(st, 0, sizeof(sd_test_state_t));
    if (TF_Init() == TF_OK) {
        st->is_detected = true;
        uint32_t sectors = 0;
        if (TF_GetSectors(&sectors) == TF_OK && sectors > 0) {
            st->sector_count = sectors;
            st->capacity_mb  = (uint32_t)(((uint64_t)sectors * TF_BLOCK_SIZE) / (1024 * 1024));
        }
        strcpy(st->card_type, "SD / TF Card");
        strcpy(st->status_msg, "卡初始化就绪");
    } else {
        st->is_detected = false;
        strcpy(st->status_msg, "未识别到 TF 卡");
    }
}

static void do_card_benchmark(sd_test_state_t *st) {
    if (!st->is_detected && TF_Init() != TF_OK) return;
    st->is_detected = true;

    static uint8_t test_buf[4096];
    memset(test_buf, 0x5A, sizeof(test_buf));

    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < 512; i += 8) {
        if (TF_WriteBlocks(2048 + i, test_buf, 8) != TF_OK) {
            strcpy(st->status_msg, "写入测试出错");
            return;
        }
    }
    uint64_t write_us = time_us_64() - t0;
    st->write_speed_kbs = (256 * 1000000ULL) / (write_us ? write_us : 1);

    t0 = time_us_64();
    for (uint32_t i = 0; i < 512; i += 8) {
        if (TF_ReadBlocks(2048 + i, test_buf, 8) != TF_OK) {
            strcpy(st->status_msg, "读取测试出错");
            return;
        }
    }
    uint64_t read_us = time_us_64() - t0;
    st->read_speed_kbs = (256 * 1000000ULL) / (read_us ? read_us : 1);

    st->test_done = true;
    strcpy(st->status_msg, "测速完成");
}

/* ================= 开关事件处理 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    data->view_mode = VIEW_MODE_BROWSER;
    if (new_tab == TAB_SD_BROWSER) {
        load_directory(data, data->current_path);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_fs_refresh_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    load_directory(data, data->current_path);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_detect_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    do_card_detect(&data->state);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_benchmark_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    do_card_benchmark(&data->state);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_msc_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    data->msc_active = state;
    data->msc_was_auto_dropped = false;

    if (state) {
        if (data->fs_mounted) {
            f_mount(NULL, "0:", 1);
            data->fs_mounted = false;
        }
        msc_disk_reset_write_state();
        msc_set_enabled(true);
        usb_switch_mode(1);
    } else {
        msc_set_enabled(false);
        usb_switch_mode(0);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_auto_flash_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    if (!data) return;
    data->auto_flash_en = state;
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= 生命周期与主循环步进 ================= */

static void app_sd_init(app_t *self) {
    self->user_data = g_static_app_ram;
}

static void app_sd_enter(app_t *self) {
    app_sd_data_t *data = (app_sd_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_sd_data_t));

    strcpy(data->current_path, "0:/");
    data->view_mode            = VIEW_MODE_BROWSER;
    data->auto_flash_en        = true;
    data->msc_was_auto_dropped = false;

    swd_flasher_init();

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_sd_tabs, TAB_SD_COUNT, TAB_SD_BROWSER, on_tab_changed);
    data->tabview.user_data = data;

    ui_file_browser_init(&data->file_browser, 120, 72, 350, 236, on_file_item_click);
    data->file_browser.user_data = data;

    ui_btn_init(&data->btn_fs_refresh, 390, 36, 75, 30, "刷新", on_fs_refresh_click);
    data->btn_fs_refresh.user_data = data;
    data->btn_fs_refresh.style     = UI_BTN_STYLE_ROUND;
    data->btn_fs_refresh.font_size = FONT_16;
    ui_btn_set_color(&data->btn_fs_refresh, 0x03DF, 0x0275, 0xFFFF);

    ui_hex_view_init(&data->hex_view, 120, 72, 350, 236, NULL);
    data->hex_view.user_data = data;
    ui_btn_init(&data->btn_close_hex, 390, 36, 75, 30, "返回", on_close_hex_click);
    data->btn_close_hex.user_data = data;
    data->btn_close_hex.style     = UI_BTN_STYLE_ROUND;
    data->btn_close_hex.font_size = FONT_16;
    ui_btn_set_color(&data->btn_close_hex, 0x8410, 0x4208, 0xFFFF);

    data->flash_card = ui_rect_make(120, 40, 350, 268);
    ui_progressbar_init(&data->prog_bar, 140, 218, 310, 20, true);
    ui_progressbar_set_progress(&data->prog_bar, 0);

    ui_btn_init(&data->btn_start_flash, 140, 252, 140, 42, "立即烧录", on_start_flash_click);
    data->btn_start_flash.user_data = data;
    data->btn_start_flash.style     = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_start_flash, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_close_flash, 310, 252, 140, 42, "取消/返回", on_close_flash_click);
    data->btn_close_flash.user_data = data;
    data->btn_close_flash.style     = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_close_flash, 0xF800, 0xB000, 0xFFFF);

    data->info_card = ui_rect_make(125, 44, 340, 160);
    ui_btn_init(&data->btn_detect, 130, 220, 150, 42, "探测 TF 卡", on_detect_click);
    data->btn_detect.user_data = data; data->btn_detect.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_detect, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_benchmark, 305, 220, 150, 42, "读写测速", on_benchmark_click);
    data->btn_benchmark.user_data = data; data->btn_benchmark.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_benchmark, 0x05E0, 0x03E0, 0xFFFF);

    data->msc_card = ui_rect_make(125, 44, 340, 130);
    ui_switch_init(&data->sw_msc_en, 385, 185, 64, 32, false, on_msc_switch);
    data->sw_msc_en.user_data = data;
    ui_switch_init(&data->sw_auto_flash, 385, 235, 64, 32, true, on_auto_flash_switch);
    data->sw_auto_flash.user_data = data;

    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->view_mode = VIEW_MODE_BROWSER;
    load_directory(data, data->current_path);
}

static void app_sd_exit(app_t *self) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    if (data) {
        swd_flasher_abort();
        data->msc_was_auto_dropped = false;
        if (data->msc_active) {
            data->msc_active = false;
            msc_set_enabled(false);
            usb_switch_mode(0);
        }
        if (data->fs_mounted) {
            f_mount(NULL, "0:", 1);
            data->fs_mounted = false;
        }
    }
    self->user_data = NULL;
}

static void app_sd_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    if (!data) return;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    if (data->msc_active && data->auto_flash_en && msc_disk_check_write_completed(800)) {
        data->msc_active = false;
        data->msc_was_auto_dropped = true;
        msc_set_enabled(false);
        usb_switch_mode(0);

        if (!data->fs_mounted) {
            f_mount(&data->fs, "0:", 1);
            data->fs_mounted = true;
        }

        char drop_path[280] = {0};
        char drop_name[64]  = {0};
        if (find_latest_firmware(data, drop_path, drop_name)) {
            strncpy(data->selected_file_path, drop_path, sizeof(data->selected_file_path) - 1);
            strncpy(data->selected_file_name, drop_name, sizeof(data->selected_file_name) - 1);
            strcpy(data->matched_algo_path, "0:/algo/STM32F10x_128.algo");

            data->tabview.active_tab_idx = TAB_SD_BROWSER;
            data->view_mode = VIEW_MODE_FLASHING;
            data->auto_result_pending = false;
            ui_progressbar_set_progress(&data->prog_bar, 0);

            if (!swd_flasher_start(data->selected_file_path,
                                   data->matched_algo_path)) {
                restore_auto_msc(data);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return;
            }
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
            return;
        } else {
            restore_auto_msc(data);
        }
    }

    if (data->tabview.active_tab_idx == TAB_SD_BROWSER && data->view_mode == VIEW_MODE_FLASHING) {
        const flasher_status_t *st = swd_flasher_get_status();
        if (st->stage != FLASHER_IDLE && st->stage != FLASHER_STAGE_SUCCESS && st->stage != FLASHER_STAGE_ERROR) {
            swd_flasher_step();
            ui_progressbar_set_progress(&data->prog_bar, st->progress_percent);
            data->dirty_rect = data->flash_card;

        }
        if ((st->stage == FLASHER_STAGE_SUCCESS ||
             st->stage == FLASHER_STAGE_ERROR) &&
            data->msc_was_auto_dropped &&
            !data->auto_result_pending) {
            swd_flasher_release_bus();
            data->auto_result_pending = true;
            data->auto_result_tick = now_tick;
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }

        /* Keep the result panel visible briefly, then reconnect the disk. */
        if (data->auto_result_pending &&
            (uint32_t)(now_tick - data->auto_result_tick) >= 1200u) {
            data->auto_result_pending = false;
            restore_auto_msc(data);
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
    }
}

static bool app_sd_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    if (!data) return false;

    if (data->tabview.active_tab_idx == TAB_SD_BROWSER && data->view_mode == VIEW_MODE_FLASHING) {
        if (ui_btn_process_touch(&data->btn_start_flash, is_touching, tx, ty, now_tick)) return true;
        if (ui_btn_process_touch(&data->btn_close_flash, is_touching, tx, ty, now_tick)) return true;
        return true;
    }

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_tabview_get_bar_dirty_rect(&data->tabview));
        return true;
    }

    switch (data->tabview.active_tab_idx) {
        case TAB_SD_BROWSER:
            if (data->view_mode == VIEW_MODE_HEX_PREVIEW) {
                if (ui_btn_process_touch(&data->btn_close_hex, is_touching, tx, ty, now_tick)) return true;
                if (ui_hex_view_process_touch(&data->hex_view, is_touching, tx, ty, now_tick)) {
                    data->dirty_rect = rect_union_box(data->dirty_rect, data->hex_view.rect);
                    return true;
                }
                return false;
            }

            if (ui_btn_process_touch(&data->btn_fs_refresh, is_touching, tx, ty, now_tick)) return true;
            if (ui_file_browser_process_touch(&data->file_browser, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, data->file_browser.rect);
                return true;
            }
            break;

        case TAB_SD_TEST:
            if (ui_btn_process_touch(&data->btn_detect, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_benchmark, is_touching, tx, ty, now_tick)) return true;
            break;

        case TAB_USB_MSC:
            if (ui_switch_process_touch(&data->sw_msc_en, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_msc_en));
                return true;
            }
            if (ui_switch_process_touch(&data->sw_auto_flash, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_auto_flash));
                return true;
            }
            break;
    }
    return false;
}

static void app_sd_draw(app_t *self, ui_tile_t *tile) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case TAB_SD_BROWSER: {
            if (data->view_mode == VIEW_MODE_FLASHING) {
                if (ui_rect_intersect(&tile->rect, &data->flash_card, &overlap)) {
                    ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                    ui_draw_rect(tile, &data->flash_card, 0xC618);

                    ui_draw_string(tile, 140, 50, "固件脱机烧录 (RAM 算法)", FONT_20, 0x1A7B, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "固件: %s", data->selected_file_name);
                    ui_draw_string(tile, 140, 80, str_buf, FONT_16, 0x03DF, 0xEF7D);

                    const flasher_status_t *st = swd_flasher_get_status();
                    snprintf(str_buf, sizeof(str_buf), "后端: %s  %s",
                             st->using_pio ? "PIO 10MHz" : "未连接",
                             st->chip_name[0] ? st->chip_name : "STM32F10x");
                    ui_draw_string(tile, 140, 104, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    if (st->total_bytes < 10240u) {
                        snprintf(str_buf, sizeof(str_buf), "大小: %lu B", st->total_bytes);
                    } else {
                        snprintf(str_buf, sizeof(str_buf), "大小: %lu.%lu KB",
                                 st->total_bytes / 1024u,
                                 ((st->total_bytes % 1024u) * 10u) / 1024u);
                    }
                    ui_draw_string(tile, 140, 128, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "编程速度: %lu KB/s", st->speed_kbs);
                    ui_draw_string(tile, 140, 150, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    uint32_t time_x10 = st->cost_time_ms / 100u;
                    snprintf(str_buf, sizeof(str_buf), "总耗时: %lu.%lu s",
                             time_x10 / 10u, time_x10 % 10u);
                    ui_draw_string(tile, 140, 172, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    uint16_t status_color = (st->stage == FLASHER_STAGE_SUCCESS) ? 0x05E0 : ((st->stage == FLASHER_STAGE_ERROR) ? 0xF800 : 0x1A7B);
                    snprintf(str_buf, sizeof(str_buf), "状态: %s", st->status_msg);
                    ui_draw_string(tile, 140, 194, str_buf, FONT_16, status_color, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "%u%%", st->progress_percent);
                    ui_draw_string(tile, 415, 220, str_buf, FONT_16, 0x03DF, 0xEF7D);
                }

                ui_progressbar_draw(tile, &data->prog_bar);
                ui_btn_draw(tile, &data->btn_start_flash);
                ui_btn_draw(tile, &data->btn_close_flash);

            } else if (data->view_mode == VIEW_MODE_HEX_PREVIEW) {
                int16_t path_y = 42;
                if ((path_y + 20) > tile->rect.y && path_y < (tile->rect.y + tile->rect.h)) {
                    snprintf(str_buf, sizeof(str_buf), "查看: %s (%u B)", data->selected_file_name, data->hex_buf_len);
                    ui_draw_string(tile, 125, path_y, str_buf, FONT_16, 0x05E0, 0xFFFF);
                }
                ui_btn_draw(tile, &data->btn_close_hex);
                ui_hex_view_draw(tile, &data->hex_view);
            } else {
                int16_t path_y = 42;
                if ((path_y + 20) > tile->rect.y && path_y < (tile->rect.y + tile->rect.h)) {
                    snprintf(str_buf, sizeof(str_buf), "路径: %s", data->current_path);
                    ui_draw_string(tile, 125, path_y, str_buf, FONT_16, 0x1A7B, 0xFFFF);
                }
                ui_btn_draw(tile, &data->btn_fs_refresh);
                ui_file_browser_draw(tile, &data->file_browser);
            }
            break;
        }

        case TAB_SD_TEST: {
            if (ui_rect_intersect(&tile->rect, &data->info_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->info_card, 0xC618);

                snprintf(str_buf, sizeof(str_buf), "状态: %s", data->state.status_msg);
                ui_draw_string(tile, 140, 54, str_buf, FONT_16, data->state.is_detected ? 0x05E0 : 0xF800, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "容量: %lu MB (%s)", data->state.capacity_mb, data->state.card_type);
                ui_draw_string(tile, 140, 78, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "扇区数: %lu", data->state.sector_count);
                ui_draw_string(tile, 140, 102, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                if (data->state.test_done) {
                    snprintf(str_buf, sizeof(str_buf), "写入: %lu KB/s", data->state.write_speed_kbs);
                    ui_draw_string(tile, 140, 130, str_buf, FONT_16, 0x03DF, 0xEF7D);
                    snprintf(str_buf, sizeof(str_buf), "读取: %lu KB/s", data->state.read_speed_kbs);
                    ui_draw_string(tile, 270, 130, str_buf, FONT_16, 0x05E0, 0xEF7D);
                } else {
                    ui_draw_string(tile, 140, 130, "点击下方按钮进行 4KB 连续读写测速", FONT_16, 0x8410, 0xEF7D);
                }
            }
            ui_btn_draw(tile, &data->btn_detect);
            ui_btn_draw(tile, &data->btn_benchmark);
            break;
        }

        case TAB_USB_MSC: {
            if (ui_rect_intersect(&tile->rect, &data->msc_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->msc_card, 0x07FF);

                if (data->msc_active) {
                    ui_draw_string(tile, 140, 54, "● USB 模式: [ 挂载中 ]", FONT_16, 0x07FF, 0x10A2);
                    if (data->auto_flash_en) {
                        ui_draw_string(tile, 140, 80, "特性: [在线拖拽烧录] 已激活", FONT_16, 0x05E0, 0x10A2);
                        ui_draw_string(tile, 140, 106, "拖入 .hex/.bin 文件将自动烧写芯片", FONT_16, 0xFFFF, 0x10A2);
                    } else {
                        ui_draw_string(tile, 140, 80, "特性: [普通移动磁盘] 模式", FONT_16, 0x8410, 0x10A2);
                        ui_draw_string(tile, 140, 106, "仅执行常规文件存取，不触发烧录", FONT_16, 0xFFFF, 0x10A2);
                    }
                } else {
                    ui_draw_string(tile, 140, 54, "○ USB 模式: [ 已断开 ]", FONT_16, 0x8410, 0x10A2);
                    ui_draw_string(tile, 140, 80, "开启下方开关后，电脑将识别为 U 盘", FONT_16, 0xFFFF, 0x10A2);
                    ui_draw_string(tile, 140, 106, "请确认 TF 卡已插好且引脚接触良好", FONT_16, 0x8410, 0x10A2);
                }
            }

            int16_t l1_y = 190;
            if ((l1_y + 20) > tile->rect.y && l1_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 140, l1_y, "启用 U 盘模式:", FONT_16, 0x1A7B, 0xFFFF);
            }
            ui_switch_draw(tile, &data->sw_msc_en);

            int16_t l2_y = 240;
            if ((l2_y + 20) > tile->rect.y && l2_y < (tile->rect.y + tile->rect.h)) {
                uint16_t color = data->msc_active ? 0x03DF : 0x8410;
                ui_draw_string(tile, 140, l2_y, "拖拽在线烧录:", FONT_16, color, 0xFFFF);
            }
            ui_switch_draw(tile, &data->sw_auto_flash);
            break;
        }

        case TAB_PINOUT: {
            ui_rect_t card = ui_rect_make(125, 44, 340, 230);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);
                ui_draw_string(tile, 140, 54, "TF 卡模块接线配置:", FONT_16, 0x03DF, 0xEF7D);
                ui_draw_string(tile, 140, 80, "MISO -> SPI1 RX (带上拉)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 106, "CS   -> PIN_TF_CS (片选)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 132, "SCK  -> SPI1 SCK (时钟)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 158, "MOSI -> SPI1 TX (主出从入)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 184, "3.3V 与 GND 必须可靠供电", FONT_16, 0xF800, 0xEF7D);
                ui_draw_string(tile, 140, 210, "建议在 VCC/GND 间加 100nF 去耦电容", FONT_16, 0x8410, 0xEF7D);
            }
            break;
        }
    }
}

app_t *app_sd_msc_get_instance(void) {
    s_app_sd.id            = APP_ID_CUSTOM_0 + 1;
    s_app_sd.name          = "TF卡/U盘";
    s_app_sd.init          = app_sd_init;
    s_app_sd.enter         = app_sd_enter;
    s_app_sd.exit          = app_sd_exit;
    s_app_sd.update        = app_sd_update;
    s_app_sd.process_touch = app_sd_process_touch;
    s_app_sd.draw          = app_sd_draw;
    return &s_app_sd;
}
