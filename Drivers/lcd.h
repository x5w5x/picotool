/*
 * @Author: 轩
 * @Date: 2026-08-17 21:04:03
 * @LastEditTime: 2026-08-21 09:55:18
 * @FilePath: \pico\Drivers\lcd.h
 */
#ifndef LCD_H
#define LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LCD_WIDTH       480
#define LCD_HEIGHT      320

/* ===================== RGB565 常用颜色 ===================== */
#define WHITE           0xFFFF
#define BLACK           0x0000
#define RED             0xF800
#define GREEN           0x07E0
#define BLUE            0x001F
#define CYAN            0x07FF
#define MAGENTA         0xF81F
#define YELLOW          0xFFE0
#define GRAY            0x8410
#define DARK_GRAY       0x31A6
#define LIGHT_GRAY      0xC618
#define NAVY            0x000F
#define ORANGE          0xFD20

/* ===================== 基础控制 API ===================== */
void LCD_Init(void);
void LCD_Clear(uint16_t color);
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* ===================== 异步 DMA 与切片流水线 API ===================== */
void LCD_WaitDMA(void);
bool LCD_IsDMABusy(void);
void LCD_BulkWrite(const uint8_t *buf, uint32_t len);
void LCD_BulkWriteDMA(const uint8_t *buf, uint32_t len);

/**
 * @brief 核心切片异步推流接口 (Zero-blocking)
 * @note 内部先同步上一批传输，设置新视窗后立刻触发 DMA 并返回，CPU 可立即去合成下一块 Tile
 */
void LCD_PushTileDMA_Async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *buf);

/* ===================== 图元绘制 API ===================== */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void LCD_DrawFastHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void LCD_DrawFastVLine(uint16_t x, uint16_t y, uint16_t h, uint16_t color);
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void LCD_DrawCircle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color);

/* ===================== 字符与字符串 API ===================== */
extern const uint8_t Font5x7[][5];
void LCD_DrawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t size);
void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size);

/* ===================== 硬件滚动 API ===================== */
void LCD_ScrollArea(uint16_t top, uint16_t scroll, uint16_t bottom);
void LCD_SetScrollOffset(uint16_t offset);
void LCD_PageFlip(void);
void LCD_PushSubTile(uint16_t screen_x, uint16_t screen_y, 
                     uint16_t sub_w, uint16_t sub_h, 
                     const uint16_t *tile_buf, uint16_t tile_stride,
                     uint16_t src_offset_x, uint16_t src_offset_y);

#ifdef __cplusplus
}
#endif

#endif /* LCD_H */