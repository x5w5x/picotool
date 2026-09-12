#include "app_settings.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_scrollview.h"
#include "ui_tabview.h"
#include "ui_slider.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "fun_esp.h"

/* =========================================================================
 * 0. 基础常量与选项配置
 * ========================================================================= */
extern char __data_start__;
extern char __bss_end__;
extern char __end__;
extern char __HeapLimit;
extern char __flash_binary_start;
extern char __flash_binary_end;
// 侧边栏
static const char *s_settings_tabs[] = {
    "系统状态", // Tab 0
    "偏好设置", // Tab 1
    "关于"

};
#define TAB_COUNT (sizeof(s_settings_tabs) / sizeof(s_settings_tabs[0]))

#define STATUS_CONTENT_H 520
#define SETTINGS_CONTENT_H 520
#define SETTINGS_CARD_Y 16
#define SETTINGS_SWITCH_Y 78

#define BRIGHTNESS_CARD_Y      148
#define BRIGHTNESS_CARD_H      130

#define BRIGHTNESS_SLIDER_X    35
#define BRIGHTNESS_SLIDER_Y    220
#define BRIGHTNESS_SLIDER_W    270
#define BRIGHTNESS_SLIDER_H    28
/* =========================================================================
 * 1. App 私有数据结构体
 *
 * 【添加新控件 - 第 1 步】：在此声明你的控件实体。
 * 铁律：dirty_rect 必须严格位于第 1 位！
 * ========================================================================= */

typedef struct
{
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab_idx;

    // --- Tab 0 系统状态 ---
    ui_scrollview_t status_scroll;

    // --- Tab 1 专属控件 (常规静态参数配置) ---
    ui_scrollview_t setting_scroll;
    ui_rect_t card_settings; // 配置底板卡片
    ui_switch_t sw_feature;  // 开关控件

    ui_rect_t card_brightness;
    ui_slider_t brightness_slider;
    uint8_t brightness_percent;
    //
    char device_serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1]; // 序列号
    uint32_t uptime_seconds;
    uint32_t last_updata_seconds;

    uint32_t static_ram;
    uint32_t heap;

    uint32_t flash_size;
    uint32_t flash_total;
    uint8_t flash_usage;

    uint32_t sys_clock_hz;
    uint32_t peri_clock_hz;
} settings_data_t;

static app_t s_settings;

/**
 * @brief 矩形安全合并助手（将两个脏矩形合为一个能包住它们的外接大矩形）
 */
static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (a.w == 0 || a.h == 0)
        return b;
    if (b.w == 0 || b.h == 0)
        return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static void format_build_date(char *out, size_t out_size)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char month[4] = {0};
    int day = 0, year = 0, month_idx = 0;
    if (sscanf(FW_BUILD_DATE, "%3s %d %d", month, &day, &year) == 3)
    {
        for (int i = 0; i < 12; ++i)
            if (strcmp(month, months[i]) == 0)
            {
                month_idx = i + 1;
                break;
            }
    }
    if (month_idx)
        snprintf(out, out_size, "%04d年%02d月%02d日", year, month_idx, day);
    else
        snprintf(out, out_size, "%s", FW_BUILD_DATE);
}

/* =========================================================================
 * 2. 交互回调函数
 *
 * 【添加新控件 - 第 2 步】：编写各控件触发的交互回调。
 * 核心法则：如果操作影响了其他界面区域，必须将对应区域并入 dirty_rect！
 * ========================================================================= */

// Tab 切换回调
static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data)
{
    (void)old_tab;
    (void)new_tab;
    settings_data_t *data = (settings_data_t *)user_data;
    if (data)
    {
        data->dirty_rect = tv->content_rect;
    }
}

// Tab 0 系统状态内容。状态项使用虚拟内容坐标，在这里逐步添加。
static void on_status_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                   int16_t scroll_y, void *user_data)
{
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;
    // x/y 是虚拟内容原点映射到屏幕后的坐标。
    const int16_t x = viewport->x;
    const int16_t y = viewport->y - scroll_y;
    uint32_t total = data->uptime_seconds;
    uint32_t hours = (total / 3600) % 24;
    uint32_t minutes = (total / 60) % 60;
    uint32_t seconds = total % 60;
    char uptime_text[32];
    char ram_text[24];
    char heap_text[24];
    char flash_size_t[24];
    char flash_total_t[24];
    char flash_usage_t[16];
    char sys_clock_text[24];
    char peri_clock_text[24];

    snprintf(uptime_text,
             sizeof(uptime_text),
             "%02lu:%02lu:%02lu",
             (unsigned long)hours,
             (unsigned long)minutes,
             (unsigned long)seconds);
    snprintf(ram_text, sizeof(ram_text),
             "%lu KB",
             (unsigned long)((data->static_ram + 1023) / 1024));

    snprintf(heap_text, sizeof(heap_text),
             "%lu KB",
             (unsigned long)(data->heap / 1024));
    snprintf(flash_size_t,
             sizeof(flash_size_t),
             "%lu KiB",
             (unsigned long)((data->flash_size + 1023u) / 1024u));

    snprintf(flash_total_t,
             sizeof(flash_total_t),
             "%lu KiB",
             (unsigned long)(data->flash_total / 1024u));

    snprintf(flash_usage_t,
             sizeof(flash_usage_t),
             "%u%%",
             data->flash_usage);
    snprintf(sys_clock_text, sizeof(sys_clock_text),
             "%lu MHz", (unsigned long)(data->sys_clock_hz / 1000000u));
    snprintf(peri_clock_text, sizeof(peri_clock_text),
             "%lu MHz", (unsigned long)(data->peri_clock_hz / 1000000u));

    // 用轻量分组面板组织状态信息，避免整页散落纯文本。
    ui_rect_t overview = ui_rect_make(x + 8, y + 32, 326, 82);
    ui_rect_t memory = ui_rect_make(x + 8, y + 124, 326, 92);
    ui_rect_t flash = ui_rect_make(x + 8, y + 226, 326, 126);
    ui_rect_t clocks = ui_rect_make(x + 8, y + 362, 326, 112);
    ui_rect_t panel_overlap;
    if (ui_rect_intersect(&tile->rect, &overview, &panel_overlap))
    {
        ui_draw_fill_rect(tile, &panel_overlap, 0xEF7D);
        ui_draw_rect(tile, &overview, 0xC618);
    }
    if (ui_rect_intersect(&tile->rect, &memory, &panel_overlap))
    {
        ui_draw_fill_rect(tile, &panel_overlap, 0xEF7D);
        ui_draw_rect(tile, &memory, 0xC618);
    }
    if (ui_rect_intersect(&tile->rect, &flash, &panel_overlap))
    {
        ui_draw_fill_rect(tile, &panel_overlap, 0xEF7D);
        ui_draw_rect(tile, &flash, 0xC618);
    }
    if (ui_rect_intersect(&tile->rect, &clocks, &panel_overlap))
    {
        ui_draw_fill_rect(tile, &panel_overlap, 0xEF7D);
        ui_draw_rect(tile, &clocks, 0xC618);
    }
    ui_draw_string(tile,
                   x + 16,
                   y + 6,
                   "系统概览",
                   FONT_20,
                   0x4A69,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 20,
                   y + 62,
                   "运行时间",
                   FONT_16,
                   0x632C,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 180,
                   y + 62,
                   uptime_text,
                   FONT_16,
                   0x4A69,
                   0xEF7D);
    ui_draw_string(tile,
                   x + 20,
                   y + 158,
                   "RAM 固定占用",
                   FONT_16,
                   0x632C,
                   0xEF7D);
    ui_draw_string(tile,
                   x + 220,
                   y + 158,
                   ram_text,
                   FONT_16,
                   0x4A69,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 20,
                   y + 192,
                   "Heap 容量",
                   FONT_16,
                   0x632C,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 220,
                   y + 192,
                   heap_text,
                   FONT_16,
                   0x4A69,
                   0xEF7D);
    ui_draw_string(tile,
                   x + 20,
                   y + 260,
                   "固件大小",
                   FONT_16,
                   0x632C,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 220,
                   y + 260,
                   flash_size_t,
                   FONT_16,
                   0x4A69,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 20,
                   y + 294,
                   "Flash 容量",
                   FONT_16,
                   0x632C,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 220,
                   y + 294,
                   flash_total_t,
                   FONT_16,
                   0x4A69,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 20,
                   y + 328,
                   "Flash 使用率",
                   FONT_16,
                   0x4A69,
                   0xEF7D);

    ui_draw_string(tile,
                   x + 220,
                   y + 328,
                   flash_usage_t,
                   FONT_16,
                   0x4A69,
                   0xF7BE);

    ui_draw_string(tile, x + 16, y + 374,
                   "时钟频率", FONT_20, 0x4A69, 0xEF7D);
    ui_draw_string(tile, x + 20, y + 418,
                   "系统时钟", FONT_16, 0x632C, 0xEF7D);
    ui_draw_string(tile, x + 220, y + 418,
                   sys_clock_text, FONT_16, 0x4A69, 0xEF7D);
    ui_draw_string(tile, x + 20, y + 452,
                   "外设时钟", FONT_16, 0x632C, 0xEF7D);
    ui_draw_string(tile, x + 220, y + 452,
                   peri_clock_text, FONT_16, 0x4A69, 0xEF7D);
}

// Tab 1 使用虚拟内容坐标；所有子控件随 scroll_y 一起换算到屏幕坐标。
static void layout_settings_content(settings_data_t *data)
{
    data->card_settings = ui_scrollview_content_rect_to_screen(
        &data->setting_scroll, ui_rect_make(5, SETTINGS_CARD_Y, 330, 112));
         data->card_brightness =
        ui_scrollview_content_rect_to_screen(&data->setting_scroll,ui_rect_make(5,BRIGHTNESS_CARD_Y,330,BRIGHTNESS_CARD_H));
}

static bool on_settings_touch_content(bool is_touching, int16_t content_x,
                                      int16_t content_y, uint32_t now_tick,
                                      void *user_data)
{
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return false;
    bool handled =ui_switch_process_touch(&data->sw_feature,is_touching,content_x,content_y,now_tick);
    if (handled) return true;
    return ui_slider_process_touch(&data->brightness_slider,is_touching,content_x,content_y,now_tick);
    
}

static void on_settings_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                     int16_t scroll_y, void *user_data)
{
    (void)viewport;
    (void)scroll_y;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;

    layout_settings_content(data);
    ui_rect_t overlap;
    if (ui_rect_intersect(&tile->rect, &data->card_settings, &overlap))
    {
        ui_draw_fill_rect(tile, &overlap, 0xF7BE);
        ui_draw_rect(tile, &data->card_settings, 0xD69A);
        ui_draw_string(tile, 140, data->card_settings.y + 14,
                       "系统行为", FONT_20, 0x4A69, 0xF7BE);
        ui_draw_string(tile, 140, data->card_settings.y + 66,
                       "系统呼吸灯", FONT_16, 0x632C, 0xF7BE);
    }
    ui_switch_t screen_switch = data->sw_feature;
    screen_switch.rect = ui_scrollview_content_rect_to_screen(
        &data->setting_scroll, data->sw_feature.rect);
    ui_switch_draw(tile, &screen_switch);

    if(ui_rect_intersect(&tile->rect,&data->card_brightness,&overlap)){
        ui_draw_fill_rect(tile,&overlap,0xF7BE);
        ui_draw_rect(tile,&data->card_brightness,0xD69A);
        ui_draw_string(tile,140,data->card_brightness.y+14,"显示亮度",FONT_20,0x4A69,0xF7BE);
        char brightness_text[16];
        snprintf(brightness_text,sizeof(brightness_text),"%u%%",(unsigned)data->brightness_percent);
        ui_draw_string(tile,data->card_brightness.x+260,data->card_brightness.y+18,brightness_text,FONT_16,0x4A69,0xF7BE);
    }
    ui_slider_t slider = data->brightness_slider;
    slider.rect=ui_scrollview_content_rect_to_screen(&data->setting_scroll,data->brightness_slider.rect);
    ui_slider_draw(tile,&slider);
}

// Tab 1 开关切换回调
static void on_switch_toggled(ui_switch_t *sw, bool state, void *user_data)
{
    (void)sw;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;

    sys_led_blink = state;
    // 将开关卡片合入脏矩形
    data->dirty_rect = rect_union_box(data->dirty_rect, data->card_settings);
}

static void send_brightness_to_esp(uint8_t percent)
{
    if (percent > 90)
        percent = 90;
    else if (percent < 10)
        percent = 10;
    uint32_t pwm = ((uint32_t)percent * 255u + 50u) / 100u;
    char cmd[24];

    snprintf(cmd,sizeof(cmd),"#PWM:%lu\r\n",(unsigned long)pwm
);
    esp_send_string(cmd);
    // if(esp_readable()){
    //     uint8_t rx_buff[64];
    //     size_t rx_len= esp_read(rx_buff,sizeof(rx_buff)-1);
    //     rx_buff[rx_len]='\0';
    //      printf(
    //     "ESP RX: %s\r\n",
    //     (char *)rx_buff
    // );
    // }
}
static void on_brightness_change(ui_slider_t *slider, int32_t value, void *user_data)
{
    settings_data_t *data =
        (settings_data_t *)user_data;

    if (data == NULL)
    {
        return;
    }
    if (value < 10)
        value = 10;
    else if (value > 90)
        value = 90;
    data->brightness_percent=(uint8_t)value;
    led_brigthness=data->brightness_percent;
    send_brightness_to_esp(data->brightness_percent);
    data->dirty_rect=rect_union_box(data->dirty_rect,data->card_brightness);
}
/* =========================================================================
 * 3. App 生命周期管理
 * ========================================================================= */

static void settings_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void settings_enter(app_t *self)
{

    settings_data_t *data = (settings_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(settings_data_t));
    // 获取运行时间
    data->uptime_seconds = to_ms_since_boot(get_absolute_time()) / 1000;
    data->last_updata_seconds = to_ms_since_boot(get_absolute_time());

    data->static_ram = (uint32_t)(&__bss_end__ - &__data_start__);
    data->heap = (uint32_t)(&__HeapLimit - &__end__);

    data->flash_size = (uint32_t)(&__flash_binary_end - &__flash_binary_start);
    data->flash_total = BOARD_FLASH_SIZE_BYTES;
    data->flash_usage = (uint8_t)(((uint64_t)data->flash_size * 100u) / data->flash_total);
    data->sys_clock_hz = clock_get_hz(clk_sys);
    data->peri_clock_hz = clock_get_hz(clk_peri);

    pico_get_unique_board_id_string(data->device_serial, sizeof(data->device_serial));

    // 首屏默认全量重绘一次
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = 0;
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_settings_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0 系统状态滚动视图，与偏好设置保持相同的视口规格。
    ui_scrollview_init(&data->status_scroll, 120, 40, 350, 260,
                       STATUS_CONTENT_H, on_status_draw_content);
    data->status_scroll.user_data = data;
    data->status_scroll.color_bg = 0xFFFF;
    data->status_scroll.color_scrollbar = 0xA514;
    data->status_scroll.show_scrollbar = true;

    // 4. Tab 1 控件排版：
    ui_scrollview_init(&data->setting_scroll, 120, 40, 350, 260,
                       SETTINGS_CONTENT_H, on_settings_draw_content);
    data->setting_scroll.user_data = data;
    data->setting_scroll.color_bg = 0xFFFF;
    data->setting_scroll.color_scrollbar = 0xA514;
    data->setting_scroll.show_scrollbar = true;
    ui_scrollview_set_touch_cb(&data->setting_scroll, on_settings_touch_content);

    ui_switch_init(&data->sw_feature, 265, SETTINGS_SWITCH_Y, 64, 32,
                   sys_led_blink, on_switch_toggled);
    data->sw_feature.user_data = data;

    data->brightness_percent=led_brigthness;
    ui_slider_init(&data->brightness_slider,BRIGHTNESS_SLIDER_X,BRIGHTNESS_SLIDER_Y,BRIGHTNESS_SLIDER_W,BRIGHTNESS_SLIDER_H,0,100,data->brightness_percent,on_brightness_change);
    data->brightness_slider.user_data=data;
    // ui_slider_set_color(&data->brightness_slider, 0xD69A, 0x04BF, 0xFFFF);
    ui_slider_set_color(&data->brightness_slider, 0xDEFB, 0xBF5E, 0xFFFF);
    layout_settings_content(data);


}

static void settings_exit(app_t *self)
{
    self->user_data = NULL; // 退出解绑
}

/* =========================================================================
 * 4. 定时轮询与后台更新 (在主循环被高频调用)
 * ========================================================================= */

static void settings_update(app_t *self, uint32_t now_tick)
{
    (void)now_tick;
    settings_data_t *data = (settings_data_t *)self->user_data;
    if (!data)
        return;

    // 关键：进入新一帧时无条件清零脏矩形
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 若有后台数据需要定时刷新，在此按需标脏对应卡片或视口
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - data->last_updata_seconds >= 1000)
    {
        data->last_updata_seconds = now_ms;
        data->uptime_seconds = now_ms / 1000;
        data->sys_clock_hz = clock_get_hz(clk_sys);
        data->peri_clock_hz = clock_get_hz(clk_peri);
        if (data->tabview.active_tab_idx == 0)
        {
            data->dirty_rect = data->status_scroll.view_rect;
        }
    }
}

/* =========================================================================
 * 5. 触控事件分发
 *
 * 【添加新控件 - 第 4 步】：按层级将触摸数据流分发给控件。
 * 顺序：全局控件 -> TabView 侧边栏 -> 当前 Tab 内部控件
 * ========================================================================= */

static bool settings_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    settings_data_t *data = (settings_data_t *)self->user_data;
    if (!data)
        return false;

    // 2. 侧边栏导航分发 (切页检测)
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick))
    {
        if (data->tabview.active_tab_idx != data->last_tab_idx)
        {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect = data->tabview.rect; // 切页整屏重绘
        }
        else
        {
            data->dirty_rect = ui_tabview_get_bar_dirty_rect(&data->tabview); // 仅指示条重绘
        }
        return true;
    }

    bool handled = false;

    // 3. 根据当前激活的 Tab 分层分流
    switch (data->tabview.active_tab_idx)
    {
    case 0: // --- Tab 0: 系统状态滚动视图 ---
        if (ui_scrollview_process_touch(&data->status_scroll, is_touching, tx, ty, now_tick))
        {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
            handled = true;
        }
        break;

    case 1: // --- Tab 1: 可滚动偏好设置 ---
        // ScrollView 统一完成坐标转换、拖动识别和子控件事件分发。
        if (ui_scrollview_process_touch(&data->setting_scroll, is_touching, tx, ty, now_tick))
        {
            layout_settings_content(data);
            data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
            handled = true;
        }
        break;

    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* =========================================================================
 * 6. Tile 切片光栅化渲染
 *
 * 【添加新控件 - 第 5 步】：调用控件 draw API。
 * 顺序：底板 -> 全局控件 -> Tab 外壳 -> Early Exit 门禁 -> Tab 内部控件
 * ========================================================================= */

static void settings_draw(app_t *self, ui_tile_t *tile)
{
    settings_data_t *data = (settings_data_t *)self->user_data;
    if (!data)
        return;
    ui_rect_t overlap;

    // 1. 底板涂白
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap))
    {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    // 3. 绘制 TabView 外壳与侧边栏
    ui_tabview_draw(tile, &data->tabview);

    // 4. 性能门禁：若切片不在右侧内容工作区内，立即退出 (大幅降低 CPU 开销)
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap))
    {
        return;
    }

    // 5. 按当前激活 Tab 分层光栅化内部控件
    switch (data->tabview.active_tab_idx)
    {
    case 0:
        ui_scrollview_draw(tile, &data->status_scroll);
        break;

    case 1:
        ui_scrollview_draw(tile, &data->setting_scroll);
        break;

    // case 2:
    //     ui_draw_string(tile,260,58,"PICO",FONT_24,0xC618,0xFFFF);
    //     ui_draw_string(tile,188,82,"多功能嵌入式工具",FONT_24,0xC618,0XFFFF);

    //     break;
    case 2:
    {
        const int16_t x = data->tabview.content_rect.x + 8;
        const int16_t y = data->tabview.content_rect.y + 28;
        char build_date[32];
        format_build_date(build_date, sizeof(build_date));

        // About 页不是滚动视图，两个面板必须完整落在 288px 内容区内。
        ui_rect_t brand = ui_rect_make(x + 4, y + 2, 342, 80);
        ui_rect_t info = ui_rect_make(x + 4, y + 92, 342, 160);
        ui_rect_t panel;
        if (ui_rect_intersect(&tile->rect, &brand, &panel))
        {
            ui_draw_fill_rect(tile, &panel, 0xE71C);
            ui_draw_rect(tile, &brand, 0xC618);
        }
        if (ui_rect_intersect(&tile->rect, &info, &panel))
        {
            ui_draw_fill_rect(tile, &panel, 0xE71C);
            ui_draw_rect(tile, &info, 0xC618);
        }

        ui_draw_string(tile,
                       x + 125,
                       y + 8,
                       "PICO",
                       FONT_24,
                       0xC618,
                       0xE71C);

        ui_draw_string(tile, x + 53, y + 28,
                       "多功能嵌入式工具",
                       FONT_20,
                       0xC618,
                       0xE71C);
        ui_draw_string(tile, x + 53 + 7 * 24, y + 28 + 22,
                       "__轩",
                       FONT_20,
                       0xC618,
                       0xE71C);

        ui_draw_string(tile, x + 18, y + 98,
                       "固件版本: " FW_VERSION_STRING,
                       FONT_20,
                       0x632C,
                       0xE71C);

        ui_draw_string(tile, x + 18, y + 126,
                       "硬件版本: " HW_VERSION_STRING,
                       FONT_20,
                       0x632C,
                       0xE71C);

        ui_draw_string(tile, x + 18, y + 154,
                       "SDK 版本: " SDK_VERSION_STRING,
                       FONT_20,
                       0x632C,
                       0xE71C);
        ui_draw_string(tile, x + 18, y + 182,
                       "构建时间: ",
                       FONT_20,
                       0x632C,
                       0xE71C);
        ui_draw_string(tile, x + 114, y + 182,
                       build_date,
                       FONT_20,
                       0x632C,
                       0xE71C);

        ui_draw_string(tile, x + 126, y + 204,
                       FW_BUILD_TIME,
                       FONT_20,
                       0x632C,
                       0xE71C);

        ui_draw_string(tile, x + 18, y + 230,
                       "设备序列号: ",
                       FONT_16,
                       0x632C,
                       0xE71C);
        ui_draw_string(tile, x + 114, y + 230,
                       data->device_serial,
                       FONT_16,
                       0x4A69,
                       0xE71C);

        break;
    }
    default:
        break;
    }
}

/* =========================================================================
 * 7. 导出单例实体 (注册到系统)
 * ========================================================================= */

app_t *app_settings_get_instance(void)
{
    s_settings.id = APP_ID_SETTINGS; // 注册到 app_manager 的枚举 ID
    s_settings.name = "系统设置";    // 桌面 App 图标名称
    s_settings.init = settings_init;
    s_settings.enter = settings_enter;
    s_settings.exit = settings_exit;
    s_settings.update = settings_update;
    s_settings.process_touch = settings_process_touch;
    s_settings.draw = settings_draw;
    return &s_settings;
}
