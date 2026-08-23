#include "ui_draw.h"
#include "lcd.h"
#include <stdlib.h>
#include <string.h>

/* ================= 1. 全局裁剪视口与基础打点 ================= */

static ui_rect_t g_clip_rect = {0, 0, LCD_WIDTH, LCD_HEIGHT};

void ui_draw_set_clip(const ui_rect_t *clip) {
    if (clip) {
        g_clip_rect = *clip;
    } else {
        ui_draw_reset_clip();
    }
}

void ui_draw_reset_clip(void) {
    g_clip_rect.x = 0;
    g_clip_rect.y = 0;
    g_clip_rect.w = LCD_WIDTH;
    g_clip_rect.h = LCD_HEIGHT;
}

ui_rect_t ui_draw_get_clip(void) {
    return g_clip_rect;
}

void ui_draw_point(ui_tile_t *tile, int16_t x, int16_t y, uint16_t color) {
    if (!tile || !tile->buf) return;

    // 1. 全局视口裁剪判定
    if (x < g_clip_rect.x || x >= (g_clip_rect.x + g_clip_rect.w) ||
        y < g_clip_rect.y || y >= (g_clip_rect.y + g_clip_rect.h)) {
        return;
    }

    // 2. 当前切片内存边界判定
    if (!ui_rect_contains_point(&tile->rect, x, y)) {
        return;
    }

    int16_t lx = x - tile->rect.x;
    int16_t ly = y - tile->rect.y;
    tile->buf[ly * UI_TILE_W + lx] = color;
}

/* ================= 2. 几何图元快速光栅化 ================= */

void ui_draw_fill_rect(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color) {
    if (!tile || !tile->buf || !rect || rect->w <= 0 || rect->h <= 0) return;

    // 视口交集
    ui_rect_t clip_v;
    if (!ui_rect_intersect(rect, &g_clip_rect, &clip_v)) return;

    // 切片交集
    ui_rect_t clip_t;
    if (!ui_rect_intersect(&tile->rect, &clip_v, &clip_t)) return;

    for (int16_t y = clip_t.y; y < clip_t.y + clip_t.h; y++) {
        int16_t ly = y - tile->rect.y;
        uint16_t *line = &tile->buf[ly * UI_TILE_W + (clip_t.x - tile->rect.x)];
        for (int16_t x = 0; x < clip_t.w; x++) {
            line[x] = color;
        }
    }
}

void ui_draw_fast_hline(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, uint16_t color) {
    ui_rect_t r = ui_rect_make(x, y, w, 1);
    ui_draw_fill_rect(tile, &r, color);
}

void ui_draw_fast_vline(ui_tile_t *tile, int16_t x, int16_t y, int16_t h, uint16_t color) {
    ui_rect_t r = ui_rect_make(x, y, 1, h);
    ui_draw_fill_rect(tile, &r, color);
}

void ui_draw_rect(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color) {
    if (!tile || !rect || rect->w <= 0 || rect->h <= 0) return;
    ui_draw_fast_hline(tile, rect->x, rect->y, rect->w, color);
    ui_draw_fast_hline(tile, rect->x, rect->y + rect->h - 1, rect->w, color);
    ui_draw_fast_vline(tile, rect->x, rect->y, rect->h, color);
    ui_draw_fast_vline(tile, rect->x + rect->w - 1, rect->y, rect->h, color);
}

void ui_draw_line(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    if (!tile) return;

    // 线段包围盒与切片求交，不相交直接跳过
    int16_t min_x = (x0 < x1) ? x0 : x1;
    int16_t max_x = (x0 > x1) ? x0 : x1;
    int16_t min_y = (y0 < y1) ? y0 : y1;
    int16_t max_y = (y0 > y1) ? y0 : y1;
    ui_rect_t box = ui_rect_make(min_x, min_y, (int16_t)(max_x - min_x + 1), (int16_t)(max_y - min_y + 1));
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &box, &ov)) return;

    int16_t dx = abs(x1 - x0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t dy = -abs(y1 - y0);
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx + dy;

    while (1) {
        ui_draw_point(tile, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ui_draw_circle(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    if (!tile || r <= 0) return;

    ui_rect_t box = ui_rect_make((int16_t)(x0 - r), (int16_t)(y0 - r), (int16_t)(2 * r + 1), (int16_t)(2 * r + 1));
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &box, &ov)) return;

    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;

    while (x <= y) {
        ui_draw_point(tile, x0 + x, y0 + y, color);
        ui_draw_point(tile, x0 - x, y0 + y, color);
        ui_draw_point(tile, x0 + x, y0 - y, color);
        ui_draw_point(tile, x0 - x, y0 - y, color);
        ui_draw_point(tile, x0 + y, y0 + x, color);
        ui_draw_point(tile, x0 - y, y0 + x, color);
        ui_draw_point(tile, x0 + y, y0 - x, color);
        ui_draw_point(tile, x0 - y, y0 - x, color);

        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void ui_draw_fill_circle(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    if (!tile || r <= 0) return;

    ui_rect_t box = ui_rect_make((int16_t)(x0 - r), (int16_t)(y0 - r), (int16_t)(2 * r + 1), (int16_t)(2 * r + 1));
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &box, &ov)) return;

    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;

    while (x <= y) {
        ui_draw_fast_hline(tile, x0 - x, y0 + y, 2 * x + 1, color);
        ui_draw_fast_hline(tile, x0 - x, y0 - y, 2 * x + 1, color);
        ui_draw_fast_hline(tile, x0 - y, y0 + x, 2 * y + 1, color);
        ui_draw_fast_hline(tile, x0 - y, y0 - x, 2 * y + 1, color);

        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* ================= 3. 点阵与文本优化光栅化 ================= */

void ui_draw_bitmap(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, 
                    const uint8_t *bitmap, uint16_t color, uint16_t bg_color) {
    if (!tile || !bitmap || w <= 0 || h <= 0) return;

    ui_rect_t box = ui_rect_make(x, y, w, h);
    ui_rect_t ov_clip;
    if (!ui_rect_intersect(&box, &g_clip_rect, &ov_clip)) return;
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &ov_clip, &ov)) return;

    int16_t bytes_per_row = (w + 7) / 8;

    for (int16_t r = (ov.y - y); r < (ov.y - y + ov.h); r++) {
        for (int16_t c = (ov.x - x); c < (ov.x - x + ov.w); c++) {
            uint8_t byte_data = bitmap[r * bytes_per_row + (c / 8)];
            bool is_set = (byte_data & (0x80 >> (c % 8))) != 0;

            int16_t px = x + c;
            int16_t py = y + r;

            if (is_set) {
                ui_draw_point(tile, px, py, color);
            } else if (bg_color != color) {
                ui_draw_point(tile, px, py, bg_color);
            }
        }
    }
}

extern const uint8_t Font5x7[][5];

// void ui_draw_char(ui_tile_t *tile, int16_t x, int16_t y, char c, 
//                   FontSize_t size, uint16_t color, uint16_t bg) {
//     if (!tile) return;
//     if (c < ' ' || c > '~') c = ' ';

//     uint8_t scale = (size >= 24) ? 2 : 1;
//     uint16_t char_w = (scale == 2) ? 12 : 6;
//     uint16_t glyph_h = 8 * scale;
//     uint16_t y_offset = (size > glyph_h) ? (size - glyph_h) / 2 : 0;

//     ui_rect_t box = ui_rect_make(x, y, char_w, size);
//     ui_rect_t ov;
//     if (!ui_rect_intersect(&tile->rect, &box, &ov)) return;

//     const uint8_t *p = Font5x7[c - ' '];

//     for (uint16_t row = 0; row < size; row++) {
//         int16_t py = y + row;
//         if (py < ov.y || py >= ov.y + ov.h) continue;

//         for (uint16_t col = 0; col < char_w; col++) {
//             int16_t px = x + col;
//             if (px < ov.x || px >= ov.x + ov.w) continue;

//             bool is_fg = false;
//             if (row >= y_offset && row < y_offset + glyph_h) {
//                 uint16_t font_row = (row - y_offset) / scale;
//                 uint16_t font_col = col / scale;
//                 if (font_col < 5 && font_row < 8) {
//                     if (p[font_col] & (1 << font_row)) {
//                         is_fg = true;
//                     }
//                 }
//             }

//             if (is_fg) {
//                 ui_draw_point(tile, px, py, color);
//             } else if (bg != color) {
//                 ui_draw_point(tile, px, py, bg);
//             }
//         }
//     }
// }

// void ui_draw_string(ui_tile_t *tile, int16_t x, int16_t y, const char *str, 
//                     FontSize_t size, uint16_t color, uint16_t bg) {
//     if (!tile || !str) return;

//     int16_t cur_x = x;
//     int16_t cur_y = y;

//     uint8_t asc_scale = (size >= 24) ? 2 : 1;
//     uint16_t asc_step = (asc_scale == 2) ? 12 : 6;

//     while (*str) {
//         if ((uint8_t)*str < 0x80) {
//             char ch = *str++;
//             ui_rect_t char_box = ui_rect_make(cur_x, cur_y, asc_step, size);
//             ui_rect_t ov;
//             if (ui_rect_intersect(&tile->rect, &char_box, &ov)) {
//                 ui_draw_char(tile, cur_x, cur_y, ch, size, color, bg);
//             }
//             cur_x += asc_step;
//         } else {
//             if (*(str + 1) == '\0') break;

//             uint16_t gb = ((uint8_t)str[0] << 8) | (uint8_t)str[1];
//             str += 2;

//             ui_rect_t hz_box = ui_rect_make(cur_x, cur_y, size, size);
//             ui_rect_t ov;
//             // ⚡ 核心优化：仅当汉字范围与当前切片重叠时，才从 Flash 读取点阵数据！
//             if (ui_rect_intersect(&tile->rect, &hz_box, &ov)) {
//                 uint16_t bytes_per = 0;
//                 const uint8_t *dot_buf = GetFontBitmap(gb, size, &bytes_per);
//                 if (dot_buf) {
//                     ui_draw_bitmap(tile, cur_x, cur_y, size, size, dot_buf, color, bg);
//                 }
//             }
//             cur_x += size;
//         }
//     }
// }

void ui_draw_char(ui_tile_t *tile, int16_t x, int16_t y, char c, 
                  FontSize_t size, uint16_t color, uint16_t bg) {
    if (!tile) return;
    if (c < ' ' || c > '~') c = ' ';

    // ⚡ 核心修复：size >= 20 即开启 2 倍缩放
    uint8_t scale = (size >= 20) ? 2 : 1;
    uint16_t char_w = (size >= 24) ? 12 : ((size >= 20) ? 10 : 8);
    uint16_t glyph_h = 8 * scale; // 2 倍放大后字高为 16px
    uint16_t y_offset = (size > glyph_h) ? (size - glyph_h) / 2 : 0; // 垂直居中

    ui_rect_t box = ui_rect_make(x, y, char_w, size);
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &box, &ov)) return;

    const uint8_t *p = Font5x7[c - ' '];

    for (uint16_t row = 0; row < size; row++) {
        int16_t py = y + row;
        if (py < ov.y || py >= ov.y + ov.h) continue;

        for (uint16_t col = 0; col < char_w; col++) {
            int16_t px = x + col;
            if (px < ov.x || px >= ov.x + ov.w) continue;

            bool is_fg = false;
            if (row >= y_offset && row < y_offset + glyph_h) {
                uint16_t font_row = (row - y_offset) / scale;
                uint16_t font_col = col / scale;
                if (font_col < 5 && font_row < 8) {
                    if (p[font_col] & (1 << font_row)) {
                        is_fg = true;
                    }
                }
            }

            if (is_fg) {
                ui_draw_point(tile, px, py, color);
            } else if (bg != color) {
                ui_draw_point(tile, px, py, bg);
            }
        }
    }
}

void ui_draw_string(ui_tile_t *tile, int16_t x, int16_t y, const char *str, 
                    FontSize_t size, uint16_t color, uint16_t bg) {
    if (!tile || !str) return;

    int16_t cur_x = x;
    int16_t cur_y = y;

    // ⚡ 核心修复：根据 16/20/24 动态匹配步进宽度
    uint16_t asc_step = (size >= 24) ? 12 : ((size >= 20) ? 10 : 8);

    while (*str) {
        if ((uint8_t)*str < 0x80) {
            char ch = *str++;
            ui_rect_t char_box = ui_rect_make(cur_x, cur_y, asc_step, size);
            ui_rect_t ov;
            if (ui_rect_intersect(&tile->rect, &char_box, &ov)) {
                ui_draw_char(tile, cur_x, cur_y, ch, size, color, bg);
            }
            cur_x += asc_step;
        } else {
            if (*(str + 1) == '\0') break;

            uint16_t gb = ((uint8_t)str[0] << 8) | (uint8_t)str[1];
            str += 2;

            ui_rect_t hz_box = ui_rect_make(cur_x, cur_y, size, size);
            ui_rect_t ov;
            if (ui_rect_intersect(&tile->rect, &hz_box, &ov)) {
                uint16_t bytes_per = 0;
                const uint8_t *dot_buf = GetFontBitmap(gb, size, &bytes_per);
                if (dot_buf) {
                    ui_draw_bitmap(tile, cur_x, cur_y, size, size, dot_buf, color, bg);
                }
            }
            cur_x += size;
        }
    }
}