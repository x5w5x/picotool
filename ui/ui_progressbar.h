/*
 * @Author: 轩
 * @Date: 2026-08-20 13:36:53
 * @LastEditTime: 2026-08-21 12:21:27
 * @FilePath: \pico\ui\ui_progressbar.h
 */
#ifndef UI_PROGRESSBAR_H
#define UI_PROGRESSBAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

// 烧录执行阶段枚举[cite: 28]
typedef enum {
    UI_PROG_STAGE_IDLE = 0,    // 0: 空闲就绪 (灰暗)[cite: 28]
    UI_PROG_STAGE_ERASE,       // 1: 扇区擦除中 (橙黄)[cite: 28]
    UI_PROG_STAGE_PROGRAM,     // 2: 固件写入中 (亮绿)[cite: 28]
    UI_PROG_STAGE_VERIFY,      // 3: 数据校验中 (青蓝)[cite: 28]
    UI_PROG_STAGE_DONE,        // 4: 烧录成功 (纯绿)[cite: 28]
    UI_PROG_STAGE_ERROR        // 5: 烧录失败/超时 (亮红)[cite: 28]
} ui_prog_stage_t;

typedef struct ui_progressbar ui_progressbar_t;

struct ui_progressbar {
    ui_rect_t           rect;           // 进度条控件外框物理包围盒[cite: 28]
    ui_prog_stage_t     stage;          // 当前执行阶段[cite: 28]
    uint8_t             progress;       // 0 ~ 100 百分比[cite: 28]

    // 统计数据[cite: 28]
    uint32_t            current_bytes;  // 已写入字节[cite: 28]
    uint32_t            total_bytes;    // 总固件字节[cite: 28]
    float               speed_kbps;     // 实时传输速率 (KB/s)[cite: 28]
    uint32_t            start_time_ms;  // 阶段起始时间戳[cite: 28]
    uint32_t            elapsed_ms;     // 阶段已耗时[cite: 28]
    bool                show_details;   // 是否渲染下方详细统计看板[cite: 28]

    // 配色方案 (RGB565)[cite: 28]
    uint16_t            color_track_bg; // 进度条底槽颜色[cite: 28]
    uint16_t            color_border;   // 边框颜色[cite: 28]
    uint16_t            color_text;     // 标签文字颜色[cite: 28]
    uint16_t            color_stage[6]; // 各阶段专属填充色[cite: 28]
};

// 初始化进度条 (show_details=true 时自动排版顶部阶段与底部速率/耗时看板)[cite: 28]
void ui_progressbar_init(ui_progressbar_t *pb, int16_t x, int16_t y, int16_t w, int16_t h, bool show_details);

// 设置当前执行阶段 (自动切换对应主题色)[cite: 28]
void ui_progressbar_set_stage(ui_progressbar_t *pb, ui_prog_stage_t stage);

// 设置进度百分比 (0 ~ 100)[cite: 28]
void ui_progressbar_set_progress(ui_progressbar_t *pb, uint8_t progress);

// 更新传输统计数据 (自动计算百分比与实时 KB/s 速率)[cite: 28]
void ui_progressbar_update_stats(ui_progressbar_t *pb, uint32_t current_bytes, uint32_t total_bytes, uint32_t now_ms);

// 重置进度条到待机状态[cite: 28]
void ui_progressbar_reset(ui_progressbar_t *pb);

// 切片光栅化渲染 (带垂直行级算术剪枝与视口保护)[cite: 28]
void ui_progressbar_draw(ui_tile_t *tile, const ui_progressbar_t *pb);

// 获取进度条自身脏矩形 (用于 ui_render_dirty_clipped 极速局部推流)
static inline ui_rect_t ui_progressbar_get_dirty_rect(const ui_progressbar_t *pb) {
    return pb ? pb->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_PROGRESSBAR_H