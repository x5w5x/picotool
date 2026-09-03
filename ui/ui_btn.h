/*
 * @Author: 轩
 * @Date: 2026-08-19 19:58:59
 * @LastEditTime: 2026-09-02 19:50:02
 * @FilePath: \pico\ui\ui_btn.h
 */
#ifndef UI_BUTTON_H
#define UI_BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h" 

#ifdef __cplusplus
extern "C" {
#endif
/** 按钮交互模式 */
typedef enum {
    UI_BTN_MODE_PUSH = 0, // 普通点按：松手触发回调
    UI_BTN_MODE_TOGGLE    // 状态切换：松手翻转选中状态并触发回调
} ui_btn_mode_t;

/** 按钮视觉形态 */
typedef enum {
    UI_BTN_STYLE_RECT = 0, // 标准矩形
    UI_BTN_STYLE_ROUND,    // 胶囊形 / 圆角矩形
    UI_BTN_STYLE_TEXT      // 无边框纯文本
} ui_btn_style_t;

/* 前向声明与回调函数类型 */
struct ui_btn;
typedef void (*ui_btn_cb_t)(struct ui_btn *btn, void *user_data); //回调

/* ================= 2. 按钮控件结构体 ================= */

typedef struct ui_btn {
    ui_rect_t       rect;           // 坐标与尺寸 (x, y, w, h)
    const char     *text;           // 显示文本
    FontSize_t      font_size;      // 字号 (FONT_16 / FONT_20 / FONT_24)
    ui_btn_mode_t   mode;           // 交互模式 (Push / Toggle)
    ui_btn_style_t  style;          // 视觉形态 (Rect / Round / Text)

    uint16_t        color_bg;       // 默认背景色
    uint16_t        color_active;   // 激活/按下时的背景或高亮色
    uint16_t        color_text;     // 默认文本颜色

    bool            is_pressed;     // 当前是否正被有效按压
    bool            is_checked;     // Toggle 模式下的选中状态
    bool            show_underline; // 处于激活/选中时是否绘制下划线

    uint32_t        last_tick;      // 防抖时间戳
    uint32_t        debounce_ms;    // 防抖冷却周期 (默认 50ms)

    //手势保护与防抖状态机
    int16_t         down_x;
    int16_t         down_y;
    bool            is_touch_down;
    bool            is_dragged;     // 是否已识别为拖拽/滑动

    ui_btn_cb_t     on_click;       // 点击事件回调
    void           *user_data;      // 用户私有数据指针
} ui_btn_t;

/* ================= 3. 核心 API 接口 ================= */

/**
 * @brief 初始化按钮基础属性
 */
void ui_btn_init(ui_btn_t *btn, int16_t x, int16_t y, int16_t w, int16_t h,
                 const char *text, ui_btn_cb_t on_click);

/**
 * @brief 触控输入处理（集成 8px 死区过滤与 Touch-Up 抬手触发）
 * @return true 表示状态发生改变需要局部/切片重绘
 */
bool ui_btn_process_touch(ui_btn_t *btn, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

/**
 * @brief 在切片显存中执行光栅化渲染（严格边界裁剪）
 */
void ui_btn_draw(ui_tile_t *tile, const ui_btn_t *btn);

/**
 * @brief 动态配置三态主题颜色
 */
void ui_btn_set_color(ui_btn_t *btn, uint16_t bg, uint16_t active, uint16_t text);

/**
 * @brief 获取按钮自身的包围盒（用于局部推流 ui_render_dirty_clipped）
 */
static inline ui_rect_t ui_btn_get_dirty_rect(const ui_btn_t *btn) {
    return btn ? btn->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_BUTTON_H