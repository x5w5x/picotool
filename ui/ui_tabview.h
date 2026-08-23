/*
 * @Author: 轩
 * @Date: 2026-08-20 12:59:16
 * @LastEditTime: 2026-08-21 12:30:37
 * @FilePath: \pico\ui\ui_tabview.h
 */
#ifndef UI_TABVIEW_H
#define UI_TABVIEW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

// 支持最多 12 个标签页[cite: 30]
#define UI_TABVIEW_MAX_TABS 12

typedef struct ui_tabview ui_tabview_t;

// 标签切换回调函数类型[cite: 30]
typedef void (*ui_tabview_cb_t)(ui_tabview_t *tabview, uint8_t old_tab, uint8_t new_tab, void *user_data);

struct ui_tabview {
    ui_rect_t       rect;           // 整体包围盒[cite: 30]
    ui_rect_t       bar_rect;       // 左侧侧边栏物理视口[cite: 30]
    ui_rect_t       content_rect;   // 右侧工作区视口[cite: 30]

    const char     *tab_names[UI_TABVIEW_MAX_TABS]; // 各标签标题[cite: 30]
    uint8_t         tab_count;      // 标签总数[cite: 30]
    uint8_t         active_tab_idx; // 当前选中的标签索引[cite: 30]

    int16_t         item_h;         // 单个标签项高度 (推荐 42~46px)[cite: 30]
    int16_t         scroll_y;       // 侧边栏当前滚动偏移[cite: 30]
    int16_t         content_h;      // 侧边栏内容总高度 (tab_count * item_h)[cite: 30]

    // ⚡ 触控手势防抖与全局捕获状态机
    int16_t         down_x;
    int16_t         down_y;
    int16_t         scroll_start_y; // 按下时的初始滚动偏移[cite: 30]
    bool            is_touch_down;  // 是否在侧边栏内按下
    bool            is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)
    bool            is_scrolling;   // 是否处于滚动中[cite: 30]

    FontSize_t      font_size;      // 字体大小 (默认 FONT_16)[cite: 30]

    // 配色方案 (RGB565)[cite: 30]
    uint16_t        color_bar_bg;       // 侧边栏底色[cite: 30]
    uint16_t        color_tab_active_bg;// 选中项背景底色[cite: 30]
    uint16_t        color_text;         // 未选中文字颜色[cite: 30]
    uint16_t        color_active_text;  // 选中文字颜色[cite: 30]
    uint16_t        color_indicator;    // 选中项左侧高亮条颜色[cite: 30]
    uint16_t        color_border;       // 分割线颜色[cite: 30]
    uint16_t        color_scrollbar;    // 滚动条颜色[cite: 30]

    ui_tabview_cb_t on_change;          // 切换回调[cite: 30]
    void           *user_data;          // 用户私有数据[cite: 30]
};

// 初始化支持滚动的侧边栏 TabView[cite: 30]
void ui_tabview_init(ui_tabview_t *tv, int16_t x, int16_t y, int16_t w, int16_t h, 
                     int16_t bar_w, int16_t item_h,
                     const char **tab_names, uint8_t tab_count, uint8_t default_idx,
                     ui_tabview_cb_t on_change);

// 设置活动选项卡 (自动触发切换回调)[cite: 30]
void ui_tabview_set_active(ui_tabview_t *tv, uint8_t tab_idx);

// 触控输入调度 (集成 8px 死区保护与 Touch Capture 全局追踪)[cite: 30]
bool ui_tabview_process_touch(ui_tabview_t *tv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (O(1) 条目索引剪枝 + 视口硬裁剪保护)[cite: 30]
void ui_tabview_draw(ui_tile_t *tile, const ui_tabview_t *tv);

// 获取侧边栏脏矩形 (用于 ui_render_dirty_clipped 局部极速推流)
static inline ui_rect_t ui_tabview_get_bar_dirty_rect(const ui_tabview_t *tv) {
    return tv ? tv->bar_rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_TABVIEW_H