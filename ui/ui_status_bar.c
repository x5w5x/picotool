#include "ui_status_bar.h"
#include "ui_draw.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOUCH_SLOP_PX 8

/* ================= 1. 内置微型纯矢量硬件图标引擎 ================= */

// 1. 绘制 W25Qxx 外部 SPI Flash 芯片矢量图标 (宽 18px, 高 12px)
static void draw_vector_w25(ui_tile_t *tile, int16_t x, int16_t y, bool is_ok, uint16_t col_act, uint16_t col_inact)
{
    uint16_t color = is_ok ? col_act : col_inact;

    // SOP8 芯片主体封装 (12x10)
    ui_rect_t chip_body = ui_rect_make((int16_t)(x + 3), (int16_t)(y + 1), 12, 10);
    ui_draw_rect(tile, &chip_body, color);

    // 引脚 1 方向缺口圆点
    ui_draw_point(tile, x + 5, y + 3, color);

    // 左右各 4 根引脚焊盘
    for (int i = 0; i < 4; i++)
    {
        int16_t pin_y = (int16_t)(y + 2 + i * 2);
        ui_draw_point(tile, x + 1, pin_y, color);
        ui_draw_point(tile, x + 2, pin_y, color);
        ui_draw_point(tile, x + 15, pin_y, color);
        ui_draw_point(tile, x + 16, pin_y, color);
    }
}

// 2. 绘制 MicroSD 卡矢量图标 (宽 12px, 高 12px)
static void draw_vector_sd(ui_tile_t *tile, int16_t x, int16_t y, bool mounted, uint16_t col_act, uint16_t col_inact)
{
    uint16_t color = mounted ? col_act : col_inact;

    ui_rect_t sd_rect = ui_rect_make(x, y, 11, 12);
    ui_draw_rect(tile, &sd_rect, color);

    // 右上缺角
    ui_draw_point(tile, x + 10, y, 0x0000);
    ui_draw_point(tile, x + 9, y, 0x0000);

    // 金手指打点
    ui_draw_fast_vline(tile, x + 2, (int16_t)(y + 2), 3, color);
    ui_draw_fast_vline(tile, x + 4, (int16_t)(y + 2), 3, color);
    ui_draw_fast_vline(tile, x + 6, (int16_t)(y + 2), 3, color);
    ui_draw_fast_vline(tile, x + 8, (int16_t)(y + 3), 2, color);
}

// 3. 绘制 Touch 触摸靶心图标 (宽 14px, 高 14px)
static void draw_vector_touch(ui_tile_t *tile, int16_t x, int16_t y, bool is_active, uint16_t col_act, uint16_t col_inact)
{
    int16_t cx = x + 7;
    int16_t cy = y + 6;
    uint16_t color = is_active ? col_act : col_inact;

    // 外圈触控圆环 (R=5)
    ui_draw_circle(tile, cx, cy, 5, color);

    // 中心触控点 (按下时实心亮橙，未按下时为空心十字靶心)
    if (is_active)
    {
        ui_draw_fill_circle(tile, cx, cy, 2, color);
    }
    else
    {
        ui_draw_point(tile, cx, cy, color);
        ui_draw_point(tile, cx - 3, cy, color);
        ui_draw_point(tile, cx + 3, cy, color);
        ui_draw_point(tile, cx, cy - 3, color);
        ui_draw_point(tile, cx, cy + 3, color);
    }
}

// 4. 绘制 USB 接口矢量图标 (宽 14px, 高 12px)
static void draw_vector_usb(ui_tile_t *tile, int16_t x, int16_t y, bool connected, uint16_t col_act, uint16_t col_inact)
{
    uint16_t color = connected ? col_act : col_inact;

    ui_draw_fast_vline(tile, x + 7, (int16_t)(y + 2), 7, color);
    ui_draw_fast_hline(tile, x + 3, (int16_t)(y + 4), 8, color);
    ui_draw_point(tile, x + 3, y + 3, color);
    ui_draw_point(tile, x + 10, y + 3, color);
    ui_draw_fill_circle(tile, x + 7, y + 10, 2, color);
}

/* ================= 2. 初始化与配置 ================= */

void ui_status_bar_init(ui_status_bar_t *sb, int16_t x, int16_t y, int16_t w, int16_t h,
                        const char *title, ui_status_bar_click_cb_t on_click)
{
    if (!sb)
        return;

    sb->rect = ui_rect_make(x, y, w, h);
    strncpy(sb->title, title ? title : "MiniRTOS System", sizeof(sb->title) - 1);
    sb->title[sizeof(sb->title) - 1] = '\0';

    sb->cpu_usage = 0;
    sb->fps = 60;
    sb->chip_temp=0;

    sb->w25_ok = true;
    sb->sd_mounted = false;
    sb->touch_active = false;
    sb->usb_connected = true;

    sb->down_x = 0;
    sb->down_y = 0;
    sb->is_touch_down = false;
    sb->is_dragged = false;

    sb->color_bg = 0xF7BE;            // 极浅灰白底 (与 0xFFFF 纯白形成轻微层级对比)
    sb->color_border = 0xC618;        // 精致浅灰下边框
    sb->color_title = 0x0000;         // 纯黑标题文字
    sb->color_telemetry = 0x03DF;     // 科技纯蓝 CPU/FPS 数值
    sb->color_icon_active = 0x05E0;   // 翡翠绿硬件激活指示
    sb->color_icon_inactive = 0xAD55; // 浅灰未激活指示
    sb->color_icon_touch = 0xF940;    // 活力橙触控靶心指示

    sb->on_click = on_click;
    sb->user_data = NULL;
}

void ui_status_bar_set_title(ui_status_bar_t *sb, const char *title)
{
    if (!sb || !title)
        return;
    strncpy(sb->title, title, sizeof(sb->title) - 1);
    sb->title[sizeof(sb->title) - 1] = '\0';
}

void ui_status_bar_set_telemetry(ui_status_bar_t *sb, uint8_t cpu_usage, int16_t temp,uint8_t fps)
{
    if (!sb)
        return;
    sb->cpu_usage = (cpu_usage > 100) ? 100 : cpu_usage;
    sb->fps = (fps > 99) ? 99 : fps;
    sb->chip_temp=temp;
}

void ui_status_bar_set_w25(ui_status_bar_t *sb, bool is_ok)
{
    if (sb)
        sb->w25_ok = is_ok;
}

void ui_status_bar_set_sd(ui_status_bar_t *sb, bool is_mounted)
{
    if (sb)
        sb->sd_mounted = is_mounted;
}

void ui_status_bar_set_touch(ui_status_bar_t *sb, bool is_active)
{
    if (sb)
        sb->touch_active = is_active;
}

void ui_status_bar_set_usb(ui_status_bar_t *sb, bool is_connected)
{
    if (sb)
        sb->usb_connected = is_connected;
}

/* ================= 3. 触控状态机 (区域识别与点选) ================= */

bool ui_status_bar_process_touch(ui_status_bar_t *sb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    (void)now_tick;
    if (!sb)
        return false;

    if (is_touching)
    {
        if (!sb->is_touch_down)
        {
            if (ui_rect_contains_point(&sb->rect, tx, ty))
            {
                sb->is_touch_down = true;
                sb->is_dragged = false;
                sb->down_x = tx;
                sb->down_y = ty;
            }
        }
        else
        {
            int16_t dx = abs(tx - sb->down_x);
            int16_t dy = abs(ty - sb->down_y);
            if (!sb->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX))
            {
                sb->is_dragged = true;
            }
        }
        return false;
    }
    else
    {
        if (sb->is_touch_down)
        {
            sb->is_touch_down = false;

            if (!sb->is_dragged && ui_rect_contains_point(&sb->rect, tx, ty))
            {
                ui_sb_hit_t hit = UI_SB_HIT_NONE;

                if (tx < (sb->rect.x + 180))
                {
                    hit = UI_SB_HIT_TITLE;
                }
                else if (tx >= (sb->rect.x + 190) && tx <= (sb->rect.x + 335))
                {
                    hit = UI_SB_HIT_TELEMETRY;
                }
                else if (tx > (sb->rect.x + 340) && tx <= (sb->rect.x + 375))
                {
                    hit = UI_SB_HIT_W25;
                }
                else if (tx > (sb->rect.x + 375) && tx <= (sb->rect.x + 410))
                {
                    hit = UI_SB_HIT_SD;
                }
                else if (tx > (sb->rect.x + 410) && tx <= (sb->rect.x + 445))
                {
                    hit = UI_SB_HIT_TOUCH;
                }
                else if (tx > (sb->rect.x + 445))
                {
                    hit = UI_SB_HIT_USB;
                }

                if (hit != UI_SB_HIT_NONE && sb->on_click)
                {
                    sb->on_click(sb, hit, sb->user_data);
                }
                sb->is_dragged = false;
                return true;
            }
            sb->is_dragged = false;
        }
    }

    return false;
}

/* ================= 4. 切片光栅化渲染 (微秒级推流) ================= */

void ui_status_bar_draw(ui_tile_t *tile, const ui_status_bar_t *sb)
{
    if (!tile || !sb)
        return;

    ui_rect_t overlap;
    // 仅与处于屏幕最顶部的切片行相交 (如 Tile 0, Y: 0~31)
    if (!ui_rect_intersect(&tile->rect, &sb->rect, &overlap))
    {
        return;
    }

    // 1. 擦除背景与底部分割线
    ui_draw_fill_rect(tile, &overlap, sb->color_bg);
    ui_draw_fast_hline(tile, sb->rect.x, (int16_t)(sb->rect.y + sb->rect.h - 1), sb->rect.w, sb->color_border);

    int16_t text_y = sb->rect.y + (sb->rect.h - 16) / 2;
    int16_t icon_y = sb->rect.y + (sb->rect.h - 12) / 2;

    if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
    {
        // 2. 左侧系统标题 (支持中英文)
        ui_draw_string(tile, sb->rect.x + 8, text_y, sb->title, FONT_16, sb->color_title, sb->color_bg);

        // 3. 性能遥测 (CPU% 与 真实物理 FPS)
        char telem_buf[32];
        snprintf(telem_buf, sizeof(telem_buf), "CPU:%2u%% TEMP:%dC FPS:%-2u", sb->cpu_usage,sb->chip_temp, sb->fps);
        ui_draw_string(tile, sb->rect.x + 150, text_y, telem_buf, FONT_16, sb->color_telemetry, sb->color_bg);
    }

    // 4. 右侧硬件外设状态图标列 (W25Qxx -> SD -> Touch -> USB)
    draw_vector_w25(tile, (int16_t)(sb->rect.x + 352), icon_y, sb->w25_ok, sb->color_icon_active, sb->color_icon_inactive);
    draw_vector_sd(tile, (int16_t)(sb->rect.x + 386), icon_y, sb->sd_mounted, sb->color_icon_active, sb->color_icon_inactive);
    draw_vector_touch(tile, (int16_t)(sb->rect.x + 418), (int16_t)(icon_y - 1), sb->touch_active, sb->color_icon_touch, sb->color_icon_inactive);
    draw_vector_usb(tile, (int16_t)(sb->rect.x + 450), icon_y, sb->usb_connected, sb->color_icon_active, sb->color_icon_inactive);
}