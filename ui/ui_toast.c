#include "ui_toast.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>

#define TOAST_ANIM_MS 350 // 入场与出场动画耗时 150ms

void ui_toast_init(ui_toast_t *toast) {
    if (!toast) return;
    toast->is_active   = false;
    toast->message[0]  = '\0';
    toast->type        = UI_TOAST_INFO;
    toast->start_tick  = 0;
    toast->duration_ms = 1500; // 150ms滑入 + 1700ms驻留 + 150ms滑出

    toast->w = 240;
    toast->h = 34;
    // 屏幕右下角静止坐标 (屏幕 480x320, 距右边 8px, 距底边 10px)
    toast->target_x = 480 - toast->w - 8; // 232
    toast->target_y = 320 - toast->h - 10; // 276

    toast->rect     = ui_rect_make(480, toast->target_y, toast->w, toast->h);
    toast->old_rect = toast->rect;
}

void ui_toast_show(ui_toast_t *toast, const char *msg, ui_toast_type_t type, uint32_t now_tick) {
    if (!toast || !msg) return;
    toast->is_active  = true;
    toast->type       = type;
    toast->start_tick = now_tick;
    strncpy(toast->message, msg, sizeof(toast->message) - 1);
    toast->message[sizeof(toast->message) - 1] = '\0';

    // 初始位置设定在屏幕右侧外部
    toast->old_rect = toast->rect;
    toast->rect     = ui_rect_make(480, toast->target_y, toast->w, toast->h);
}

bool ui_toast_update(ui_toast_t *toast, uint32_t now_tick) {
    if (!toast || !toast->is_active) return false;

    uint32_t elapsed = now_tick - toast->start_tick;
    toast->old_rect  = toast->rect; // 备份上一帧位置
    int16_t cur_x    = toast->target_x;

    // 1. 超时关闭
    if (elapsed >= toast->duration_ms) {
        toast->is_active = false;
        toast->rect.x    = 480;
        return true; // 触发最后一次擦除重绘
    }

    // 2. ⚡ 入场阶段 (0 ~ 150ms: 纯定点整数平滑插值从 480 移动至 target_x)
    if (elapsed < TOAST_ANIM_MS) {
        int32_t total_dist = 480 - toast->target_x;
        cur_x = (int16_t)(480 - (total_dist * (int32_t)elapsed) / TOAST_ANIM_MS);
    }
    // 3. ⚡ 出场阶段 (倒数 150ms: 纯定点整数插值从 target_x 移动至 480)
    else if (elapsed >= (toast->duration_ms - TOAST_ANIM_MS)) {
        uint32_t out_elapsed = elapsed - (toast->duration_ms - TOAST_ANIM_MS);
        int32_t total_dist = 480 - toast->target_x;
        cur_x = (int16_t)(toast->target_x + (total_dist * (int32_t)out_elapsed) / TOAST_ANIM_MS);
    }
    // 4. 静止驻留阶段
    else {
        cur_x = toast->target_x;
    }

    toast->rect.x = cur_x;
    return (toast->rect.x != toast->old_rect.x);
}

void ui_toast_draw(ui_tile_t *tile, const ui_toast_t *toast) {
    if (!tile || !toast || !toast->is_active) return;

    ui_rect_t overlap;
    // 1. 切片相交快速裁剪
    if (!ui_rect_intersect(&tile->rect, &toast->rect, &overlap)) return;

    uint16_t bg_col   = 0xE73C; // 浅银灰背景
    uint16_t bdr_col  = 0xC618; // 中浅灰边框
    uint16_t icon_col = 0xE73C; // 深灰状态指示
    uint16_t text_col = 0x0000; // 深灰正文
    const char *tag   = "[INFO]";

    if (toast->type == UI_TOAST_SUCCESS) {
        icon_col = 0x2945; tag = "[OK]";
    } else if (toast->type == UI_TOAST_WARN) {
        icon_col = 0x2945; tag = "[WARN]";
    } else if (toast->type == UI_TOAST_ERROR) {
        icon_col = 0x2945; tag = "[ERR]";
    }

    // 2. 填充气泡背景与发光外框
    ui_draw_fill_rect(tile, &overlap, bg_col);
    ui_draw_rect(tile, &toast->rect, bdr_col);

    // 3. 锁定视口像素级硬裁剪保护 (滑入时不溢出)
    ui_draw_set_clip(&toast->rect);

    // 左侧彩色指示条 (宽 4px)
    // ui_rect_t left_bar = ui_rect_make((int16_t)(toast->rect.x + 1), (int16_t)(toast->rect.y + 1), 2, (int16_t)(toast->rect.h - 2));
    // ui_rect_t bar_ov;
    // if (ui_rect_intersect(&tile->rect, &left_bar, &bar_ov)) {
    //     ui_draw_fill_rect(tile, &bar_ov, icon_col);
    // }

    int16_t text_y = (int16_t)(toast->rect.y + 9);
    if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {
        // 状态徽标
        ui_draw_string(tile, (int16_t)(toast->rect.x + 10), text_y, tag, FONT_16, icon_col, bg_col);

        // 提示正文文本
        ui_draw_string(tile, (int16_t)(toast->rect.x + 62), text_y, toast->message, FONT_16, text_col, bg_col);
    }

    ui_draw_reset_clip();
}
