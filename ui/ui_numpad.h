/*
 * @Author: 轩
 * @Date: 2026-08-20 13:49:12
 * @LastEditTime: 2026-08-21 12:13:27
 * @FilePath: \pico\ui\ui_numpad.h
 */
#ifndef UI_NUMPAD_H
#define UI_NUMPAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_NUMPAD_MAX_UNITS 4
#define UI_NUMPAD_BUF_LEN   32

typedef struct ui_numpad ui_numpad_t;

// 确认提交回调：回传数字浮点值、单位字符串、原始文本与用户私有指针[cite: 24]
typedef void (*ui_numpad_cb_t)(ui_numpad_t *np, double val, const char *unit, const char *raw_str, void *user_data);

struct ui_numpad {
    ui_rect_t           rect;           // 弹窗物理包围盒 (居中 380x260)[cite: 24]
    bool                is_open; /*[cite: 24] */
    char                title[32];      // 弹窗标题[cite: 24]
    char                buffer[UI_NUMPAD_BUF_LEN]; // 输入缓冲区[cite: 24]
    uint8_t             cursor_pos; /*[cite: 24] */

    const char         *units[UI_NUMPAD_MAX_UNITS]; // 可选单位列表[cite: 24]
    uint8_t             unit_count; /*[cite: 24] */
    int8_t              selected_unit;  // 当前选中的单位索引 (-1 为无单位)[cite: 24]

    // ⚡ 触控手势防抖与防误触状态机
    int16_t             down_x;
    int16_t             down_y;
    bool                is_touch_down;
    bool                is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)

    // 配色方案 (RGB565)[cite: 24]
    uint16_t            color_bg; /*[cite: 24] */
    uint16_t            color_preview_bg; /*[cite: 24] */
    uint16_t            color_key_num_bg; /*[cite: 24] */
    uint16_t            color_key_fn_bg; /*[cite: 24] */
    uint16_t            color_key_unit_bg; /*[cite: 24] */
    uint16_t            color_key_ok_bg; /*[cite: 24] */
    uint16_t            color_border; /*[cite: 24] */

    ui_numpad_cb_t      on_confirm; /*[cite: 24] */
    void               *user_data; /*[cite: 24] */
};

// 初始化数字小键盘[cite: 24]
void ui_numpad_init(ui_numpad_t *np, ui_numpad_cb_t on_confirm);

// 弹出数字小键盘 (传入标题、初始值、可选单位数组与默认单位索引)[cite: 24]
void ui_numpad_open(ui_numpad_t *np, const char *title, const char *init_val,
                    const char **units, uint8_t unit_count, int8_t default_unit_idx, void *user_data);

// 关闭键盘[cite: 24]
void ui_numpad_close(ui_numpad_t *np);

// 触控交互分发 (集成 8px 死区保护与 Touch-Up 松手触发)
bool ui_numpad_process_touch(ui_numpad_t *np, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (像素级视口裁剪与算术剪枝)
void ui_numpad_draw(ui_tile_t *tile, const ui_numpad_t *np);

#ifdef __cplusplus
}
#endif

#endif // UI_NUMPAD_H