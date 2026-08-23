/*
 * @Author: 轩
 * @Date: 2026-08-20 16:24:08
 * @LastEditTime: 2026-08-21 12:38:18
 * @FilePath: \pico\ui\ui_toast.h
 */
#ifndef UI_TOAST_H
#define UI_TOAST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_TOAST_INFO = 0,    // 0: 蓝色普通信息[cite: 36]
    UI_TOAST_SUCCESS,     // 1: 绿色成功提示[cite: 36]
    UI_TOAST_WARN,        // 2: 黄色告警提示[cite: 36]
    UI_TOAST_ERROR        // 3: 红色错误提示[cite: 36]
} ui_toast_type_t;

typedef struct {
    bool            is_active;    // 激活状态[cite: 36]
    char            message[48];  // 提示文本[cite: 36]
    ui_toast_type_t type;         // 提示类型[cite: 36]

    uint32_t        start_tick;   // 弹出时刻时间戳[cite: 36]
    uint32_t        duration_ms;  // 驻留与动画总时长 (默认 2300ms)[cite: 36]

    int16_t         target_x;     // 静止停靠目标 X (默认 232)[cite: 36]
    int16_t         target_y;     // 静止停靠目标 Y (默认 276)[cite: 36]
    int16_t         w;            // 气泡宽度 (默认 240)[cite: 36]
    int16_t         h;            // 气泡高度 (默认 34)[cite: 36]

    ui_rect_t       rect;         // 当前帧计算出的实时物理包围盒[cite: 36]
    ui_rect_t       old_rect;     // 上一帧物理包围盒 (用于计算脏矩形并集)
} ui_toast_t;

// 初始化全局 Toast 管理器[cite: 36]
void ui_toast_init(ui_toast_t *toast);

// 触发右下角滑入气泡[cite: 36]
void ui_toast_show(ui_toast_t *toast, const char *msg, ui_toast_type_t type, uint32_t now_tick);

// 驱动动画与倒计时 (返回 true 表示正在平移或状态变更，需局部重绘切片)[cite: 36]
bool ui_toast_update(ui_toast_t *toast, uint32_t now_tick);

// 切片光栅化渲染 (带像素级视口裁剪保护)[cite: 36]
void ui_toast_draw(ui_tile_t *tile, const ui_toast_t *toast);

// 计算 Toast 动画帧涉及的完整脏矩形并集 (包含旧位置与新位置，彻底消除残影)
static inline ui_rect_t ui_toast_get_dirty_union(const ui_toast_t *toast) {
    if (!toast) return ui_rect_make(0, 0, 0, 0);
    ui_rect_t dirty;
    ui_rect_union(&toast->old_rect, &toast->rect, &dirty);
    return dirty;
}

#ifdef __cplusplus
}
#endif

#endif // UI_TOAST_H