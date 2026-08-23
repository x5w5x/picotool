#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "ui_input.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_KB_MODE_LOWER = 0,   // 小写字母 (q w e r t y...)
    UI_KB_MODE_UPPER,       // 大写字母 (Q W E R T Y...)
    UI_KB_MODE_NUM_SYM      // 数字与常用符号 (1 2 3 + - * / @ =...)
} ui_kb_mode_t;

typedef struct ui_keyboard ui_keyboard_t;
typedef void (*ui_kb_submit_cb_t)(ui_keyboard_t *kb, const char *text, void *user_data);

struct ui_keyboard {
    ui_rect_t           rect;           // 键盘整体覆盖区域 (X:0, Y:120, W:480, H:200)
    bool                is_open;        // 开启状态
    ui_kb_mode_t        mode;           // 当前按键模式

    char                buffer[UI_INPUT_MAX_LEN]; // 当前编辑中的文本
    uint8_t             cursor_pos;

    ui_input_t         *target_input;   // 关联绑定的目标输入框
    ui_kb_submit_cb_t   on_submit;      // 提交发送回调
    void               *user_data;

    // ⚡ 触控手势防抖与防误触状态机
    int16_t             down_x;
    int16_t             down_y;
    bool                is_touch_down;
    bool                is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)
};

void ui_keyboard_init(ui_keyboard_t *kb, ui_kb_submit_cb_t on_submit);
void ui_keyboard_open(ui_keyboard_t *kb, ui_input_t *target_input);
void ui_keyboard_close(ui_keyboard_t *kb);

// 触控状态机 (集成 8px 死区保护与 Touch-Up 松手触发)
bool ui_keyboard_process_touch(ui_keyboard_t *kb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染
void ui_keyboard_draw(ui_tile_t *tile, const ui_keyboard_t *kb);

#ifdef __cplusplus
}
#endif

#endif // UI_KEYBOARD_H