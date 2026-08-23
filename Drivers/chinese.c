#include "chinese.h"
#include "w25q64.h"
#include "lcd.h"
#include <string.h>

/* 最大支持 24x24 汉字像素字节流缓冲 (24 * 24 * 2 = 1152 字节) */
static uint8_t hz_pixel_buf[24 * 24 * 2];

/* ASCII 字符全高整块渲染缓冲区 (最大 12 宽 * 24 高 * 2 字节 = 576 字节) */
static uint8_t ascii_pixel_buf[12 * 24 * 2];

/* ===================== 64 槽位 RAM LRU 字形缓存池 ===================== */
#define FONT_CACHE_SIZE   64

typedef struct {
    uint16_t gb_code;
    uint8_t  size;
    uint8_t  data[72];
    uint32_t last_used;
} font_cache_t;

static font_cache_t g_font_cache[FONT_CACHE_SIZE];
static uint32_t     g_cache_timer = 0;

/* ===================== GB2312 区位码偏移换算 ===================== */
static uint32_t GB2312_GetOffset(uint8_t high, uint8_t low)
{
    if (high < 0xA1 || high > 0xF7 || low < 0xA1 || low > 0xFE) return 0xFFFFFFFF;
    return ((uint32_t)(high - 0xA1) * 94 + (low - 0xA1));
}

static uint32_t GetFontInfo(FontSize_t size, uint16_t *bytes_per)
{
    switch (size) {
        case FONT_16: *bytes_per = 32; return FONT16_BASE_ADDR;
        case FONT_20: *bytes_per = 60; return FONT20_BASE_ADDR;
        case FONT_24: *bytes_per = 72; return FONT24_BASE_ADDR;
        default:      *bytes_per = 32; return FONT16_BASE_ADDR;
    }
}

bool Chinese_Init(void)
{
    W25_Init();
    memset(g_font_cache, 0, sizeof(g_font_cache));
    uint32_t id = W25_ReadJEDECID();
    return (id != 0x00000000 && id != 0x00FFFFFF);
}

/* ===================== 极速字模获取 (带 RAM 缓存) ===================== */
uint8_t *GetFontBitmap(uint16_t gb, FontSize_t size, uint16_t *bytes_per)
{
    uint16_t bp = 0;
    uint32_t base = GetFontInfo(size, &bp);
    if (bytes_per) *bytes_per = bp;

    g_cache_timer++;

    // 1. 查找 Cache 命中
    for (int i = 0; i < FONT_CACHE_SIZE; i++) {
        if (g_font_cache[i].gb_code == gb && g_font_cache[i].size == (uint8_t)size) {
            g_font_cache[i].last_used = g_cache_timer;
            return g_font_cache[i].data;
        }
    }

    // 2. LRU 淘汰最久未使用的字模
    int victim = 0;
    uint32_t oldest = g_font_cache[0].last_used;
    for (int i = 1; i < FONT_CACHE_SIZE; i++) {
        if (g_font_cache[i].last_used < oldest) {
            oldest = g_font_cache[i].last_used;
            victim = i;
        }
    }

    // 3. 从 Flash 读取
    uint8_t high = (uint8_t)(gb >> 8);
    uint8_t low  = (uint8_t)(gb & 0xFF);
    uint32_t offset = GB2312_GetOffset(high, low);
    if (offset == 0xFFFFFFFF) return NULL;

    uint32_t addr = base + offset * bp;
    W25_FastReadData(addr, g_font_cache[victim].data, bp);

    g_font_cache[victim].gb_code   = gb;
    g_font_cache[victim].size      = (uint8_t)size;
    g_font_cache[victim].last_used = g_cache_timer;

    return g_font_cache[victim].data;
}

/* ===================== 高性能单汉字 RAM 合成推流 ===================== */
static void DrawHZSize(uint16_t x, uint16_t y, uint16_t gb, FontSize_t size, uint16_t color, uint16_t bg)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;

    uint16_t bytes_per = 0;
    const uint8_t *dot_buf = GetFontBitmap(gb, size, &bytes_per);
    if (!dot_buf) return;

    uint8_t col_hi = (uint8_t)(color >> 8);
    uint8_t col_lo = (uint8_t)(color & 0xFF);
    uint8_t bg_hi  = (uint8_t)(bg >> 8);
    uint8_t bg_lo  = (uint8_t)(bg & 0xFF);

    uint16_t bytes_per_row = (size + 7) / 8;
    uint32_t idx = 0;

    // 100% 还原你最初完全正确的取模解算公式
    for (uint16_t row = 0; row < size; row++) {
        for (uint16_t col = 0; col < size; col++) {
            uint8_t byte_val = dot_buf[row * bytes_per_row + (col / 8)];
            if (byte_val & (0x80 >> (col % 8))) {
                hz_pixel_buf[idx++] = col_hi;
                hz_pixel_buf[idx++] = col_lo;
            } else {
                hz_pixel_buf[idx++] = bg_hi;
                hz_pixel_buf[idx++] = bg_lo;
            }
        }
    }

    uint16_t render_w = (x + size > LCD_WIDTH)  ? (LCD_WIDTH - x)  : size;
    uint16_t render_h = (y + size > LCD_HEIGHT) ? (LCD_HEIGHT - y) : size;

    if (render_w == size && render_h == size) {
        LCD_SetWindow(x, y, x + size - 1, y + size - 1);
        LCD_BulkWrite(hz_pixel_buf, size * size * 2);
    } else {
        LCD_SetWindow(x, y, x + render_w - 1, y + render_h - 1);
        for (uint16_t r = 0; r < render_h; r++) {
            LCD_BulkWrite(&hz_pixel_buf[r * size * 2], render_w * 2);
        }
    }
}

/* ===================== ASCII 字符全高覆盖渲染 (消灭残影) ===================== */
extern const uint8_t Font5x7[][5];

static void DrawAsciiChar(uint16_t x, uint16_t y, char c, FontSize_t size, uint16_t color, uint16_t bg)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (c < ' ' || c > '~') c = ' ';

    uint8_t scale = (size >= 24) ? 2 : 1;
    uint16_t char_w = (scale == 2) ? 12 : 6;
    uint16_t glyph_h = 8 * scale;
    uint16_t y_offset = (size > glyph_h) ? (size - glyph_h) / 2 : 0;

    const uint8_t *p = Font5x7[c - ' '];
    uint8_t col_hi = (uint8_t)(color >> 8);
    uint8_t col_lo = (uint8_t)(color & 0xFF);
    uint8_t bg_hi  = (uint8_t)(bg >> 8);
    uint8_t bg_lo  = (uint8_t)(bg & 0xFF);

    uint32_t idx = 0;

    for (uint16_t row = 0; row < size; row++) {
        for (uint16_t col = 0; col < char_w; col++) {
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
                ascii_pixel_buf[idx++] = col_hi;
                ascii_pixel_buf[idx++] = col_lo;
            } else {
                ascii_pixel_buf[idx++] = bg_hi;
                ascii_pixel_buf[idx++] = bg_lo;
            }
        }
    }

    uint16_t render_w = (x + char_w > LCD_WIDTH)  ? (LCD_WIDTH - x)  : char_w;
    uint16_t render_h = (y + size > LCD_HEIGHT) ? (LCD_HEIGHT - y) : size;

    if (render_w == char_w && render_h == size) {
        LCD_SetWindow(x, y, x + char_w - 1, y + size - 1);
        LCD_BulkWrite(ascii_pixel_buf, char_w * size * 2);
    } else {
        LCD_SetWindow(x, y, x + render_w - 1, y + render_h - 1);
        for (uint16_t r = 0; r < render_h; r++) {
            LCD_BulkWrite(&ascii_pixel_buf[r * char_w * 2], render_w * 2);
        }
    }
}

/* ===================== 多字号字符串混排引擎 ===================== */
void Chinese_ShowStringSize(uint16_t x, uint16_t y, const char *str, FontSize_t size, uint16_t color, uint16_t bg)
{
    if (!str) return;

    uint16_t cx = x, cy = y;
    uint8_t asc_scale = (size >= 24) ? 2 : 1;
    uint16_t asc_step  = (asc_scale == 2) ? 12 : 6;

    while (*str) {
        if (*str == '\r') {
            cx = x;
            str++;
            continue;
        }
        if (*str == '\n') {
            cx = x;
            cy += size + 4;
            str++;
            continue;
        }

        if ((uint8_t)*str < 0x80) {
            char c = *str++;
            if (cx + asc_step > LCD_WIDTH) {
                cx = x;
                cy += size + 4;
            }
            if (cy + size > LCD_HEIGHT) break;

            if (c >= 0x20) {
                DrawAsciiChar(cx, cy, c, size, color, bg);
                cx += asc_step;
            }
        } else {
            if (*(str + 1) == '\0') break;

            if (cx + size > LCD_WIDTH) {
                cx = x;
                cy += size + 4;
            }
            if (cy + size > LCD_HEIGHT) break;

            uint16_t gb = ((uint8_t)str[0] << 8) | (uint8_t)str[1];
            str += 2;
            DrawHZSize(cx, cy, gb, size, color, bg);
            cx += size;
        }
    }
}

void Chinese_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg)
{
    Chinese_ShowStringSize(x, y, str, FONT_16, color, bg);
}

void Chinese_ShowCharSize(uint16_t x, uint16_t y, const char *hz, FontSize_t size, uint16_t color, uint16_t bg)
{
    Chinese_ShowStringSize(x, y, hz, size, color, bg);
}

void Chinese_ShowChar(uint16_t x, uint16_t y, const char *hz, uint16_t color, uint16_t bg)
{
    Chinese_ShowString(x, y, hz, color, bg);
}

void Chinese_ShowGB2312(uint16_t x, uint16_t y, uint16_t gb, uint16_t color, uint16_t bg)
{
    DrawHZSize(x, y, gb, FONT_16, color, bg);
}

void Chinese_ShowGB2312Size(uint16_t x, uint16_t y, uint16_t gb, FontSize_t size, uint16_t color, uint16_t bg)
{
    DrawHZSize(x, y, gb, size, color, bg);
}