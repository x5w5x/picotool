/*
 * @Author: 轩
 * @Date: 2026-08-20 13:19:04
 * @LastEditTime: 2026-08-21 12:36:23
 * @FilePath: \pico\ui\ui_waveform.h
 */
#ifndef UI_WAVEFORM_H
#define UI_WAVEFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_WAVE_MAX_CHANNELS    8

typedef enum {
    UI_WAVE_MODE_DIGITAL_8CH = 0, // 8 通道数字逻辑时序图 (每个字节 8 bit)
    UI_WAVE_MODE_ANALOG_1CH       // 单通道模拟示波器曲线 (int16_t/uint16_t ADC 值)
} ui_waveform_mode_t;

typedef enum {
    UI_WAVE_DRAG_NONE = 0,
    UI_WAVE_DRAG_PAN,             // 左右滑动平移时间轴[cite: 34]
    UI_WAVE_DRAG_CURSOR_A,        // 拖拽测量游标 A[cite: 34]
    UI_WAVE_DRAG_CURSOR_B         // 拖拽测量游标 B[cite: 34]
} ui_waveform_drag_t;

typedef struct ui_waveform ui_waveform_t;

// 波形光标/视野改变回调[cite: 34]
typedef void (*ui_waveform_cb_t)(ui_waveform_t *wf, void *user_data);

struct ui_waveform {
    ui_rect_t           rect;               // 波形视口物理包围盒[cite: 34]
    ui_waveform_mode_t  mode;               // 渲染模式[cite: 34]

    // 数据源绑定[cite: 34]
    const uint8_t      *digital_data;       // 8 通道数字数据流 (D0~D7 分布在 bit0~bit7)[cite: 34]
    const int16_t      *analog_data;        // 模拟数据流[cite: 34]
    uint32_t            total_samples;      // 采样总点数[cite: 34]

    // 视野与缩放 (时间轴)[cite: 34]
    uint32_t            view_offset;        // 左边缘对应的起始采样点序号[cite: 34]
    uint16_t            samples_per_pixel;  // 采样压缩比 (1: 1像素1点, 2: 1像素2点...)[cite: 34]
    uint8_t             active_channels;    // 逻辑通道启用数量 (1 ~ 8)[cite: 34]

    // 双游标测量系统 (Cursor A / B)[cite: 34]
    bool                show_cursors;       // 是否显示游标[cite: 34]
    int16_t             cursor_a_x;         // 游标 A 屏幕 X 像素坐标[cite: 34]
    int16_t             cursor_b_x;         // 游标 B 屏幕 X 像素坐标[cite: 34]
    uint32_t            sample_rate_hz;     // 当前采样率 (用于换算真实时间 ns/us/ms)[cite: 34]

    // ⚡ 触控手势防抖与全局捕获状态机
    ui_waveform_drag_t  drag_state;         // 当前拖拽目标 (游标 A / 游标 B / 时间轴)[cite: 34]
    int16_t             down_x;
    int16_t             down_y;
    int16_t             touch_start_x;      // 按下时的基准 X 坐标[cite: 34]
    uint32_t            view_start_offset;  // 按下时的起始视野偏移[cite: 34]
    bool                is_touch_down;      // 是否处于触控按压中
    bool                is_dragged;         // 是否已确认为拖拽手势 (位移 > 8px)

    // 配色方案 (RGB565)[cite: 34]
    uint16_t            color_bg;           // 背景底色 (通常为纯黑)[cite: 34]
    uint16_t            color_grid;         // 网格与通道基准线暗色[cite: 34]
    uint16_t            color_cursor_a;     // 游标 A 颜色 (金黄)[cite: 34]
    uint16_t            color_cursor_b;     // 游标 B 颜色 (紫红)[cite: 34]
    uint16_t            color_channels[UI_WAVE_MAX_CHANNELS]; // CH0 ~ CH7 各通道专属颜色[cite: 34]

    ui_waveform_cb_t    on_change;          // 视野/游标改变回调[cite: 34]
    void               *user_data;          // 用户私有指针[cite: 34]
};

// 初始化波形画布[cite: 34]
void ui_waveform_init(ui_waveform_t *wf, int16_t x, int16_t y, int16_t w, int16_t h,
                      ui_waveform_mode_t mode, uint32_t sample_rate_hz);

// 绑定采样数据流[cite: 34]
void ui_waveform_bind_digital(ui_waveform_t *wf, const uint8_t *data, uint32_t total_samples);
void ui_waveform_bind_analog(ui_waveform_t *wf, const int16_t *data, uint32_t total_samples);

// 设置缩放与通道数[cite: 34]
void ui_waveform_set_scale(ui_waveform_t *wf, uint16_t samples_per_pixel, uint8_t active_channels);

// 触控交互调度 (集成 8px 死区保护、游标抓取与时间轴 Touch Capture 追踪)[cite: 34]
bool ui_waveform_process_touch(ui_waveform_t *wf, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (通道级 O(1) 算术剪枝 + 视口硬裁剪保护)[cite: 34]
void ui_waveform_draw(ui_tile_t *tile, const ui_waveform_t *wf);

// 获取波形画布脏矩形包围盒 (用于 ui_render_dirty_clipped 局部极速推流)
static inline ui_rect_t ui_waveform_get_dirty_rect(const ui_waveform_t *wf) {
    return wf ? wf->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_WAVEFORM_H