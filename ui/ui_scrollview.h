#ifndef UI_SCROLLVIEW_H
#define UI_SCROLLVIEW_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_scrollview ui_scrollview_t;

// 内容切片绘制回调函数原型
typedef void (*ui_scrollview_draw_cb_t)(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data);

struct ui_scrollview {
    ui_rect_t               view_rect;          // 视口在屏幕上的绝对物理矩形
    int16_t                 content_h;          // 虚拟内容的总像素高度
    int16_t                 scroll_y;           // 当前 Y 轴滚动偏移量 (0 ~ content_h - view_rect.h)

    // ⚡ 触控手势防抖与全局捕获状态机
    int16_t                 down_x;
    int16_t                 down_y;
    int16_t                 scroll_start_y;     // 按下时的初始滚动偏移
    bool                    is_touch_down;      // 触控按压中
    bool                    is_dragged;         // 是否已识别为滑动手势 (位移 > 8px)
    bool                    is_scrolling;       // 是否处于滚动状态

    // 绘制回调与上下文
    ui_scrollview_draw_cb_t draw_content;
    void                   *user_data;

    // 配色与指示条
    uint16_t                color_bg;           // 视口背景色
    uint16_t                color_scrollbar;    // 滚动条滑块颜色
    bool                    show_scrollbar;     // 是否显示滚动条
};

// 初始化滚动视口
void ui_scrollview_init(ui_scrollview_t *sv, int16_t x, int16_t y, int16_t w, int16_t h,
                        int16_t content_h, ui_scrollview_draw_cb_t draw_content);

// 视口触控调度 (集成 8px 死区保护与 Touch Capture 全局捕获)
bool ui_scrollview_process_touch(ui_scrollview_t *sv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (带相交快速剔除与像素级视口裁剪)
void ui_scrollview_draw(ui_tile_t *tile, const ui_scrollview_t *sv);

#ifdef __cplusplus
}
#endif

#endif // UI_SCROLLVIEW_H