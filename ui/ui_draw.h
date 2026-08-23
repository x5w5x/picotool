/*
 * @Author: 轩
 * @Date: 2026-08-19 17:55:44
 * @LastEditTime: 2026-08-22 16:39:46
 * @FilePath: \pico\ui\ui_draw.h
 */
#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ================= 1. 全局视口裁剪 API ================= */
void ui_draw_set_clip(const ui_rect_t *clip);
void ui_draw_reset_clip(void);
ui_rect_t ui_draw_get_clip(void);

/* ================= 2. 基础图元光栅化 API ================= */
void ui_draw_point(ui_tile_t *tile, int16_t x, int16_t y, uint16_t color);
void ui_draw_fill_rect(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color);
void ui_draw_rect(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color);
void ui_draw_fast_hline(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, uint16_t color);
void ui_draw_fast_vline(ui_tile_t *tile, int16_t x, int16_t y, int16_t h, uint16_t color);
void ui_draw_line(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void ui_draw_circle(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t r, uint16_t color);
void ui_draw_fill_circle(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t r, uint16_t color);

/* ================= 3. 点阵与文本图元 API ================= */
void ui_draw_bitmap(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, 
                    const uint8_t *bitmap, uint16_t color, uint16_t bg_color);
void ui_draw_char(ui_tile_t *tile, int16_t x, int16_t y, char c, 
                  FontSize_t size, uint16_t color, uint16_t bg);
void ui_draw_string(ui_tile_t *tile, int16_t x, int16_t y, const char *str, 
                    FontSize_t size, uint16_t color, uint16_t bg);


#ifdef __cplusplus
}
#endif

#endif // UI_DRAW_H