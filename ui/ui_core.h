/*
 * @Author: 轩
 * @Date: 2026-08-19 18:50:20
 * @LastEditTime: 2026-09-02 17:13:27
 * @FilePath: \pico\ui\ui_core.h
 */
#ifndef UI_CORE_H
#define UI_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "ui_draw.h"
#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

// 场景与控件切片光栅化回调函数类型
typedef void (*ui_scene_draw_cb_t)(ui_tile_t *tile);

/**
 * @brief 全屏切片流水线渲染 (遍历全部 10 个切片行)
 * @param draw_scene 场景绘制回调
 */
void ui_render_frame(ui_scene_draw_cb_t draw_scene);

/**
 * @brief 指定切片行范围渲染 (全屏行宽推流，例如顶部状态栏仅刷 row 0)
 * @param start_row 起始切片索引 (0 ~ UI_TILE_ROWS-1)
 * @param end_row   结束切片索引 (0 ~ UI_TILE_ROWS-1)
 * @param draw_scene 场景绘制回调
 */
void ui_render_tiles(uint8_t start_row, uint8_t end_row, ui_scene_draw_cb_t draw_scene);

/**
 * @brief ⚡ 任意局部脏矩形裁剪切片极速推流 (仅推流与脏矩形相交的微小区域，零总线冗余)
 * @param dirty      待更新的目标局部脏矩形
 * @param draw_scene 场景绘制回调
 */
void ui_render_dirty_clipped(const ui_rect_t *dirty, ui_scene_draw_cb_t draw_scene);

#ifdef __cplusplus
}
#endif

#endif // UI_CORE_H