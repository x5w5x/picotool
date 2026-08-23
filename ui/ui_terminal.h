/*
 * @Author: 轩
 * @Date: 2026-08-20 13:25:48
 * @LastEditTime: 2026-08-21 12:32:25
 * @FilePath: \pico\ui\ui_terminal.h
 */
#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_TERM_MAX_LINES    64   // 环形缓冲最大行数
#define UI_TERM_LINE_LEN     64   // 单行最大字符数

typedef enum {
    UI_TERM_MODE_ASCII = 0,       // 标准 ASCII 文本模式
    UI_TERM_MODE_HEX              // 16 进制 HEX 分栏模式
} ui_terminal_mode_t;

typedef struct ui_terminal ui_terminal_t;

struct ui_terminal {
    ui_rect_t           rect;           // 控件视口物理包围盒
    ui_terminal_mode_t  mode;           // 运行模式

    // 环形行缓冲区 (静态 4KB 内存)
    char                lines[UI_TERM_MAX_LINES][UI_TERM_LINE_LEN];
    uint16_t            line_colors[UI_TERM_MAX_LINES];
    uint16_t            head;           // 写入头指针
    uint16_t            count;          // 当前有效总行数

    // 滚动与自动跟滚机制
    int16_t             line_height;    // 单行像素高度 (默认 18px)
    int16_t             scroll_y;       // 当前滚动偏移
    bool                auto_scroll;    // 新数据写入时是否自动吸附到底部

    // ⚡ 触控手势防抖与全局捕获状态机
    int16_t             down_x;
    int16_t             down_y;
    int16_t             scroll_start_y; // 按下时的初始滚动偏移
    bool                is_touch_down;  // 是否在终端范围内按下
    bool                is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)
    bool                is_scrolling;   // 是否处于拖拽滚动中

    // 配色方案 (RGB565)
    uint16_t            color_bg;       // 控制台底色 (极深黑蓝)
    uint16_t            color_border;   // 边框色
    uint16_t            color_text_def; // 默认文本色
    uint16_t            color_scrollbar;// 滚动条颜色
};

// 初始化终端控件 (默认每行高 18px，配合 FONT_16 点阵)
void ui_terminal_init(ui_terminal_t *term, int16_t x, int16_t y, int16_t w, int16_t h);

// 写入单行文本 (支持自动按 \r\n 拆行与颜色指定)
void ui_terminal_write_line(ui_terminal_t *term, const char *str, uint16_t color);

// 格式化输出 (类似 printf)
void ui_terminal_printf(ui_terminal_t *term, uint16_t color, const char *fmt, ...);

// 写入二进制 Hex Dump 数据流 (自动按每 8 字节分行排版)[cite: 31, 32]
void ui_terminal_write_hex(ui_terminal_t *term, const uint8_t *data, size_t len, uint16_t color);

// 清空控制台历史缓冲区
void ui_terminal_clear(ui_terminal_t *term);

// 切换自动跟滚与查看模式
void ui_terminal_set_autoscroll(ui_terminal_t *term, bool enable);
void ui_terminal_set_mode(ui_terminal_t *term, ui_terminal_mode_t mode);

// 触控交互调度 (集成 8px 死区保护与 Touch Capture 全局追踪)
bool ui_terminal_process_touch(ui_terminal_t *term, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (O(1) 算术行剪枝 + 视口硬裁剪保护)
void ui_terminal_draw(ui_tile_t *tile, const ui_terminal_t *term);

// 获取终端脏矩形包围盒 (用于 ui_render_dirty_clipped 局部极速推流)
static inline ui_rect_t ui_terminal_get_dirty_rect(const ui_terminal_t *term) {
    return term ? term->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_TERMINAL_H