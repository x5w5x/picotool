#include "ui_progressbar.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *k_stage_names[6] = {
    "待机就绪", "扇区擦除中...", "固件烧录中...", "校验比对中...", "烧录完成 (PASS)", "烧录失败 (ERROR)"
};

static const uint16_t k_stage_colors[6] = {
    0x632C, // 待机: 灰暗
    0xFD20, // 擦除: 橙黄
    0x07E0, // 写入: 亮绿
    0x07FF, // 校验: 青蓝
    0x07E0, // 完成: 纯绿
    0xF800  // 失败: 亮红
};

void ui_progressbar_init(ui_progressbar_t *pb, int16_t x, int16_t y, int16_t w, int16_t h, bool show_details) {
    if (!pb) return;

    pb->rect = ui_rect_make(x, y, w, h);
    pb->stage = UI_PROG_STAGE_IDLE;
    pb->progress = 0;

    pb->current_bytes = 0;
    pb->total_bytes   = 0;
    pb->speed_kbps    = 0.0f;
    pb->start_time_ms = 0;
    pb->elapsed_ms    = 0;
    pb->show_details  = show_details;

    pb->color_track_bg = 0x1082; // 极深蓝黑底槽
    pb->color_border   = 0x39E7; // 深灰外框
    pb->color_text     = 0xFFFF; // 纯白文字

    for (int i = 0; i < 6; i++) {
        pb->color_stage[i] = k_stage_colors[i];
    }
}

void ui_progressbar_set_stage(ui_progressbar_t *pb, ui_prog_stage_t stage) {
    if (!pb || stage > UI_PROG_STAGE_ERROR) return;
    pb->stage = stage;
    if (stage == UI_PROG_STAGE_DONE) {
        pb->progress = 100;
    } else if (stage == UI_PROG_STAGE_IDLE) {
        pb->progress = 0;
    }
}

void ui_progressbar_set_progress(ui_progressbar_t *pb, uint8_t progress) {
    if (!pb) return;
    pb->progress = (progress > 100) ? 100 : progress;
}

void ui_progressbar_update_stats(ui_progressbar_t *pb, uint32_t current_bytes, uint32_t total_bytes, uint32_t now_ms) {
    if (!pb) return;

    pb->current_bytes = current_bytes;
    pb->total_bytes   = total_bytes;

    if (total_bytes > 0) {
        uint32_t prog = ((uint64_t)current_bytes * 100) / total_bytes;
        pb->progress = (prog > 100) ? 100 : (uint8_t)prog;
    }

    if (pb->start_time_ms == 0) {
        pb->start_time_ms = now_ms;
    }

    if (now_ms >= pb->start_time_ms) {
        pb->elapsed_ms = now_ms - pb->start_time_ms;
        if (pb->elapsed_ms > 50 && current_bytes > 0) {
            pb->speed_kbps = ((float)current_bytes / 1024.0f) / ((float)pb->elapsed_ms / 1000.0f);
        }
    }
}

void ui_progressbar_reset(ui_progressbar_t *pb) {
    if (!pb) return;
    pb->stage         = UI_PROG_STAGE_IDLE;
    pb->progress      = 0;
    pb->current_bytes = 0;
    pb->total_bytes   = 0;
    pb->speed_kbps    = 0.0f;
    pb->start_time_ms = 0;
    pb->elapsed_ms    = 0;
}

/* ================= 切片光栅化渲染 (行级算术剪枝) ================= */

void ui_progressbar_draw(ui_tile_t *tile, const ui_progressbar_t *pb) {
    if (!tile || !pb) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &pb->rect, &overlap)) return;

    ui_draw_set_clip(&pb->rect);

    // 1. 顶部阶段状态标题行 (高度 20px)
    if (pb->show_details) {
        int16_t header_y = pb->rect.y + 2;
        if ((header_y + 16) > tile->rect.y && header_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_string(tile, pb->rect.x + 2, header_y, "烧录状态:", FONT_16, 0x07FF, 0x0000);
            ui_draw_string(tile, pb->rect.x + 80, header_y, k_stage_names[pb->stage], FONT_16, pb->color_stage[pb->stage], 0x0000);
        }
    }

    // 2. 中间进度条槽体 (高度 22px)
    int16_t bar_y = pb->rect.y + (pb->show_details ? 22 : 0);
    int16_t bar_h = pb->show_details ? 22 : pb->rect.h;
    ui_rect_t bar_rect = ui_rect_make(pb->rect.x, bar_y, pb->rect.w, bar_h);

    ui_rect_t bar_ov;
    if (ui_rect_intersect(&tile->rect, &bar_rect, &bar_ov)) {
        // 绘制底槽与外框
        ui_draw_fill_rect(tile, &bar_ov, pb->color_track_bg);
        ui_draw_rect(tile, &bar_rect, pb->color_border);

        // 绘制动态前进的填充条
        if (pb->progress > 0) {
            int16_t fill_w = (int16_t)(((int32_t)(bar_rect.w - 4) * pb->progress) / 100);
            if (fill_w < 2) fill_w = 2;
            ui_rect_t fill_rect = ui_rect_make(bar_rect.x + 2, bar_rect.y + 2, fill_w, bar_rect.h - 4);

            ui_rect_t fill_ov;
            if (ui_rect_intersect(&tile->rect, &fill_rect, &fill_ov)) {
                ui_draw_fill_rect(tile, &fill_ov, pb->color_stage[pb->stage]);
            }
        }

        // 进度条内部居中百分比数字
        int16_t text_y = bar_rect.y + (bar_rect.h - 16) / 2;
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
            char pct_str[8];
            snprintf(pct_str, sizeof(pct_str), "%d%%", pb->progress);
            int16_t str_w = (int16_t)strlen(pct_str) * 8;
            int16_t text_x = bar_rect.x + (bar_rect.w - str_w) / 2;
            ui_draw_string(tile, text_x, text_y, pct_str, FONT_16, 0xFFFF, pb->color_track_bg);
        }
    }

    // 3. 底部详细传输看板行 (高度 18px)
    if (pb->show_details) {
        int16_t stats_y = bar_rect.y + bar_rect.h + 8;
        if ((stats_y + 16) > tile->rect.y && stats_y < (tile->rect.y + tile->rect.h)) {
            char buf_size[32];
            char buf_speed[32];
            char buf_time[32];

            if (pb->total_bytes > 0) {
                snprintf(buf_size, sizeof(buf_size), "大小:%lu/%luB", 
                         (unsigned long)pb->current_bytes, (unsigned long)pb->total_bytes);
            } else {
                snprintf(buf_size, sizeof(buf_size), "大小: -- / --");
            }

            snprintf(buf_speed, sizeof(buf_speed), "速率:%.1fKB/s", pb->speed_kbps);
            snprintf(buf_time, sizeof(buf_time), "耗时:%.1fs", (float)pb->elapsed_ms / 1000.0f);

            ui_draw_string(tile, pb->rect.x + 2,   stats_y, buf_size,  FONT_16, 0x8410, 0x0000);
            ui_draw_string(tile, pb->rect.x + 200, stats_y, buf_speed, FONT_16, 0x07E0, 0x0000);
            ui_draw_string(tile, pb->rect.x + 320, stats_y, buf_time,  FONT_16, 0xFFE0, 0x0000);
        }
    }

    ui_draw_reset_clip();
}