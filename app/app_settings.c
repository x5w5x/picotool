#include "app_settings.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_scrollview.h"
#include "ui_tabview.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "config.h"

/* =========================================================================
 * 0. 基础常量与选项配置
 * ========================================================================= */

// 侧边栏
static const char *s_settings_tabs[] = {
    "系统状态", // Tab 0
    "偏好设置", // Tab 1
    "关于"

};
#define TAB_COUNT (sizeof(s_settings_tabs) / sizeof(s_settings_tabs[0]))

#define SETTINGS_CONTENT_H       520
#define SETTINGS_CARD_Y          16
#define SETTINGS_SWITCH_Y        78

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

    // --- Tab 0 专属控件 (滚动视口 + 视口外部固定按钮) ---
    ui_scrollview_t scroll_view; // 滚动视口容器 (虚拟长画布)
    ui_btn_t btn_tab0_fixed;     // 视口外部的固定操作按钮 (不随滚动条位移)

    // --- Tab 1 专属控件 (常规静态参数配置) ---
    ui_scrollview_t setting_scroll;
    ui_rect_t card_settings; // 配置底板卡片
    ui_switch_t sw_feature;  // 开关控件
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
    if (sscanf(FW_BUILD_DATE, "%3s %d %d", month, &day, &year) == 3) {
        for (int i = 0; i < 12; ++i)
            if (strcmp(month, months[i]) == 0) { month_idx = i + 1; break; }
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
        // 标签切换时，整个右侧内容工作区标记为脏区域，触发重新光栅化
        data->dirty_rect = tv->content_rect;
    }
}

// 全局按钮点击回调 (位于 TabView 外部)
static void on_global_btn_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;

    printf("点击了全局常驻按钮！\r\n");
    // 刷新自身按压态
    data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(btn));
}

// Tab 0 内部滚动视口的内容绘制代理回调
static void on_scrollview_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    (void)user_data;

    // 假设长画布排布 6 个信息卡片，每个卡片高 36px，间距 8px
    const int16_t item_h = 36;
    const int16_t item_gap = 8;
    const int16_t item_w = viewport->w - 18; // 预留右侧滚动条滑块空间

    for (int i = 0; i < 6; i++)
    {
        // 1. 换算当前卡片在屏幕上的绝对物理 Y 坐标
        int16_t screen_y = viewport->y + i * (item_h + item_gap) - scroll_y;

        // 2. 算术剪枝：超出物理视口上下边界的不参与计算
        if (screen_y + item_h <= viewport->y || screen_y >= viewport->y + viewport->h)
        {
            continue;
        }

        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, screen_y, item_w, item_h);
        ui_rect_t overlap;

        // 3. 切片安全相交裁剪光栅化
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap))
        {
            ui_draw_fill_rect(tile, &overlap, 0xEF7D); // 底板浅灰
            ui_draw_rect(tile, &item_rect, 0xC618);    // 边框灰

            char buf[32];
            snprintf(buf, sizeof(buf), "监控节点 #%d 运行正常", i + 1);
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 10, buf, FONT_16, 0x1A7B, 0xEF7D);
        }
    }
}

// Tab 1 使用虚拟内容坐标；所有子控件随 scroll_y 一起换算到屏幕坐标。
static void layout_settings_content(settings_data_t *data)
{
    const int16_t y = data->setting_scroll.view_rect.y - data->setting_scroll.scroll_y;
    data->card_settings = ui_rect_make(125, y + SETTINGS_CARD_Y, 330, 112);
    data->sw_feature.rect.x = 385;
    data->sw_feature.rect.y = y + SETTINGS_SWITCH_Y;
}

static void on_settings_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                     int16_t scroll_y, void *user_data)
{
    (void)viewport;
    (void)scroll_y;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data) return;

    layout_settings_content(data);
    ui_rect_t overlap;
    if (ui_rect_intersect(&tile->rect, &data->card_settings, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xF7BE);
        ui_draw_rect(tile, &data->card_settings, 0xD69A);
        ui_draw_string(tile, 140, data->card_settings.y + 14,
                       "系统行为", FONT_20, 0x4A69, 0xF7BE);
        ui_draw_string(tile, 140, data->card_settings.y + 66,
                       "LED 状态灯", FONT_16, 0x632C, 0xF7BE);
    }
    ui_switch_draw(tile, &data->sw_feature);
}

// Tab 0 视口外部的固定按钮点击回调
static void on_tab0_fixed_btn_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;

    printf("点击了 Tab 0 视口外的固定操作按钮！\r\n");
    // 重绘整个滚动视口区域 (例如模拟刷新了数据)
    data->dirty_rect = rect_union_box(data->dirty_rect, data->scroll_view.view_rect);
}

// Tab 1 开关切换回调
static void on_switch_toggled(ui_switch_t *sw, bool state, void *user_data)
{
    (void)sw;
    settings_data_t *data = (settings_data_t *)user_data;
    if (!data)
        return;

    sys_led_blink = !state;
    // 将开关卡片合入脏矩形
    data->dirty_rect = rect_union_box(data->dirty_rect, data->card_settings);
}

/* =========================================================================
 * 3. App 生命周期管理
 * ========================================================================= */

static void settings_init(app_t *self)
{
    self->user_data = g_static_app_ram; // 挂载共享内存基址
}

static void settings_enter(app_t *self)
{
    // ⚡ 进入时从共享静态池格式化，清零历史残留
    settings_data_t *data = (settings_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(settings_data_t));

    // 首屏默认全量重绘一次
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = 0;

    // 2. TabView 侧边栏容器：(X:0, Y:32, 宽:480, 高:288)，侧边栏宽 110px
    // 工作区 content_rect 自动计算为 (X:110, Y:32, 宽:370, 高:288)
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_settings_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // 3. Tab 0 控件排版：
    // A. 滚动视口：物理窗口放置在 (X:125, Y:42)，宽 340，高 195；虚拟内容长画布高 280px
    ui_scrollview_init(&data->scroll_view, 125, 42, 340, 195, 280, on_scrollview_draw_content);
    data->scroll_view.user_data = data;
    data->scroll_view.color_bg = 0xFFFF;        // 视口纯白底
    data->scroll_view.color_scrollbar = 0x8410; // 暗灰滚动条
    data->scroll_view.show_scrollbar = true;

    // B. 视口外部固定按钮：放置在视口下方 (X:125, Y:248)，宽 340，高 40 (不随视口滚动)
    ui_btn_init(&data->btn_tab0_fixed, 125, 248, 340, 40, "刷新列表数据", on_tab0_fixed_btn_click);
    data->btn_tab0_fixed.user_data = data;
    data->btn_tab0_fixed.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_tab0_fixed, 0x05E0, 0x03E0, 0xFFFF);

    // 4. Tab 1 控件排版：
    ui_scrollview_init(&data->setting_scroll, 120, 40, 350, 260,
                       SETTINGS_CONTENT_H, on_settings_draw_content);
    data->setting_scroll.user_data = data;
    data->setting_scroll.color_bg = 0xFFFF;
    data->setting_scroll.color_scrollbar = 0xA514;
    data->setting_scroll.show_scrollbar = true;

    ui_switch_init(&data->sw_feature, 385, 0, 64, 32,
                   sys_led_blink, on_switch_toggled);
    data->sw_feature.user_data = data;
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
    case 0: // --- Tab 0: 滚动列表与固定按钮 ---
        // A. 处理视口外固定按钮
        if (ui_btn_process_touch(&data->btn_tab0_fixed, is_touching, tx, ty, now_tick))
        {
            data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_tab0_fixed));
            handled = true;
        }
        // B. 处理视口拖拽滚动
        else if (ui_scrollview_process_touch(&data->scroll_view, is_touching, tx, ty, now_tick))
        {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->scroll_view.view_rect);
            handled = true;
        }
        break;

    case 1: // --- Tab 1: 可滚动偏好设置 ---
        layout_settings_content(data);

        // ScrollView 始终先接收完整手势，以便识别拖动并取得触控捕获。
        if (ui_scrollview_process_touch(&data->setting_scroll, is_touching, tx, ty, now_tick)) {
            layout_settings_content(data);
            data->dirty_rect = rect_union_box(data->dirty_rect, data->setting_scroll.view_rect);
            handled = true;
        }

        // 仅当没有形成滚动手势且控件仍位于视口内时才允许开关响应。
        if (!data->setting_scroll.is_dragged &&
            (ui_rect_contains_point(&data->setting_scroll.view_rect, tx, ty) ||
             data->sw_feature.is_touch_down) &&
            ui_switch_process_touch(&data->sw_feature, is_touching, tx, ty, now_tick)) {
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
        // A. 光栅化滚动视口
        ui_scrollview_draw(tile, &data->scroll_view);
        // B. 光栅化外部固定按钮
        ui_btn_draw(tile, &data->btn_tab0_fixed);
        break;

    case 1:
        ui_scrollview_draw(tile, &data->setting_scroll);
        break;

    // case 2:
    //     ui_draw_string(tile,260,58,"PICO",FONT_24,0xC618,0xFFFF);
    //     ui_draw_string(tile,188,82,"多功能嵌入式工具",FONT_24,0xC618,0XFFFF);

    

    //     break;
    case 2: {
    const int16_t x = data->tabview.content_rect.x + 28;
    const int16_t y = data->tabview.content_rect.y + 28;
    char build_date[32];
    format_build_date(build_date, sizeof(build_date));

    ui_draw_string(tile,
                   x + 125,
                   y,
                   "PICO",
                   FONT_24,
                   0xC618,
                   0xFFFF);

    ui_draw_string(tile,x + 53,y + 30,
                   "多功能嵌入式工具",
                   FONT_20,
                   0xC618,
                   0xFFFF);

    ui_draw_string(tile, x, y + 78,
                   "固件版本: " FW_VERSION_STRING,
                   FONT_20,
                   0x632C,
                   0xFFFF);

    ui_draw_string(tile, x, y + 104,
                   "硬件版本: " HW_VERSION_STRING,
                   FONT_20,
                   0x632C,
                   0xFFFF);

    ui_draw_string(tile, x, y + 130,
                   "SDK 版本: " SDK_VERSION_STRING,
                   FONT_20,
                   0x632C,
                   0xFFFF);
    ui_draw_string(tile, x, y + 156,
                   "构建时间: ",
                   FONT_20,
                   0x632C,
                   0xFFFF);
    ui_draw_string(tile, x+96, y + 156,
                   build_date,
                   FONT_20,
                   0x632C,
                   0xFFFF);

    ui_draw_string(tile, x+108, y + 182,
                   FW_BUILD_TIME,
                   FONT_20,
                   0x632C,
                   0xFFFF);

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
