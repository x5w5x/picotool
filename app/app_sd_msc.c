#include "app_sd_msc.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "ui_file_browser.h"
#include "ui_hex_view.h"
#include "lcd.h"
#include "chinese.h"
#include "tf.h"
#include "ff.h"
#include "f_util.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// 外部 USB 动态切换与介质使能接口
extern void usb_switch_mode(uint8_t mode);
extern void msc_set_enabled(bool enabled);

// 侧边栏 Tab 定义
enum {
    TAB_SD_BROWSER = 0, // 文件浏览与 Hex 预览
    TAB_SD_TEST    = 1, // TF 卡探测与读写测速
    TAB_USB_MSC    = 2, // 虚拟 U 盘模式
    TAB_PINOUT     = 3, // 接线说明
    TAB_SD_COUNT   = 4
};

static const char *s_sd_tabs[] = {
    "文件浏览",
    "TF卡测试",
    "虚拟U盘",
    "引脚说明"
};

// TF 卡运行状态
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
    ui_rect_t          dirty_rect;     // 必须放在第 1 位
    ui_tabview_t       tabview;
    sd_test_state_t    state;

    // FatFs 状态与路径
    FATFS              fs;
    bool               fs_mounted;
    char               current_path[256];

    // Tab 0: 文件浏览器与 Hex 查看器
    ui_file_browser_t  file_browser;
    ui_btn_t           btn_fs_refresh;
    bool               is_viewing_hex;
    ui_hex_view_t      hex_view;
    ui_btn_t           btn_close_hex;
    char               preview_file_name[64];
    uint8_t            hex_buf[512];
    UINT               hex_buf_len;

    // Tab 1: 测速与探测
    ui_rect_t          info_card;
    ui_btn_t           btn_detect;
    ui_btn_t           btn_benchmark;
    
    // Tab 2: 虚拟 U 盘
    bool               msc_active;
    ui_rect_t          msc_card;
    ui_switch_t        sw_msc_en;
} app_sd_data_t;

static app_sd_data_t s_sd_data;
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

/* ================= FatFs 目录扫描与浏览器绑定 ================= */

static void load_directory(app_sd_data_t *data, const char *path) {
    ui_file_browser_clear(&data->file_browser);

    if (!data->fs_mounted) {
        if (f_mount(&data->fs, "0:", 1) != FR_OK) {
            return;
        }
        data->fs_mounted = true;
    }

    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) return;

    // 非根目录添加上一级 ".." 选项
    if (strcmp(path, "0:") != 0 && strcmp(path, "0:/") != 0) {
        ui_file_browser_add_item(&data->file_browser, "..", 0, true);
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        ui_file_browser_add_item(&data->file_browser, fno.fname, (uint32_t)fno.fsize, is_dir);
    }
    f_closedir(&dir);
}

// 文件/目录点击响应回调
static void on_file_item_click(ui_file_browser_t *fb, int8_t index, const ui_file_item_t *item, void *user_data) {
    (void)fb;
    (void)index;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    const char *name = item->name;
    bool is_dir = (item->type == UI_FILE_TYPE_DIR);

    if (is_dir) {
        if (strcmp(name, "..") == 0) {
            // 回退上一级目录
            char *last_slash = strrchr(data->current_path, '/');
            if (last_slash && last_slash != data->current_path + 2) {
                *last_slash = '\0';
            } else {
                strcpy(data->current_path, "0:/");
            }
        } else {
            // 进入下一级目录
            if (data->current_path[strlen(data->current_path) - 1] != '/') {
                strcat(data->current_path, "/");
            }
            strcat(data->current_path, name);
        }
        load_directory(data, data->current_path);
        data->dirty_rect = data->tabview.content_rect;
    } else {
        // 点击普通文件：读取前 512 字节载入 Hex 查看器
        char full_file_path[280];
        if (strcmp(data->current_path, "0:/") == 0) {
            snprintf(full_file_path, sizeof(full_file_path), "0:/%s", name);
        } else {
            snprintf(full_file_path, sizeof(full_file_path), "%s/%s", data->current_path, name);
        }

        FIL fil;
        if (f_open(&fil, full_file_path, FA_READ) == FR_OK) {
            memset(data->hex_buf, 0, sizeof(data->hex_buf));
            f_read(&fil, data->hex_buf, sizeof(data->hex_buf), &data->hex_buf_len);
            f_close(&fil);

            ui_hex_view_bind(&data->hex_view, data->hex_buf, data->hex_buf_len, 0x0000);
            strncpy(data->preview_file_name, name, sizeof(data->preview_file_name) - 1);
            data->preview_file_name[sizeof(data->preview_file_name) - 1] = '\0';

            data->is_viewing_hex = true;
            data->dirty_rect = data->tabview.content_rect;
        }
    }
}

static void on_close_hex_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    data->is_viewing_hex = false;
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= 业务动作：探测与读写基准测试 ================= */

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
    if (!st->is_detected) {
        if (TF_Init() != TF_OK) return;
        st->is_detected = true;
    }

    static uint8_t test_buf[4096];
    memset(test_buf, 0x5A, sizeof(test_buf));

    uint32_t test_sectors = 512;
    uint32_t start_sec = 2048;

    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < test_sectors; i += 8) {
        if (TF_WriteBlocks(start_sec + i, test_buf, 8) != TF_OK) {
            strcpy(st->status_msg, "写入测试出错");
            return;
        }
    }
    uint64_t write_time_us = time_us_64() - t0;
    st->write_speed_kbs = (256 * 1000000ULL) / (write_time_us ? write_time_us : 1);

    t0 = time_us_64();
    for (uint32_t i = 0; i < test_sectors; i += 8) {
        if (TF_ReadBlocks(start_sec + i, test_buf, 8) != TF_OK) {
            strcpy(st->status_msg, "读取测试出错");
            return;
        }
    }
    uint64_t read_time_us = time_us_64() - t0;
    st->read_speed_kbs = (256 * 1000000ULL) / (read_time_us ? read_time_us : 1);

    st->test_done = true;
    strcpy(st->status_msg, "测速完成");
}

/* ================= UI 回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_sd_data_t *data = (app_sd_data_t *)user_data;

    data->is_viewing_hex = false;
    if (new_tab == TAB_SD_BROWSER) {
        load_directory(data, data->current_path);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_fs_refresh_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    load_directory(data, data->current_path);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_detect_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    do_card_detect(&data->state);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_benchmark_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    do_card_benchmark(&data->state);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_msc_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_sd_data_t *data = (app_sd_data_t *)user_data;
    data->msc_active = state;
    msc_set_enabled(state);

    if (state) {
        if (data->fs_mounted) {
            f_mount(NULL, "0:", 1);
            data->fs_mounted = false;
        }
        usb_switch_mode(1); // 切换为 MSC 模式，挂载 U 盘
    } else {
        usb_switch_mode(0); // 切回默认模式，卸载盘符
    }
    data->dirty_rect = data->msc_card;
}

/* ================= 生命周期与绘制 ================= */

static void app_sd_init(app_t *self) {
    self->user_data = &s_sd_data;
    app_sd_data_t *data = &s_sd_data;
    memset(data, 0, sizeof(app_sd_data_t));

    strcpy(data->current_path, "0:/");

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_sd_tabs, TAB_SD_COUNT, TAB_SD_BROWSER, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0: 文件浏览器与 Hex 查看器控件
    ui_file_browser_init(&data->file_browser, 120, 72, 350, 236, on_file_item_click);
    data->file_browser.user_data = data;

    ui_btn_init(&data->btn_fs_refresh, 390, 36, 75, 30, "刷新", on_fs_refresh_click);
    data->btn_fs_refresh.user_data = data;
    data->btn_fs_refresh.style     = UI_BTN_STYLE_ROUND;
    data->btn_fs_refresh.font_size = FONT_16;
    ui_btn_set_color(&data->btn_fs_refresh, 0x03DF, 0x0275, 0xFFFF);

    ui_hex_view_init(&data->hex_view, 120, 72, 350, 236, NULL);
    data->hex_view.user_data = data;

    ui_btn_init(&data->btn_close_hex, 390, 36, 75, 30, "关闭", on_close_hex_click);
    data->btn_close_hex.user_data = data;
    data->btn_close_hex.style     = UI_BTN_STYLE_ROUND;
    data->btn_close_hex.font_size = FONT_16;
    ui_btn_set_color(&data->btn_close_hex, 0xF800, 0xB000, 0xFFFF);

    // Tab 1: 探测与基准测试
    data->info_card = ui_rect_make(125, 44, 340, 160);
    ui_btn_init(&data->btn_detect, 130, 220, 150, 42, "探测 TF 卡", on_detect_click);
    data->btn_detect.user_data = data;
    data->btn_detect.style     = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_detect, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_benchmark, 305, 220, 150, 42, "读写测速", on_benchmark_click);
    data->btn_benchmark.user_data = data;
    data->btn_benchmark.style     = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_benchmark, 0x05E0, 0x03E0, 0xFFFF);

    // Tab 2: 虚拟 U 盘
    data->msc_card = ui_rect_make(125, 44, 340, 140);
    ui_switch_init(&data->sw_msc_en, 385, 205, 64, 32, false, on_msc_switch);
    data->sw_msc_en.user_data = data;
}

static void app_sd_enter(app_t *self) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->is_viewing_hex = false;
    load_directory(data, data->current_path);
}

static void app_sd_exit(app_t *self) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
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

static void app_sd_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
}

static bool app_sd_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_tabview_get_bar_dirty_rect(&data->tabview));
        return true;
    }

    switch (data->tabview.active_tab_idx) {
        case TAB_SD_BROWSER:
            if (data->is_viewing_hex) {
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
            break;
    }
    return false;
}

static void app_sd_draw(app_t *self, ui_tile_t *tile) {
    app_sd_data_t *data = (app_sd_data_t *)self->user_data;
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
            if (data->is_viewing_hex) {
                int16_t path_y = 42;
                if ((path_y + 20) > tile->rect.y && path_y < (tile->rect.y + tile->rect.h)) {
                    char title[80];
                    snprintf(title, sizeof(title), "查看: %s (%u B)", data->preview_file_name, data->hex_buf_len);
                    ui_draw_string(tile, 125, path_y, title, FONT_16, 0x05E0, 0xFFFF);
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
                    ui_draw_string(tile, 140, 56, "● USB 读卡器模式: [ 挂载中 ]", FONT_16, 0x07FF, 0x10A2);
                    ui_draw_string(tile, 140, 84, "协议标准: USB MSC Bulk-Only", FONT_16, 0xFFFF, 0x10A2);
                    ui_draw_string(tile, 140, 112, "电脑端可直接读取 TF 盘符并存入文件", FONT_16, 0x05E0, 0x10A2);
                } else {
                    ui_draw_string(tile, 140, 56, "○ USB 读卡器模式: [ 已断开 ]", FONT_16, 0x8410, 0x10A2);
                    ui_draw_string(tile, 140, 84, "开启开关后，Pico 将模拟为高速 U 盘", FONT_16, 0xFFFF, 0x10A2);
                    ui_draw_string(tile, 140, 112, "请确认 TF 卡已插入且引脚接触良好", FONT_16, 0x8410, 0x10A2);
                }
            }

            int16_t lbl_y = 210;
            if ((lbl_y + 20) > tile->rect.y && lbl_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, 140, lbl_y, "启用 U 盘模式:", FONT_20, 0x1A7B, 0xFFFF);
            }
            ui_switch_draw(tile, &data->sw_msc_en);
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