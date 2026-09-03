// /*
//  * @Author: 轩
//  * @Date: 2026-08-31 13:24:12
//  * @LastEditTime: 2026-08-31 13:51:12
//  * @FilePath: \pico\app\app_template.c
//  */
// #include "app_template.h"
// #include "app_shared_ram.h" // ⚡ 引入共享内存头文件
// #include "ui_draw.h"
// #include "ui_btn.h"
// #include "lcd.h"
// #include <stdio.h>
// #include <string.h>

// /* ================= 1. 组件与外设交互回调 ================= */

// static void on_hello_btn_click(ui_btn_t *btn, void *user_data) {
//     (void)btn;
//     (void)user_data;
//     printf("hello\r\n");
// }

// /* ================= 2. App 私有数据结构体 ================= */

// typedef struct {
//     ui_rect_t dirty_rect; // ⚠️ 必须保持在第 1 位！供 app_manager 提取局部脏矩形
//     ui_btn_t  btn_hello;  // 业务控件实体
// } app_template_data_t;

// static app_t               s_app_instance;

// /* ================= 3. 生命周期与事件回调 ================= */

// static void app_template_init(app_t *self) {
//     // ⚡ 仅绑定共享静态池指针
//     self->user_data = g_static_app_ram;
// }

// static void app_template_enter(app_t *self) {
//     // ⚡ 进入 App 时从 8KB 静态池就地初始化
//     app_template_data_t *data = (app_template_data_t *)g_static_app_ram;
//     self->user_data = data;
//     memset(data, 0, sizeof(app_template_data_t));

//     data->dirty_rect = ui_rect_make(0, 0, 0, 0);

//     // 控件初始化 (X: 190, Y: 130, 宽: 75, 高: 36)
//     ui_btn_init(&data->btn_hello, 190, 130, 75, 36, "你好", on_hello_btn_click);
//     data->btn_hello.style     = UI_BTN_STYLE_ROUND;
//     data->btn_hello.font_size = FONT_20;
//     ui_btn_set_color(&data->btn_hello, 0xBF5E, 0xC618, 0x1A7B);
//     data->btn_hello.user_data = data;
// }

// static void app_template_exit(app_t *self) {
//     self->user_data = NULL;
// }

// static void app_template_update(app_t *self, uint32_t now_tick) {
//     (void)self;
//     (void)now_tick;
// }

// static bool app_template_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
//     app_template_data_t *data = (app_template_data_t *)self->user_data;
//     if (!data) return false;

//     if (ui_btn_process_touch(&data->btn_hello, is_touching, tx, ty, now_tick)) {
//         data->dirty_rect = ui_btn_get_dirty_rect(&data->btn_hello);
//         return true;
//     }
//     return false;
// }

// /* ================= 4. 切片光栅化渲染 ================= */

// static void app_template_draw(app_t *self, ui_tile_t *tile) {
//     app_template_data_t *data = (app_template_data_t *)self->user_data;
//     if (!data) return;
//     ui_rect_t overlap;

//     ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//     if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
//         ui_draw_fill_rect(tile, &overlap, 0xFFFF);
//     }

//     ui_btn_draw(tile, &data->btn_hello);
// }

// /* ================= 5. 导出单例实体 ================= */

// app_t *app_template_get_instance(void) {
//     s_app_instance.id            = APP_ID_SETTINGS;
//     s_app_instance.name          = "空白模板";
//     s_app_instance.init          = app_template_init;
//     s_app_instance.enter         = app_template_enter;
//     s_app_instance.exit          = app_template_exit;
//     s_app_instance.update        = app_template_update;
//     s_app_instance.process_touch = app_template_process_touch;
//     s_app_instance.draw          = app_template_draw;
//     return &s_app_instance;
// }
#include "app_template.h"
#include "app_shared_ram.h"  // ⚡ 核心：所有 App 共享的静态 RAM 缓冲池[cite: 1, 3]
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
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
//左侧面板
static const char *s_tabs[] = {
    "监控主页",
    "参数配置",
    "测试",
    "频率"
};
#define TAB_COUNT (sizeof(s_tabs) / sizeof(s_tabs[0]))

/* =========================================================================
 * 1. App 私有数据结构体
 * 
 * 📌【添加新控件 - 第 1 步】：
 * 在这里声明控件对象实体（如 ui_btn_t、ui_switch_t、ui_slider_t 等）。
 * ⚠️ 注意：dirty_rect 必须严格放在第 1 位！
 * ========================================================================= */

typedef struct {
    ui_rect_t       dirty_rect;     // ⚠️ 严禁移动！框架用于跨切片局部增量重绘
    ui_tabview_t    tabview;        // 页面多标签容器
    uint8_t         last_tab_idx;   // 记录上一次所在标签页，判断是否整屏重绘

    // --- 业务数据变量 ---
    uint32_t        counter;
    uint32_t        last_tick_ms;

    // --- Tab 0 专属控件与布局卡片 ---
    ui_rect_t       card_info;      // 信息展示卡片底框
    ui_btn_t        btn_action;     // 原有按钮：用于计数
    
    /* -------------------------------------------------------------
     * 🟢 [示例新增控件 1]：在 Tab 0 添加一个复位重置按钮
     * ------------------------------------------------------------- */
    ui_btn_t        btn_reset;

    // --- Tab 1 专属控件与布局卡片 ---
    ui_rect_t       card_settings;
    ui_switch_t     sw_feature;     // 原有开关控件
} app_template_data_t;

static app_t s_app_template;

/**
 * @brief 矩形安全合并助手（将两个脏矩形合为一个能包住它们的外接大矩形）
 */
static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* =========================================================================
 * 2. 交互回调函数
 * 
 * 📌【添加新控件 - 第 2 步】：
 * 为支持点击/交互的控件编写回调函数。
 * 核心法则：如果控件点击后改变了界面上的某个区域，必须把被修改区域并入 dirty_rect！
 * ========================================================================= */

// Tab 切换回调
static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    app_template_data_t *data = (app_template_data_t *)user_data;
    if (data) {
        // 标签切换时，整个内容工作区都标记为脏区域，触发底层切片重绘
        data->dirty_rect = tv->content_rect;
    }
}

// 原有按钮回调：计数 + 1
static void on_btn_action_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_template_data_t *data = (app_template_data_t *)user_data;
    if (!data) return;

    data->counter++;
    // 计数改变了 card_info 上的文本，所以把 card_info 标记为待重绘区域
    data->dirty_rect = rect_union_box(data->dirty_rect, data->card_info);
}

/* -------------------------------------------------------------
 * 🟢 [示例新增控件 2]：编写新增复位按钮的点击业务回调
 * ------------------------------------------------------------- */
static void on_btn_reset_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_template_data_t *data = (app_template_data_t *)user_data;
    if (!data) return;

    data->counter = 0; // 计数值清零
    // 通知 UI 引擎：card_info 里面的内容已经变了，需要重新光栅化此区域
    data->dirty_rect = rect_union_box(data->dirty_rect, data->card_info);
}

// 原有开关切换回调
static void on_switch_toggled(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw; (void)state;
    app_template_data_t *data = (app_template_data_t *)user_data;
    if (!data) return;

    // 开关状态变化后的业务逻辑...
    data->dirty_rect = rect_union_box(data->dirty_rect, data->card_settings);
    sys_led_blink = !state;
}

/* =========================================================================
 * 3. App 生命周期管理
 * ========================================================================= */

static void app_template_init(app_t *self) {
    //仅绑定共享静态 RAM 内存指针，不要在这里分配动态堆内存
    self->user_data = g_static_app_ram;
}

static void app_template_enter(app_t *self) {
    //  每次进入该 App 时，将 8KB 共享池格式化并绑定指针
    app_template_data_t *data = (app_template_data_t *)g_static_app_ram;
    self->user_data = data;
    memset(data, 0, sizeof(app_template_data_t)); // 清零脏数据

    // 默认全屏强制重绘一次
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->counter    = 0;

    // 初始化 TabView 容器 (顶部留出 32px 状态栏)
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_tabs, TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data; // 绑定回传指针

    /* -------------------------------------------------------------
     * 📌【添加新控件 - 第 3 步】：
     * 在对应页面的布局代码区进行【坐标设定】、【属性配置】与【事件绑定】
     * ------------------------------------------------------------- */

    // Tab 0 控件布局
     data->card_info = ui_rect_make(125, 48, 340, 100);

    // 控件 A：累加按钮 (X:130, Y:170, W:150, H:44)
    ui_btn_init(&data->btn_action, 130, 170, 150, 44, "累加计数", on_btn_action_click);
    data->btn_action.user_data = data;
    data->btn_action.style     = UI_BTN_STYLE_ROUND;
    data->btn_action.font_size = FONT_16;
    ui_btn_set_color(&data->btn_action, 0x05E0, 0x03E0, 0xFFFF);

    // 🟢 [示例新增控件 3]：初始化复位按钮 (紧挨累加按钮放置，X:300, Y:170, W:150, H:44)
    ui_btn_init(&data->btn_reset, 300, 170, 150, 44, "清空重置", on_btn_reset_click);
    data->btn_reset.user_data = data;                   // 必填：回调时找回数据上下文[cite: 1, 3]
    data->btn_reset.style     = UI_BTN_STYLE_ROUND;     // 样式：圆角按钮
    data->btn_reset.font_size = FONT_16;                // 字体大小
    ui_btn_set_color(&data->btn_reset, 0xF800, 0xC800, 0xFFFF); // 红色危险告警风格

    // Tab 1 控件布局
     data->card_settings = ui_rect_make(125, 48, 340, 100);
    ui_switch_init(&data->sw_feature, 385, 168, 64, 32, false, on_switch_toggled);
    data->sw_feature.user_data = data;

    // (可选) 在此初始化硬件外设，例如 GPIO, DMA, UART, SPI 等...
}

static void app_template_exit(app_t *self) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (data) {
        // (可选) 退出时必须安全注销中断、停用 DMA、释放硬件外设[cite: 1, 3]
    }
    self->user_data = NULL; // 断开指针连接[cite: 1, 3]
}

/* =========================================================================
 * 4. 定时轮询与后台更新 (在主循环被高频调用)
 * ========================================================================= */

static void app_template_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (!data) return;

    // ⚠️ 关键：进入新一帧周期时，先重置脏矩形为空
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 硬件与数据节流采集（例如每 100ms 检查一次传感器或协议收发）[cite: 1]
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - data->last_tick_ms >= 100) {
        data->last_tick_ms = now_ms;

        // 如果只有处于 Tab 0 时才需要展示动态数据，则按条件产生脏矩形[cite: 1]
        if (data->tabview.active_tab_idx == 0) {
            // data->dirty_rect = data->card_info; // 触发刷新
        }
    }
}

/* =========================================================================
 * 5. 触控事件分发
 * 
 * 📌【添加新控件 - 第 4 步】：
 * 在这里接收屏幕触控并分发给控件的 process_touch 函数。
 * 如果控件被按下/弹起（状态改变），必须捕获其脏矩形并向上通知系统。
 * ========================================================================= */

static bool app_template_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (!data) return false;

    // A. 顶级浮层优先拦截（如果页面上有弹出的键盘、下拉框或 Modal，先给它们处理）[cite: 1]
    // if (data->kb.is_open) { ... return true; }

    // B. 全局容器拦截：Tab 栏点击
    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect; // 切页整屏重绘[cite: 1]
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview); // 仅高亮条重绘[cite: 1]
        }
        return true;
    }

    bool handled = false;

    // C. 当前激活 Tab 页面内部的控件触控分发
    switch (data->tabview.active_tab_idx) {
        case 0: // --- Tab 0 监控页 ---
            // 控件 A 处理触控
            if (ui_btn_process_touch(&data->btn_action, is_touching, tx, ty, now_tick)) {
                // 按钮被按压或抬手时，合并按钮自身区域为脏矩形，显示下陷/回弹效果[cite: 1, 3]
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_action));
                handled = true;
            }

            // 🟢 [示例新增控件 4]：把触控数据流同步分发给新增的复位按钮
            if (ui_btn_process_touch(&data->btn_reset, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_reset));
                handled = true;
            }
            break;

        case 1: // --- Tab 1 配置页 ---
            if (ui_switch_process_touch(&data->sw_feature, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_feature));
                handled = true;
            }
            break;

        default:
            break;
    }

    // 只要有任何控件被激活（或者产生了待刷新区域），就返回 true 请求显存刷新[cite: 3]
    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* =========================================================================
 * 6. Tile 切片光栅化渲染
 * 
 * 📌【添加新控件 - 第 5 步】：
 * 在 draw 函数中调用控件的专属绘制 API（如 ui_btn_draw）。
 * ⚠️ 切片原则：不可直接无脑绘图！
 * 自定义卡片背景或文字必须通过 ui_rect_intersect() 求交集，否则会导致 Tile 显存越界崩溃！
 * ========================================================================= */

static void app_template_draw(app_t *self, ui_tile_t *tile) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t overlap;

    // 1. 底板背景涂白 (跳过 32px 顶部系统状态栏)[cite: 1]
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    // 2. 绘制 Tab 容器外壳 (包含标签导航栏)[cite: 1, 3]
    ui_tabview_draw(tile, &data->tabview);

    // 3. 性能门禁：若当前切片显存与内容工作区无交集，立即提前退出 (Early Exit)[cite: 1]
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    char str_buf[48];

    // 4. 按当前激活的 Tab 光栅化控件与 UI
    switch (data->tabview.active_tab_idx) {
        case 0: { // --- 绘制 Tab 0 ---
            // 绘制信息卡片背景（安全裁剪求交）
            if (ui_rect_intersect(&tile->rect, &data->card_info, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->card_info, 0xC618);

                ui_draw_string(tile, 140, 58, "状态监控面板", FONT_16, 0x1A7B, 0xEF7D);

                snprintf(str_buf, sizeof(str_buf), "当前计数值: %lu", data->counter);
                ui_draw_string(tile, 140, 88, str_buf, FONT_20, 0x05E0, 0xEF7D);
            }

            // 绘制原有累加按钮（控件内部已集成切片裁剪求交保护）
            ui_btn_draw(tile, &data->btn_action);

            // 🟢 [示例新增控件 5]：在此调用新增控件的绘制接口
            ui_btn_draw(tile, &data->btn_reset);
            break;
        }

        case 1: { // --- 绘制 Tab 1 ---
            if (ui_rect_intersect(&tile->rect, &data->card_settings, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->card_settings, 0xC618);

                ui_draw_string(tile, 140, 58, "配置选项面板", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 88, "开关控制特性使能", FONT_16, 0x4208, 0xEF7D);
            }

            ui_switch_draw(tile, &data->sw_feature);
            break;
        }
        case 2:{
             if (ui_rect_intersect(&tile->rect, &data->card_settings, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->card_settings, 0xC618);

                ui_draw_string(tile, 140, 58, "测试选项面板", FONT_20, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 140, 88, "测试", FONT_20, 0x4208, 0xEF7D);
            }
        }

        default:
            break;
    }

    // 5. 如果有顶层悬浮元素（如下拉菜单弹窗、浮动输入法全键盘），放在最后画，确保盖在最上层[cite: 1]
    // if (data->kb.is_open) { ui_keyboard_draw(tile, &data->kb); }
}

/* =========================================================================
 * 7. 导出单例实体 (注册到系统)
 * ========================================================================= */

app_t *app_template_get_instance(void) {
    s_app_template.id            = APP_ID_CUSTOM_0; // ⚡ 改为你在 app_manager.h 中新增的 App ID
    s_app_template.name          = "新模板";        // 桌面 App 图标下方显示的名称
    s_app_template.init          = app_template_init;
    s_app_template.enter         = app_template_enter;
    s_app_template.exit          = app_template_exit;
    s_app_template.update        = app_template_update;
    s_app_template.process_touch = app_template_process_touch;
    s_app_template.draw          = app_template_draw;
    return &s_app_template;
}