#include "app_dap.h"
#include "app_shared_ram.h"
#include "ui_tabview.h"
#include "ui_scrollview.h"
#include "ui_draw.h"
#include "lcd.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *s_dap_tabs[] = { "状态", "配置", "终端" };
#define DAP_TAB_COUNT ((uint8_t)(sizeof(s_dap_tabs) / sizeof(s_dap_tabs[0])))
#define DAP_CONTENT_H 420

typedef struct {
    /* 必须放在第一个字段，app_manager 依赖它获取脏矩形。 */
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab;
    ui_scrollview_t scroll[DAP_TAB_COUNT];
} dap_data_t;

static app_t s_dap;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (!a.w || !a.h) return b;
    if (!b.w || !b.h) return a;
    int16_t x1 = a.x < b.x ? a.x : b.x;
    int16_t y1 = a.y < b.y ? a.y : b.y;
    int16_t x2 = (a.x + a.w) > (b.x + b.w) ? a.x + a.w : b.x + b.w;
    int16_t y2 = (a.y + a.h) > (b.y + b.h) ? a.y + a.h : b.y + b.h;
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static void dap_tab_changed(ui_tabview_t *tv, uint8_t old_tab,
                            uint8_t new_tab, void *user_data)
{
    (void)old_tab; (void)new_tab;
    dap_data_t *data = (dap_data_t *)user_data;
    if (data) data->dirty_rect = tv->rect;
}

static void dap_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                             int16_t scroll_y, void *user_data)
{
    (void)scroll_y;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    int16_t x = viewport->x + 16;
    int16_t y = viewport->y + 20 - data->scroll[data->tabview.active_tab_idx].scroll_y;
    ui_draw_string(tile, x, y, s_dap_tabs[data->tabview.active_tab_idx],
                   FONT_20, 0x4208, 0xFFFF);
}

static void dap_init(app_t *self) { self->user_data = g_static_app_ram; }

static void dap_enter(app_t *self)
{
    dap_data_t *data = (dap_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(*data));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_dap_tabs, DAP_TAB_COUNT, 0, dap_tab_changed);
    data->tabview.user_data = data;
    for (uint8_t i = 0; i < DAP_TAB_COUNT; ++i) {
        ui_scrollview_init(&data->scroll[i], 120, 40, 350, 260,
                           DAP_CONTENT_H, dap_draw_content);
        data->scroll[i].user_data = data;
        data->scroll[i].color_bg = 0xFFFF;
        data->scroll[i].color_scrollbar = 0xA514;
        data->scroll[i].show_scrollbar = true;
    }
}

static void dap_exit(app_t *self) { self->user_data = NULL; }
static void dap_update(app_t *self, uint32_t now_tick)
{ (void)self; (void)now_tick; }

static bool dap_process_touch(app_t *self, bool touching, int16_t x,
                              int16_t y, uint32_t tick)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return false;
    if (ui_tabview_process_touch(&data->tabview, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.rect;
        return true;
    }
    uint8_t tab = data->tabview.active_tab_idx;
    if (ui_scrollview_process_touch(&data->scroll[tab], touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect,
                                          data->scroll[tab].view_rect);
        return true;
    }
    return false;
}

static void dap_draw(app_t *self, ui_tile_t *tile)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t overlap;
    if (ui_rect_intersect(&tile->rect, &body, &overlap))
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    ui_tabview_draw(tile, &data->tabview);
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) return;
    ui_scrollview_draw(tile, &data->scroll[data->tabview.active_tab_idx]);
}

app_t *app_dap_get_instance(void)
{
    s_dap.id = APP_ID_CUSTOM_0;
    s_dap.name = "DAP工具";
    s_dap.init = dap_init;
    s_dap.enter = dap_enter;
    s_dap.exit = dap_exit;
    s_dap.update = dap_update;
    s_dap.process_touch = dap_process_touch;
    s_dap.draw = dap_draw;
    return &s_dap;
}
