/*
 * @Author: 轩
 * @Date: 2026-08-21 14:10:21
 * @LastEditTime: 2026-08-21 14:10:27
 * @FilePath: \pico\ui\ui_stepper.h
 */
#ifndef UI_STEPPER_H
#define UI_STEPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_stepper ui_stepper_t;

// 数值变更回调
typedef void (*ui_stepper_change_cb_t)(ui_stepper_t *sp, int32_t val, void *user_data);
// 点击中心文本框回调 (通常用于唤起全键盘精确输入)
typedef void (*ui_stepper_click_val_cb_t)(ui_stepper_t *sp, void *user_data);

typedef enum {
    UI_STEPPER_HIT_NONE = 0,
    UI_STEPPER_HIT_DEC,       // 命中左侧减号按钮 [-]
    UI_STEPPER_HIT_INC,       // 命中右侧加号按钮 [+]
    UI_STEPPER_HIT_VAL        // 命中中心数值显示区域
} ui_stepper_hit_t;

struct ui_stepper {
    ui_rect_t               rect;               // 控件物理包围盒 (如 180x36)
    int32_t                 min_val;            // 最小值
    int32_t                 max_val;            // 最大值
    int32_t                 cur_val;            // 当前值
    int32_t                 step;               // 单次步进增量

    const char             *unit;               // 单位文本 (如 "kHz", "mV", "%")
    const char             *prefix;             // 前缀文本 (如 "CH1:", "PWM:")

    // ⚡ 触控手势、防抖与长按连发状态机
    ui_stepper_hit_t        hit_target;         // 当前按压目标
    int16_t                 down_x;
    int16_t                 down_y;
    bool                    is_touch_down;
    bool                    is_dragged;         // 8px 防抖死区锁定
    uint32_t                press_start_tick;   // 初次按下的时间戳
    uint32_t                last_step_tick;     // 上次连发步进时间戳
    uint16_t                repeat_interval_ms; // 动态加速连发周期 (400ms -> 60ms)

    // 配色方案 (RGB565)
    uint16_t                color_bg;           // 中心数值框底色
    uint16_t                color_btn_bg;       // 减/加按钮底色
    uint16_t                color_btn_active;   // 按钮按下高亮色
    uint16_t                color_border;       // 边框分割线
    uint16_t                color_text;         // 数值与标签文本色
    uint16_t                color_unit;         // 单位文本色

    ui_stepper_change_cb_t  on_change;
    ui_stepper_click_val_cb_t on_click_val;
    void                   *user_data;
};

// 初始化步进微调器 (默认三段式 [ - ] [ Value + Unit ] [ + ])
void ui_stepper_init(ui_stepper_t *sp, int16_t x, int16_t y, int16_t w, int16_t h,
                     int32_t min_val, int32_t max_val, int32_t cur_val, int32_t step,
                     const char *unit, ui_stepper_change_cb_t on_change);

// 设置步进器当前值 (带边界钳位)
void ui_stepper_set_val(ui_stepper_t *sp, int32_t val);

// 设置前缀标签
void ui_stepper_set_prefix(ui_stepper_t *sp, const char *prefix);

// 触控交互驱动 (支持单击步进、长按动态加速连发、8px 防抖死区)
bool ui_stepper_process_touch(ui_stepper_t *sp, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (三段式独立切片相交裁剪)
void ui_stepper_draw(ui_tile_t *tile, const ui_stepper_t *sp);

// 获取局部脏矩形包围盒
static inline ui_rect_t ui_stepper_get_dirty_rect(const ui_stepper_t *sp) {
    return sp ? sp->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_STEPPER_H