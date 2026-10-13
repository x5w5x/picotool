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

/* ⚡ 渲染期字节序转换(RGB565 → 大端字节流,屏要的)
 *
 * 必须在 draw_scene() 画完、推送之前调用,且必须覆盖"即将发送的
 * 整块区域",不能只覆盖实际画过的像素 —— clip 内没被图元碰到的
 * 像素还是上一帧翻转后的残留,漏翻就会以错字节序发出,表现为花屏。
 *
 *   - 全刷路径     → ui_tile_swap_full(tile)
 *   - 脏矩形路径   → ui_tile_swap_rect(tile, &clip_rect)
 *
 * 为什么不能在 push 里做:那样 CPU 翻转像素的时间会串进 DMA 时间线,
 * 乒乓重叠带来的收益全被吃掉。放到渲染期,翻转和上一块的 DMA 并行。 */
void ui_tile_swap_region(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h);
void ui_tile_swap_rect(ui_tile_t *tile, const ui_rect_t *r);
void ui_tile_swap_full(ui_tile_t *tile);

/* 同步推送路径(不经 ui_tile_flush)用完一块缓冲后调用,
 * 通知乒乓层这块已不在途,避免下一次 ui_tile_get_next 白等。 */
void ui_tile_mark_idle(ui_tile_t *tile);

#ifdef __cplusplus
}
#endif

#endif // UI_TILE_H_INCLUDED
