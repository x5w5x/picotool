#include "ui_tile.h"
#include "lcd.h"
#include <string.h>

static uint16_t  s_tile_bufs[2][UI_TILE_W * UI_TILE_H];
static uint8_t   s_buf_idx = 0;
static ui_tile_t s_current_tile;

void ui_tile_init(void)
{
    s_buf_idx = 0;
    memset(s_tile_bufs, 0, sizeof(s_tile_bufs));
}

ui_tile_t *ui_tile_get_next(uint8_t row_idx)
{
    s_current_tile.buf  = s_tile_bufs[s_buf_idx];
    s_current_tile.rect = ui_rect_make(0, (int16_t)(row_idx * UI_TILE_H), UI_TILE_W, UI_TILE_H);
    
    // 强制清零当前切片显存，消除上一帧残像
    //memset(s_current_tile.buf, 0, UI_TILE_W * UI_TILE_H * sizeof(uint16_t));
    
    return &s_current_tile;
}

void ui_tile_flush(ui_tile_t *tile)
{
    // 必须确保上一包 DMA 发送彻底完成[cite: 4]
    LCD_WaitDMA();

    // 启动当前切片异步推流[cite: 4]
    LCD_PushTileDMA_Async(tile->rect.x, tile->rect.y, tile->rect.w, tile->rect.h, tile->buf);

    // 切换乒乓缓冲区[cite: 4]
    s_buf_idx ^= 1;
}

void ui_tile_wait_finish(void)
{
    LCD_WaitDMA();
}