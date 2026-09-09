#ifndef UI_DROPDOWN_H
#define UI_DROPDOWN_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_dropdown ui_dropdown_t;

// 选中项变更回调函数类型[cite: 17]
typedef void (*ui_dropdown_cb_t)(ui_dropdown_t *dd, int8_t selected_idx, void *user_data);

struct ui_dropdown {
    ui_rect_t           rect;           // 收起时的表头包围盒[cite: 17]
    const char        **items;          // 选项字符串数组[cite: 17]
    uint8_t             item_count;     // 选项总数[cite: 17]
    int8_t              selected_idx;   // 当前选中的索引 (0 ~ item_count-1)[cite: 17]
    uint8_t             max_visible_items; // 展开时最多显示几项，0 表示全部
    uint8_t             first_visible_idx; // 当前可见列表的首项索引
    int16_t             list_scroll_y;     // 展开列表的像素滚动偏移
    int16_t             scroll_start_y;    // 本次拖动开始时的滚动偏移

    bool                is_open;        // 是否处于展开状态[cite: 17]
    FontSize_t          font_size;      // 字体大小[cite: 17]

    uint16_t            color_bg;       // 菜单背景底色 (RGB565)[cite: 17]
    uint16_t            color_border;   // 边框颜色 (RGB565)[cite: 17]
    uint16_t            color_text;     // 普通文字颜色 (RGB565)[cite: 17]
    uint16_t            color_active;   // 选中高亮/指示箭头颜色 (RGB565)[cite: 17]

    // ⚡ 触控手势防抖与防误触状态机
    int16_t             down_x;
    int16_t             down_y;
    bool                is_touch_down;
    bool                is_dragged;     // 是否已识别为滑动手势 (位移 > 8px)

    ui_dropdown_cb_t    on_change;      // 选项改变回调[cite: 17]
    void               *user_data;      // 用户私有指针[cite: 17]
};

// 初始化下拉菜单[cite: 17]
void ui_dropdown_init(ui_dropdown_t *dd, int16_t x, int16_t y, int16_t w, int16_t h,
                      const char **items, uint8_t item_count, int8_t default_idx,
                      ui_dropdown_cb_t on_change);

// 设置下拉菜单配色[cite: 17]
void ui_dropdown_set_color(ui_dropdown_t *dd, uint16_t bg, uint16_t border, uint16_t text, uint16_t active);

// 限制展开列表高度；超过可见数量的选项可在列表内上下滑动
void ui_dropdown_set_max_visible_items(ui_dropdown_t *dd, uint8_t max_visible_items);

// 触控驱动状态机 (集成 8px 死区保护与 Touch-Up 松手触发)
bool ui_dropdown_process_touch(ui_dropdown_t *dd, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (需在所在图层末尾绘制以保证展开浮层位于顶层)[cite: 17]
void ui_dropdown_draw(ui_tile_t *tile, const ui_dropdown_t *dd);

// 获取下拉菜单当前完整包围盒 (用于 ui_render_dirty_clipped 局部推流)
ui_rect_t ui_dropdown_get_total_rect(const ui_dropdown_t *dd);

#ifdef __cplusplus
}
#endif

#endif // UI_DROPDOWN_H
