#ifndef UI_TILE_H_INCLUDED
#define UI_TILE_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

// 横屏 480x320 分辨率切片规格 (10 个切片行，每行高 32px)[cite: 4]
#define UI_TILE_W       480
#define UI_TILE_H       32
#define UI_TILE_ROWS    10
#define UI_TILE_COUNT   UI_TILE_ROWS

typedef struct {
    ui_rect_t   rect;   // 当前切片在屏幕上的绝对物理区域[cite: 4]
    uint16_t   *buf;    // 指向当前切片的像素显存指针 (RGB565)[cite: 4]
} ui_tile_t;

// 初始化切片双缓冲显存池[cite: 4]
void ui_tile_init(void);

// 获取下一个待渲染的切片 (零等待，直接复用闲置缓冲区)[cite: 4]
ui_tile_t *ui_tile_get_next(uint8_t row_idx);

// 提交当前切片至 SPI DMA 发送，并自动切换乒乓缓冲区[cite: 4]
void ui_tile_flush(ui_tile_t *tile);

// 等待所有 DMA 传输完成[cite: 4]
void ui_tile_wait_finish(void);

#ifdef __cplusplus
}
#endif

#endif // UI_TILE_H_INCLUDED
