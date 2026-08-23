/*
 * @Author: 轩
 * @Date: 2026-08-20 13:55:44
 * @LastEditTime: 2026-08-21 12:18:44
 * @FilePath: \pico\ui\ui_packet_table.h
 */
#ifndef UI_PACKET_TABLE_H
#define UI_PACKET_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PKT_MAX_ROWS 48
#define UI_PKT_DATA_MAX 6

typedef enum {
    UI_PKT_STAT_ACK = 0,    // 应答 ACK (亮绿)
    UI_PKT_STAT_NACK,       // 无应答 NACK (亮红)
    UI_PKT_STAT_ERR,        // 总线冲突/错误 (亮红)
    UI_PKT_STAT_START,      // 起始信号 START (黄色)
    UI_PKT_STAT_STOP        // 停止信号 STOP (紫红)
} ui_pkt_status_t;

typedef struct {
    uint32_t        time_us;                    // 时间戳 (微秒)
    uint16_t        addr;                       // 设备地址 (0x00~0xFF, 0xFFFF 为无)
    bool            is_read;                    // true: 读(R), false: 写(W)
    uint8_t         data[UI_PKT_DATA_MAX];      // 负载数据
    uint8_t         data_len;                   // 有效长度
    ui_pkt_status_t status;                     // 状态标识
} ui_pkt_row_t;

typedef struct ui_packet_table ui_packet_table_t;
typedef void (*ui_pkt_select_cb_t)(ui_packet_table_t *table, int16_t index, const ui_pkt_row_t *row, void *user_data);

struct ui_packet_table {
    ui_rect_t           rect;           // 表格整体物理包围盒
    ui_pkt_row_t        rows[UI_PKT_MAX_ROWS]; // 环形缓冲区
    uint16_t            head;           // 写入头
    uint16_t            count;          // 有效总行数
    int16_t             selected_idx;   // 选中的逻辑行索引 (-1 为未选中)

    int16_t             header_h;       // 表头高度 (默认 22px)
    int16_t             row_h;          // 行高度 (默认 24px)
    int16_t             scroll_y;       // 当前滚动偏移
    bool                auto_scroll;    // 自动跟随吸底

    // ⚡ 触控手势防抖与防误触状态机
    int16_t             down_x;
    int16_t             down_y;
    int16_t             scroll_start_y;
    bool                is_touch_down;
    bool                is_dragged;     // 是否已识别为滑动拖拽 (位移 > 8px)
    bool                is_scrolling;   // 是否正在滚动

    // 配色方案 (RGB565)
    uint16_t            color_bg;
    uint16_t            color_header_bg;
    uint16_t            color_row_even;
    uint16_t            color_row_odd;
    uint16_t            color_selected_bg;
    uint16_t            color_border;
    uint16_t            color_scrollbar;

    ui_pkt_select_cb_t  on_select;
    void               *user_data;
};

// 初始化报文解析表格
void ui_packet_table_init(ui_packet_table_t *tbl, int16_t x, int16_t y, int16_t w, int16_t h, ui_pkt_select_cb_t on_select);

// 清空全部报文记录
void ui_packet_table_clear(ui_packet_table_t *tbl);

// 推入一条新解析的报文
void ui_packet_table_push(ui_packet_table_t *tbl, uint32_t time_us, uint16_t addr, bool is_read, 
                          const uint8_t *data, uint8_t len, ui_pkt_status_t status);

// 获取当前选中的报文条目指针
const ui_pkt_row_t *ui_packet_table_get_selected(const ui_packet_table_t *tbl);

// 触控交互分发 (集成 8px 死区保护与 Touch-Up 松手触发)
bool ui_packet_table_process_touch(ui_packet_table_t *tbl, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染 (表头固顶 + O(1) 索引算术剪枝)
void ui_packet_table_draw(ui_tile_t *tile, const ui_packet_table_t *tbl);

#ifdef __cplusplus
}
#endif

#endif // UI_PACKET_TABLE_H