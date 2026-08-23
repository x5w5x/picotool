#ifndef CHINESE_H
#define CHINESE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ===================== 字号枚举 ===================== */
typedef enum {
    FONT_16 = 16,   /* 16x16 汉字 (8x16 ASCII),  32 字节/字 */
    FONT_20 = 20,   /* 20x20 汉字 (10x20 ASCII), 60 字节/字 */
    FONT_24 = 24    /* 24x24 汉字 (12x24 ASCII), 72 字节/字 */
} FontSize_t;

/* ===================== W25Q64 字库基地址偏移 ===================== */
#define FONT16_BASE_ADDR     0x000000   /* 16x16 点阵区 (261,696 字节) */
#define FONT20_BASE_ADDR     0x040000   /* 20x20 点阵区 (490,680 字节) */
#define FONT24_BASE_ADDR     0x0C0000   /* 24x24 点阵区 (588,816 字节) */

/* ===================== 核心 API ===================== */
bool Chinese_Init(void);

/* 多字号字符串与字符显示 */
void Chinese_ShowStringSize(uint16_t x, uint16_t y, const char *str, FontSize_t size, uint16_t color, uint16_t bg);
void Chinese_ShowCharSize(uint16_t x, uint16_t y, const char *hz, FontSize_t size, uint16_t color, uint16_t bg);
void Chinese_ShowGB2312Size(uint16_t x, uint16_t y, uint16_t gb, FontSize_t size, uint16_t color, uint16_t bg);

/* 默认 16 点阵兼容 API */
void Chinese_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg);
void Chinese_ShowChar(uint16_t x, uint16_t y, const char *hz, uint16_t color, uint16_t bg);
void Chinese_ShowGB2312(uint16_t x, uint16_t y, uint16_t gb, uint16_t color, uint16_t bg);
uint8_t *GetFontBitmap(uint16_t gb, FontSize_t size, uint16_t *bytes_per);
#ifdef __cplusplus
}
#endif

#endif /* CHINESE_H */