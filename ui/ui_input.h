#ifndef UI_INPUT_H
#define UI_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_INPUT_MAX_LEN 64

typedef struct ui_input ui_input_t;
typedef void (*ui_input_cb_t)(ui_input_t *input, void *user_data);

struct ui_input {
    ui_rect_t       rect;                       // 控件物理包围盒
    char            text[UI_INPUT_MAX_LEN];     // 文本内容缓冲区
    const char     *placeholder;                // 占位提示符 (如 "点击输入指令...")
    uint8_t         cursor_pos;                 // 光标位置
    bool            is_focused;                 // 焦点/选中状态

    // 配色方案 (RGB565)
    uint16_t        color_bg;
    uint16_t        color_border;
    uint16_t        color_focus_border;
    uint16_t        color_text;
    uint16_t        color_placeholder;

    // ⚡ 触控手势防抖与防误触状态机
    int16_t         down_x;
    int16_t         down_y;
    bool            is_touch_down;
    bool            is_dragged;                 // 是否已识别为滑动手势 (位移 > 8px)

    ui_input_cb_t   on_focus;                   // 点击聚焦回调 (用于呼出键盘)
    void           *user_data;
};

void ui_input_init(ui_input_t *input, int16_t x, int16_t y, int16_t w, int16_t h, 
                   const char *placeholder, ui_input_cb_t on_focus);
void ui_input_set_text(ui_input_t *input, const char *str);
void ui_input_clear(ui_input_t *input);

// 触控驱动状态机 (集成 8px 死区保护与 Touch-Up 松手触发)
bool ui_input_process_touch(ui_input_t *input, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (受全局视口裁剪保护)
void ui_input_draw(ui_tile_t *tile, const ui_input_t *input);

#ifdef __cplusplus
}
#endif

#endif // UI_INPUT_H