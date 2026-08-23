/*
 * @Author: 轩
 * @Date: 2026-08-19 18:50:20
 * @LastEditTime: 2026-08-21 11:15:43
 * @FilePath: \pico\ui\ui_core.c
 */
#include "ui_core.h"

// void ui_render_frame(ui_scene_draw_cb_t draw_scene)
// {
//     // 1. 遍历整屏的所有切片行 (共 15 行) 🔄
//     for (uint8_t row = 0; row < UI_TILE_ROWS; row++) {
        
//         // 获取当前切片 (自动等待上一次 DMA 并绑定缓冲区) 📦
//         ui_tile_t *tile = ui_tile_get_next(row);

//         // 默认背景底色清屏 ⬛
//         ui_draw_fill_rect(tile, &tile->rect, 0x0000);

//         // 触发上层场景与控件绘制 🎨
//         if (draw_scene) {
//             draw_scene(tile);
//         }

//         // 启动 DMA 异步推送切片并切换乒乓缓冲区 🚀
//         ui_tile_flush(tile);
//     }

//     // 等待最后一帧切片推送完毕 ⏱️
//     ui_tile_wait_finish();
// }

// void ui_render_tiles(uint8_t start_row, uint8_t end_row, ui_scene_draw_cb_t draw_scene)
// {
//     // 限制在合法切片范围内 🛡️
//     if (start_row >= UI_TILE_ROWS) return;
//     if (end_row >= UI_TILE_ROWS) end_row = UI_TILE_ROWS - 1;

//     for (uint8_t row = start_row; row <= end_row; row++) {
//         ui_tile_t *tile = ui_tile_get_next(row);

//         // 默认背景底色清屏 ⬛
//         ui_draw_fill_rect(tile, &tile->rect, 0x0000);

//         if (draw_scene) {
//             draw_scene(tile);
//         }

//         ui_tile_flush(tile);
//     }

//     ui_tile_wait_finish();
// }

void ui_render_tiles(uint8_t start_row, uint8_t end_row, ui_scene_draw_cb_t draw_scene)
{
    if (!draw_scene || start_row >= UI_TILE_ROWS) return;
    if (end_row >= UI_TILE_ROWS) end_row = UI_TILE_ROWS - 1;
    if (start_row > end_row) return;

    for (uint8_t row = start_row; row <= end_row; row++) {
        // 1. 获取当前切片显存指针 (内部自动复用乒乓 Buffer)[cite: 8]
        ui_tile_t *tile = ui_tile_get_next(row);

        // 2. 触发场景光栅化合成[cite: 8]
        draw_scene(tile);

        // 3. 启动整行 DMA 异步推流并切换双缓冲[cite: 8]
        ui_tile_flush(tile);
    }

    // 4. 等待最后一包传输收尾[cite: 8]
    ui_tile_wait_finish();
}

void ui_render_frame(ui_scene_draw_cb_t draw_scene)
{
    ui_render_tiles(0, UI_TILE_ROWS - 1, draw_scene);
}

void ui_render_dirty_clipped(const ui_rect_t *dirty, ui_scene_draw_cb_t draw_scene)
{
    if (!dirty || !draw_scene || dirty->w <= 0 || dirty->h <= 0) return;

    // 1. 计算受脏矩形影响的切片行起止索引
    int16_t start_row = dirty->y / UI_TILE_H;
    int16_t end_row   = (dirty->y + dirty->h - 1) / UI_TILE_H;

    // 边界安全收敛
    if (start_row < 0) start_row = 0;
    if (end_row >= UI_TILE_ROWS) end_row = UI_TILE_ROWS - 1;
    if (start_row > end_row) return;

    // 2. 仅遍历相交的切片行
    for (uint8_t row = (uint8_t)start_row; row <= (uint8_t)end_row; row++) {
        ui_tile_t *tile = ui_tile_get_next(row);

        // 3. 计算脏矩形与当前切片的物理重叠矩形
        ui_rect_t clip_rect;
        if (!ui_rect_intersect(&tile->rect, dirty, &clip_rect)) {
            continue;
        }

        // ⚡ 核心补齐：强制约束软光栅视口！
        // 超出 clip_rect 的任何右侧组件，ui_draw 内部在打点前直接 0 开销跳过
        ui_draw_set_clip(&clip_rect);

        // 4. 执行场景合成 (受 clip 保护，右侧完全不参与计算)
        draw_scene(tile);

        // 恢复视口
        ui_draw_reset_clip();

        // 5. 计算子矩形在切片显存内部的相对偏移
        uint16_t src_off_x = (uint16_t)(clip_rect.x - tile->rect.x);
        uint16_t src_off_y = (uint16_t)(clip_rect.y - tile->rect.y);

        // 6. 调用 LCD 驱动进行单次 DMA 局部开窗极速推流
        LCD_PushSubTile((uint16_t)clip_rect.x, (uint16_t)clip_rect.y,
                        (uint16_t)clip_rect.w, (uint16_t)clip_rect.h,
                        tile->buf, UI_TILE_W,
                        src_off_x, src_off_y);
    }

    ui_tile_wait_finish();
}