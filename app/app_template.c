#include "app_template.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "lcd.h"
#include <stdio.h>

/* ================= 1. 组件与外设交互回调 ================= */

static void on_hello_btn_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    (void)user_data;
    // ⚡ 业务触发：串口打印、外设引脚翻转或修改业务变量
    printf("hello\r\n");
}

/* ================= 2. App 私有数据结构体 ================= */

typedef struct {
    ui_rect_t dirty_rect; // ⚠️ 必须保持在第 1 位！供 app_manager 提取局部脏矩形
    ui_btn_t  btn_hello;  // 业务控件实体
} app_template_data_t;

static app_template_data_t s_template_data;
static app_t               s_app_instance;

/* ================= 3. 生命周期与事件回调 ================= */

static void app_template_init(app_t *self) {
    // 绑定私有数据上下文
    self->user_data = &s_template_data;
    app_template_data_t *data = &s_template_data;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // 控件初始化 (X: 190, Y: 130, 宽: 75, 高: 36)
    ui_btn_init(&data->btn_hello, 190, 130, 75, 36, "你好", on_hello_btn_click);
    data->btn_hello.style     = UI_BTN_STYLE_ROUND;
    data->btn_hello.mode      = UI_BTN_MODE_PUSH;
    data->btn_hello.font_size = FONT_20;
    ui_btn_set_color(&data->btn_hello, 0xBF5E, 0xC618, 0x1A7B);
    data->btn_hello.user_data = data;
}

static void app_template_enter(app_t *self) {
    (void)self;
    // ⚡ 每次从抽屉进入该 App 时触发：开启传感器采样、启动外设定时器或分配临时显存
}

static void app_template_exit(app_t *self) {
    (void)self;
    // ⚡ 每次滑出退出该 App 时触发：关闭外设中断、释放内存或进入低功耗
}

static void app_template_update(app_t *self, uint32_t now_tick) {
    (void)self;
    (void)now_tick;
    // ⚡ 16.6ms 定时后台轮询逻辑（如无后台任务保持空实现即可）
}

static bool app_template_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;

    // ⚡ 触控分发：组件状态改变时提取局部脏矩形，触发切片微推流
    if (ui_btn_process_touch(&data->btn_hello, is_touching, tx, ty, now_tick)) {
        data->dirty_rect = ui_btn_get_dirty_rect(&data->btn_hello);
        return true;
    }
    return false;
}

/* ================= 4. 切片光栅化渲染 ================= */

static void app_template_draw(app_t *self, ui_tile_t *tile) {
    app_template_data_t *data = (app_template_data_t *)self->user_data;
    ui_rect_t overlap;

    // 1. 工作区纯白底色 (Y: 32 ~ 320，避开顶部 32px 状态栏)
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    // 2. 绘制业务组件（内部自动与当前 32px 切片做相交裁剪）
    ui_btn_draw(tile, &data->btn_hello);
}

/* ================= 5. 导出单例实体 ================= */

app_t *app_template_get_instance(void) {
    s_app_instance.id            = APP_ID_SETTINGS;   // 关联 app_base.h 中的枚举 ID
    s_app_instance.name          = "空白模板";         // 抽屉列表中显示的名称
    s_app_instance.init          = app_template_init;
    s_app_instance.enter         = app_template_enter;
    s_app_instance.exit          = app_template_exit;
    s_app_instance.update        = app_template_update;
    s_app_instance.process_touch = app_template_process_touch;
    s_app_instance.draw          = app_template_draw;
    return &s_app_instance;
}