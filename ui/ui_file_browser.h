#ifndef UI_FILE_BROWSER_H
#define UI_FILE_BROWSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui_geometry.h"
#include "ui_tile.h"
#include "chinese.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FB_MAX_ITEMS     32
#define UI_FB_NAME_LEN      32

typedef enum {
    UI_FILE_TYPE_UNKNOWN = 0,
    UI_FILE_TYPE_DIR,       /* 目录文件夹 [DIR] */
    UI_FILE_TYPE_BIN,       /* .bin 固件 [BIN] */
    UI_FILE_TYPE_HEX,       /* .hex 固件 [HEX] */
    UI_FILE_TYPE_UF2,       /* .uf2 固件 [UF2] */
    UI_FILE_TYPE_TXT        /* 文本/日志 [TXT] */
} ui_file_type_t;

typedef struct {
    char            name[UI_FB_NAME_LEN];
    uint32_t        size_bytes;
    ui_file_type_t  type;
} ui_file_item_t;

typedef struct ui_file_browser ui_file_browser_t;
typedef void (*ui_file_select_cb_t)(ui_file_browser_t *fb, int8_t index, const ui_file_item_t *item, void *user_data);

struct ui_file_browser {
    ui_rect_t           rect;           /* 控件物理包围盒 (支持任意半宽/全宽)[cite: 19] */
    ui_file_item_t      items[UI_FB_MAX_ITEMS]; /*[cite: 19] */
    uint8_t             item_count; /*[cite: 19] */
    int8_t              selected_idx;   /* 选中条目索引 (-1 为未选中)[cite: 19] */

    int16_t             item_h;         /* 单行高度 (默认 30px) */
    int16_t             scroll_y;       /* 当前滚动偏移[cite: 19] */
    bool                is_scrolling;   /* 是否处于拖拽滑动状态[cite: 19] */

    /* ⚡ 触控手势防抖与防误触状态机 */
    int16_t             down_x;
    int16_t             down_y;
    int16_t             scroll_start_y;
    bool                is_touch_down;
    bool                is_dragged;     /* 是否已识别为滑动手势 (位移 > 8px) */

    uint16_t            color_bg; /*[cite: 19] */
    uint16_t            color_item_bg; /*[cite: 19] */
    uint16_t            color_active_bg; /*[cite: 19] */
    uint16_t            color_border; /*[cite: 19] */
    uint16_t            color_scrollbar; /*[cite: 19] */

    ui_file_select_cb_t on_select; /*[cite: 19] */
    void               *user_data; /*[cite: 19] */
};

void ui_file_browser_init(ui_file_browser_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, ui_file_select_cb_t on_select); /*[cite: 19] */
void ui_file_browser_clear(ui_file_browser_t *fb); /*[cite: 19] */
bool ui_file_browser_add_item(ui_file_browser_t *fb, const char *name, uint32_t size_bytes, bool is_dir); /*[cite: 19] */
const ui_file_item_t *ui_file_browser_get_selected(const ui_file_browser_t *fb); /*[cite: 19] */

/* 触控状态机 (集成 8px 死区保护与 Touch-Up 松手触发) */
bool ui_file_browser_process_touch(ui_file_browser_t *fb, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick); /*[cite: 19] */

/* 切片安全光栅化渲染 */
void ui_file_browser_draw(ui_tile_t *tile, const ui_file_browser_t *fb); /*[cite: 19] */

#ifdef __cplusplus
}
#endif

#endif /* UI_FILE_BROWSER_H */