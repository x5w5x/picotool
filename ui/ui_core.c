/*
 * @Author: 轩
 * @Date: 2026-08-19 18:50:20
 * @LastEditTime: 2026-09-02 19:48:05
 * @FilePath: \pico\ui\ui_core.c
 */
#include "ui_core.h"
void ui_render_tiles(uint8_t start_row, uint8_t end_row, ui_scene_draw_cb_t draw_scene)
{
    if (!draw_scene || start_row >= UI_TILE_ROWS) return;
    if (end_row >= UI_TILE_ROWS) end_row = UI_TILE_ROWS - 1;
    if (start_row > end_row) return;

    for (uint8_t row = start_row; row <= end_row; row++) {
        //获取当前切片显存指针 (内部只在复用该块时才等它的 DMA)
        ui_tile_t *tile = ui_tile_get_next(row);
        draw_scene(tile);

        //渲染期翻转整块字节序,然后启动异步 DMA 并切乒乓
        ui_tile_swap_full(tile);
        ui_tile_flush(tile);
    }

    // 等待最后一包传输收尾
    ui_tile_wait_finish();
}
//全刷
void ui_render_frame(ui_scene_draw_cb_t draw_scene)
{
    ui_render_tiles(0, UI_TILE_ROWS - 1, draw_scene);
}

void ui_render_dirty_clipped(const ui_rect_t *dirty, ui_scene_draw_cb_t draw_scene)
{
    if (!dirty || !draw_scene || dirty->w <= 0 || dirty->h <= 0) return;

    //计算受脏矩形影响的切片行起止索引
    int16_t start_row = dirty->y / UI_TILE_H;
    int16_t end_row   = (dirty->y + dirty->h - 1) / UI_TILE_H;

    // 边界安全收敛
    if (start_row < 0) start_row = 0;
    if (end_row >= UI_TILE_ROWS) end_row = UI_TILE_ROWS - 1;
    if (start_row > end_row) return;

    //仅遍历相交的切片行
    for (uint8_t row = (uint8_t)start_row; row <= (uint8_t)end_row; row++) {
        ui_tile_t *tile = ui_tile_get_next(row);

        //计算脏矩形与当前切片的物理重叠矩形
        ui_rect_t clip_rect;
        if (!ui_rect_intersect(&tile->rect, dirty, &clip_rect)) {
            continue;
        }
        ui_draw_set_clip(&clip_rect);

        //场景合成 (受 clip 保护，右侧完全不参与计算)
        draw_scene(tile);

        // 恢复视口
        ui_draw_reset_clip();

        /* 5. 渲染期翻转整个 clip 区域(不是"画过的像素" —— 稀疏区域
         *    仍是上一帧翻转后的残留,不翻就会以错字节序发出去花屏) */
        ui_tile_swap_rect(tile, &clip_rect);

        /* 6. 推送"脏矩形 ∩ 本切片"这块子区域。
         *
         * 为什么这里不能像全刷那样调 ui_tile_flush(整块 480x32):
         * 脏矩形路径下 draw_scene 只重画了 clip 内的像素,切片里
         * clip 之外的像素是上一帧(甚至上一个 row 复用同一缓冲时)
         * 的残留 —— 整块发出去会把残留画到屏幕上。
         *
         * 所以这条路径发子矩形。它拿不到乒乓重叠(逐行 DMA + 行间
         * 等待),但已经比旧版快:bswap 现在只翻 clip 交集,而不是
         * 整个子矩形在推送里翻。
         *
         * ⚡ 真正需要"全刷级"吞吐的场景走 ui_render_frame,那条路
         * 已经完全乒乓化。 */
        uint16_t src_off_x = (uint16_t)(clip_rect.x - tile->rect.x);
        uint16_t src_off_y = (uint16_t)(clip_rect.y - tile->rect.y);
        LCD_PushSubTile((uint16_t)clip_rect.x, (uint16_t)clip_rect.y,
                        (uint16_t)clip_rect.w, (uint16_t)clip_rect.h,
                        tile->buf, UI_TILE_W,
                        src_off_x, src_off_y);

        /* 这块缓冲刚被同步发完,标记为不在途,避免 get_next 空等 */
        ui_tile_mark_idle(tile);
    }

    ui_tile_wait_finish();
}