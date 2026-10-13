#include "ui_tile.h"
#include "lcd.h"
#include <string.h>

/* ============================================================
 * 切片乒乓缓冲 —— 真正生效的版本
 * ------------------------------------------------------------
 * 旧版三个问题导致双缓冲形同虚设:
 *   1) ui_render_dirty_clipped 不调 ui_tile_flush → s_buf_idx 永不翻转
 *   2) LCD_PushTileDMA_Async 内部 LCD_WaitDMA() → "Async" 其实是同步
 *   3) bswap 在 push 里原地做,CPU 时间串进 DMA 时间线
 *
 * 现在:所有权在 ui_tile.c 归属,等的地方是"复用这块缓冲之前",
 * 而不是"启动下一次传输之前"。这样 CPU 渲染 buf1 时 buf0 正在 DMA。
 * ============================================================ */

static uint16_t  s_tile_bufs[2][UI_TILE_W * UI_TILE_H];
static uint8_t   s_buf_idx = 0;

/* 每块缓冲的 DMA 在途标记。跨块复用前必须等它自己的传输结束
 * (单个 DMA 通道只有一个读地址,所以"换缓冲"这个动作仍需等上一次) */
static volatile bool s_buf_inflight[2] = { false, false };

static ui_tile_t s_current_tile;

void ui_tile_init(void)
{
    s_buf_idx = 0;
    s_buf_inflight[0] = s_buf_inflight[1] = false;
    memset(s_tile_bufs, 0, sizeof(s_tile_bufs));
}

void ui_tile_wait_finish(void)
{
    LCD_WaitDMA();
    s_buf_inflight[0] = s_buf_inflight[1] = false;
}

ui_tile_t *ui_tile_get_next(uint8_t row_idx)
{
    /* ⚡ 关键:只等"即将复用的这一块"自己的传输。
     * 注意不能只调 LCD_WaitDMA(),因为它等的是共享通道上的
     * 任意传输 —— 那样又会把上一块也等掉,重叠就没了。 */
    if (s_buf_inflight[s_buf_idx]) {
        LCD_WaitDMA();
        s_buf_inflight[s_buf_idx] = false;
    }

    s_current_tile.buf  = s_tile_bufs[s_buf_idx];
    s_current_tile.rect = ui_rect_make(0, (int16_t)(row_idx * UI_TILE_H),
                                       UI_TILE_W, UI_TILE_H);
    return &s_current_tile;
}

void ui_tile_flush(ui_tile_t *tile)
{
    if (!tile) return;

    /* ⚡ 这里刻意不再等待!bswap 已经在渲染期完成
     * (见 ui_tile_swap_rect / ui_tile_swap_full)。
     * 直接启动 DMA 并立刻返回,CPU 好去渲染下一块。 */
    LCD_PushTileDMA_Async(tile->rect.x, tile->rect.y,
                          tile->rect.w, tile->rect.h, tile->buf);

    s_buf_inflight[s_buf_idx] = true;
    s_buf_idx ^= 1;          /* 切到另一块给下一轮渲染 */
}

/* 同步路径(如 LCD_PushSubTile)自己等完了 DMA,通知乒乓层
 * "这块不在途",免得 ui_tile_get_next 下一次为它白等。 */
void ui_tile_mark_idle(ui_tile_t *tile)
{
    if (!tile || !tile->buf) return;
    if (tile->buf == s_tile_bufs[0]) s_buf_inflight[0] = false;
    else if (tile->buf == s_tile_bufs[1]) s_buf_inflight[1] = false;
}

/* ============================================================
 * 渲染期字节序转换
 * ------------------------------------------------------------
 * ⚡ 必须翻转"将被发送的整块区域",而不是"实际画过的像素"。
 * 原因:draw 图元的裁剪是做在 g_clip_rect 上的,所以 clip 内
 * 没被任何图元覆盖的像素不会进内存里写新值 —— 它们仍是上一帧
 * 翻转后的残留。若只翻转"画过的像素",这些残留会以错误字节序
 * 发出去变成花屏。所以按区域翻转,才是确定性的。
 *
 * 参数用屏幕坐标,内部自己裁到切片范围。
 * ============================================================ */
void ui_tile_swap_region(ui_tile_t *tile, int16_t x, int16_t y,
                         int16_t w, int16_t h)
{
    if (!tile || !tile->buf || w <= 0 || h <= 0) return;

    int16_t x0 = x, y0 = y;
    int16_t x1 = (int16_t)(x + w), y1 = (int16_t)(y + h);

    /* 裁剪到切片自身范围 */
    if (x0 < tile->rect.x) x0 = tile->rect.x;
    if (y0 < tile->rect.y) y0 = tile->rect.y;
    if (x1 > tile->rect.x + tile->rect.w) x1 = (int16_t)(tile->rect.x + tile->rect.w);
    if (y1 > tile->rect.y + tile->rect.h) y1 = (int16_t)(tile->rect.y + tile->rect.h);
    if (x0 >= x1 || y0 >= y1) return;

    int16_t sx = (int16_t)(x0 - tile->rect.x);
    int16_t sy = (int16_t)(y0 - tile->rect.y);

    for (int16_t row = 0; row < (int16_t)(y1 - y0); row++) {
        uint16_t *p = tile->buf + (uint32_t)(sy + row) * UI_TILE_W + sx;
        for (int16_t col = 0; col < (int16_t)(x1 - x0); col++) {
            p[col] = __builtin_bswap16(p[col]);
        }
    }
}

void ui_tile_swap_rect(ui_tile_t *tile, const ui_rect_t *r)
{
    if (!r) return;
    ui_tile_swap_region(tile, r->x, r->y, r->w, r->h);
}

void ui_tile_swap_full(ui_tile_t *tile)
{
    if (!tile) return;
    ui_tile_swap_region(tile, tile->rect.x, tile->rect.y,
                        tile->rect.w, tile->rect.h);
}
