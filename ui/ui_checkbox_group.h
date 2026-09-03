#ifndef UI_CHECKBOX_GROUP_H
#define UI_CHECKBOX_GROUP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CHECK_MAX_ITEMS 32

typedef enum {
    UI_CHECK_MODE_MULTI = 0,    // 多选复选框模式 
    UI_CHECK_MODE_SINGLE        // 互斥单选钮模式
} ui_check_mode_t;

typedef struct {
    char        label[32];      // 选项文本
    bool        is_checked;     // 勾选状态
    bool        is_disabled;    // 禁用置灰状态
} ui_check_item_t;

typedef struct ui_checkbox_group ui_checkbox_group_t;

typedef void (*ui_checkbox_cb_t)(ui_checkbox_group_t *cg, uint8_t index, bool is_checked, void *user_data);

struct ui_checkbox_group {
    ui_rect_t           rect;                           // 控件物理包围盒
    ui_check_mode_t     mode;                           // 运行模式 (多选 / 单选互斥)
    ui_check_item_t     items[UI_CHECK_MAX_ITEMS];
    uint8_t             item_count;                     // 选项总数
    int16_t             item_h;                         // 单行高度 (默认 36px)

    // ⚡ 滚动与视口机制
    int16_t             scroll_y;                       // 当前 Y 轴滚动偏移
    int16_t             content_h;                      // 虚拟内容总高度 (item_count * item_h)

    // ⚡ 触控手势防抖与滑动追踪状态机
    int16_t             down_x;
    int16_t             down_y;
    int16_t             scroll_start_y;
    bool                is_touch_down;
    bool                is_dragged;                     // 8px 防抖死区锁定
    bool                is_scrolling;                   // 是否处于拖拽滚动中

    // 配色方案 (RGB565)
    uint16_t            color_bg;                       // 背景底色 (极深黑蓝)
    uint16_t            color_box_bg;                   // 勾选框未选中底色
    uint16_t            color_box_border;               // 勾选框边框色
    uint16_t            color_checked;                  // 勾选激活高亮色 (亮绿/青蓝)
    uint16_t            color_text;                     // 选项文字颜色
    uint16_t            color_text_disabled;            // 禁用置灰文字色
    uint16_t            color_border;                   // 控件外框分割线
    uint16_t            color_scrollbar;                // 滚动条滑块颜色

    ui_checkbox_cb_t    on_change;
    void               *user_data;
};

// 初始化支持滑动的复选框/单选组
void ui_checkbox_group_init(ui_checkbox_group_t *cg, int16_t x, int16_t y, int16_t w, int16_t h,
                            ui_check_mode_t mode, int16_t item_h, ui_checkbox_cb_t on_change);

// 添加选项条目 (自动重算总虚拟高度)
bool ui_checkbox_group_add_item(ui_checkbox_group_t *cg, const char *label, bool init_checked, bool is_disabled);

// 设置指定选项的勾选状态
void ui_checkbox_group_set_checked(ui_checkbox_group_t *cg, uint8_t index, bool checked);

// 查询指定选项是否被勾选
bool ui_checkbox_group_is_checked(const ui_checkbox_group_t *cg, uint8_t index);

// 清空全部选项
void ui_checkbox_group_clear(ui_checkbox_group_t *cg);

// 触控交互驱动 (支持拖拽滚动 + 8px 死区保护 + Touch-Up 抬手勾选)
bool ui_checkbox_group_process_touch(ui_checkbox_group_t *cg, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (带 O(1) 行剪枝与视口硬裁剪)
void ui_checkbox_group_draw(ui_tile_t *tile, const ui_checkbox_group_t *cg);

// 获取控件局部脏矩形包围盒
static inline ui_rect_t ui_checkbox_group_get_dirty_rect(const ui_checkbox_group_t *cg) {
    return cg ? cg->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_CHECKBOX_GROUP_H