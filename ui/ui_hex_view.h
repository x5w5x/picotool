/*
 * @Author: 轩
 * @Date: 2026-08-21 14:24:25
 * @LastEditTime: 2026-08-21 14:24:30
 * @FilePath: \pico\ui\ui_hex_view.h
 */
#ifndef UI_HEX_VIEW_H
#define UI_HEX_VIEW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_HEX_BYTES_PER_ROW 8 // 480px 屏宽配合 FONT_16 最佳每行字节数 (地址 + 8字节Hex + ASCII)

typedef struct ui_hex_view ui_hex_view_t;

// 字节单元点选交互回调
typedef void (*ui_hex_select_cb_t)(ui_hex_view_t *hv, uint32_t byte_offset, uint8_t val, void *user_data);

struct ui_hex_view {
    ui_rect_t           rect;                                       // 控件物理包围盒
    const uint8_t      *data;                                       // 数据源内存指针 (RAM / Flash / Buffer)
    uint32_t            data_len;                                   // 数据总字节数
    uint32_t            base_addr;                                  // 基准物理地址偏移 (如 0x08000000, 0x10000000)

    int16_t             row_h;                                      // 单行高度 (默认 20px)
    int16_t             scroll_y;                                   // 当前 Y 轴滚动偏移
    int32_t             selected_byte_idx;                          // 当前选中的字节索引 (-1 为未选中)

    // ⚡ 触控手势防抖与滑动追踪状态机
    int16_t             down_x;
    int16_t             down_y;
    int16_t             scroll_start_y;
    bool                is_touch_down;
    bool                is_dragged;                                 // 8px 防抖死区锁定
    bool                is_scrolling;                               // 是否处于拖拽滚动中

    // 配色方案 (RGB565)
    uint16_t            color_bg;                                   // 视口底色 (极深黑蓝)
    uint16_t            color_addr;                                 // 地址列文本色 (暗金黄/橙黄)
    uint16_t            color_hex;                                  // Hex 16进制数据色 (纯白)
    uint16_t            color_ascii;                                // ASCII 对照字符色 (青蓝)
    uint16_t            color_selected_bg;                          // 选中字节高亮底色 (亮绿)
    uint16_t            color_selected_text;                        // 选中字节文本色 (纯黑)
    uint16_t            color_border;                               // 分割线颜色
    uint16_t            color_scrollbar;                            // 滚动指示条颜色

    ui_hex_select_cb_t  on_select;
    void               *user_data;
};

// 初始化 16 进制内存检视器
void ui_hex_view_init(ui_hex_view_t *hv, int16_t x, int16_t y, int16_t w, int16_t h,
                      ui_hex_select_cb_t on_select);

// 绑定数据源 (内存指针、总字节数、物理基地址)
void ui_hex_view_bind(ui_hex_view_t *hv, const uint8_t *data, uint32_t data_len, uint32_t base_addr);

// 快速跳转至指定绝对地址
void ui_hex_view_jump_to_addr(ui_hex_view_t *hv, uint32_t target_addr);

// 触控交互驱动 (支持拖拽上下浏览 + 8px 死区保护 + Touch-Up 精准点选字节)
bool ui_hex_view_process_touch(ui_hex_view_t *hv, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (O(1) 行级算术剪枝 + 视口硬裁剪保护)
void ui_hex_view_draw(ui_tile_t *tile, const ui_hex_view_t *hv);

// 获取控件局部脏矩形包围盒
static inline ui_rect_t ui_hex_view_get_dirty_rect(const ui_hex_view_t *hv) {
    return hv ? hv->rect : ui_rect_make(0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif // UI_HEX_VIEW_H