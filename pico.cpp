#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "hardware/adc.h"

// 硬件驱动层
#include "board_pins.h"
#include "spi_bus.h"
#include "lcd.h"
#include "touch.h"
#include "chinese.h"

// UI 核心渲染套件
#include "ui_geometry.h"
#include "ui_tile.h"
#include "ui_core.h"
#include "ui_draw.h"

// UI 组件与图像数据
#include "ui_status_bar.h"
#include "ui_picture.h"
#include "ui_btn.h"
#include "dog_data.h"

// ? App 管理与独立应用模块
#include "app_manager.h"
#include "app_led.h"
#include "app_template.h"

#define LED_PIN PICO_DEFAULT_LED_PIN

// 屏幕物理背景色: 纯白 (0xFFFF)
#define SCREEN_BG_COLOR 0xFFFF

/* ================= 1. 全局控件实体与性能遥测 ================= */

static app_manager_t   g_app_mgr;       // 应用管理器核心//[cite: 15]
static ui_status_bar_t g_status_bar;   // 顶部常驻状态栏 (480x32)//[cite: 16]
static ui_picture_t    g_pic_dog;      // 图像组件//[cite: 16]
static ui_btn_t        g_btn_sl;       // 烧录模式更新按键//[cite: 16]

static uint32_t        g_sys_fps_real   = 60;//[cite: 16]
static uint32_t        g_cpu_usage_real = 0;//[cite: 16]
static uint8_t         g_last_cpu_val   = 0xFF;//[cite: 16]
static uint8_t         g_last_fps_val   = 0xFF;//[cite: 16]
static int16_t         g_last_temp_val  = -99;//[cite: 16]
static bool            g_last_touch_val = false;//[cite: 16]
static bool            g_w25_ok         = false;//[cite: 16]

/* ================= 2. 硬件传感器与交互回调 ================= */

// 片内 ADC 温度传感器初始化//[cite: 16]
static void chip_temp_init(void) {
    adc_init();//[cite: 16]
    adc_set_temp_sensor_enabled(true);//[cite: 16]
}

// 高精度平滑采样的片内温度读取//[cite: 16]
static int16_t chip_temp_read(void) {
    adc_select_input(4);//[cite: 16]

    // 1. 预采样并丢弃首帧，让采样电容充分稳定//[cite: 16]
    (void)adc_read();//[cite: 16]
    sleep_us(10);//[cite: 16]

    // 2. 16 次过采样均值滤波//[cite: 16]
    uint32_t raw_sum = 0;//[cite: 16]
    for (int i = 0; i < 16; i++) {
        raw_sum += adc_read();//[cite: 16]
        sleep_us(5);//[cite: 16]
    }
    float raw_avg = (float)raw_sum / 16.0f;//[cite: 16]

    // 3. 实际 3V3 轨基准换算//[cite: 16]
    const float vref = 3.28f;//[cite: 16]
    float voltage = raw_avg * (vref / 4096.0f);//[cite: 16]

    // 4. 官方温感公式计算//[cite: 16]
    float temp_c = 27.0f - (voltage - 0.706f) / 0.001721f;//[cite: 16]

    return (int16_t)(temp_c + 0.5f);//[cite: 16]
}

// 状态栏点击交互：点击标题区域呼出/关闭抽屉菜单//[cite: 14, 15, 16]
static void on_status_bar_click(ui_status_bar_t *sb, ui_sb_hit_t hit_area, void *user_data) {
    (void)sb; (void)user_data;//[cite: 16]
    if (hit_area == UI_SB_HIT_TITLE) {
        app_manager_toggle_drawer(&g_app_mgr);//[cite: 14, 15]
    }
}

// 按键点击直接进入 BOOTSEL 模式//[cite: 16]
static void on_btn_click(ui_btn_t *btn, void *user_data) {
    (void)btn; (void)user_data;//[cite: 16]
    LCD_WaitDMA();//[cite: 16]
    reset_usb_boot(1u << LED_PIN, 0);//[cite: 16]
}

/* ================= 3. UI 布局与应用注册初始化 ================= */

void app_ui_custom_init(void) {
    // 1. 初始化 App 管理器 (抽屉宽 140px，Y:32 下方展开)//[cite: 14, 15]
    app_manager_init(&g_app_mgr);//[cite: 14, 15]

    // 2. 注册已开发的独立 App (后续新增 App 直接在此处注册)//[cite: 14, 15]
    app_manager_register(&g_app_mgr, app_led_get_instance());//[cite: 13, 14, 15]
    app_manager_register(&g_app_mgr, app_template_get_instance());

    // 3. 初始化顶部状态栏 (X:0, Y:0, W:480, H:32)//[cite: 16]
    ui_status_bar_init(&g_status_bar, 0, 0, LCD_WIDTH, 32, "多功能嵌入式终端", on_status_bar_click);//[cite: 16]

    // 4. 初始化更新按键 (X:54, Y:280, W:75, H:36)//[cite: 16]
    ui_btn_init(&g_btn_sl, 54, 280, 75, 36, "更新", on_btn_click);//[cite: 16]
    g_btn_sl.style = UI_BTN_STYLE_ROUND;//[cite: 16]
    g_btn_sl.mode = UI_BTN_MODE_PUSH;//[cite: 16]
    g_btn_sl.font_size = FONT_20;//[cite: 16]
    ui_btn_set_color(&g_btn_sl, 0xBF5E, 0xC618, 0x1A7B);//[cite: 16]

    // 5. 初始化图像/动图 (X:230, Y:60)//[cite: 16]
    int16_t img_x = 230;//[cite: 16]
    int16_t img_y = 60;//[cite: 16]
#if DOG_IS_ANIM
    ui_picture_init_anim(&g_pic_dog, img_x, img_y, DOG_IMG_W, DOG_IMG_H,
                         g_dog_frames, DOG_IMG_FRAMES, 60, false, 0, NULL);//[cite: 16]
#else
    ui_picture_init_static(&g_pic_dog, img_x, img_y, DOG_IMG_W, DOG_IMG_H,
                           g_dog_frame_0, false, 0, NULL);//[cite: 16]
#endif
}

/* ================= 4. 全局场景切片光栅化合成 ================= */

static void render_custom_scene(ui_tile_t *tile) {
    // ---------------- 图层 1: 主桌面或当前 App ----------------
    if (g_app_mgr.is_home) {//[cite: 14, 15]
        // 1. 纯白底色填充//[cite: 16]
        ui_draw_fill_rect(tile, &tile->rect, SCREEN_BG_COLOR);//[cite: 16]

        // 2. 绘制图像与按键//[cite: 16]
        ui_picture_draw(tile, &g_pic_dog);//[cite: 16]
        ui_btn_draw(tile, &g_btn_sl);//[cite: 16]

        // 3. 欢迎标题 (Y: 40)//[cite: 16]
        int16_t text_y = 40;//[cite: 16]
        if ((text_y + 24) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {//[cite: 16]
            ui_draw_string(tile, 30, text_y, "欢迎回来,轩", FONT_24, 0x0000, SCREEN_BG_COLOR);//[cite: 16]
        }

        // 4. 装饰分割线 (Y: 75)//[cite: 16]
        ui_rect_t div_rect = ui_rect_make(30, 75, 180, 1);//[cite: 16]
        ui_rect_t div_ov;//[cite: 16]
        if (ui_rect_intersect(&tile->rect, &div_rect, &div_ov)) {//[cite: 16]
            ui_draw_fill_rect(tile, &div_ov, 0xC618);//[cite: 16]
        }

        // 5. W25 Flash 挂载状态 (Y: 88)//[cite: 16]
        text_y = 88;//[cite: 16]
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {//[cite: 16]
            ui_draw_string(tile, 30, text_y, "存储:", FONT_20, 0x4208, SCREEN_BG_COLOR);//[cite: 16]
            if (g_w25_ok) {//[cite: 16]
                ui_draw_string(tile, 75, text_y, " W25Q64 [已挂载]", FONT_16, 0x05E0, SCREEN_BG_COLOR);//[cite: 16]
            } else {
                ui_draw_string(tile, 75, text_y, "Flash [未连接]", FONT_16, 0xF800, SCREEN_BG_COLOR);//[cite: 16]
            }
        }

        // 6. 触控芯片状态 (Y: 114)//[cite: 16]
        text_y = 114;//[cite: 16]
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {//[cite: 16]
            ui_draw_string(tile, 30, text_y, "触控:", FONT_20, 0x4208, SCREEN_BG_COLOR);//[cite: 16]
            ui_draw_string(tile, 75, text_y, " FT6336 [就绪]", FONT_16, 0x05E0, SCREEN_BG_COLOR);//[cite: 16]
        }

        // 7. 系统主频 (Y: 140)//[cite: 16]
        text_y = 140;//[cite: 16]
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {//[cite: 16]
            ui_draw_string(tile, 30, text_y, "主频:", FONT_20, 0x4208, SCREEN_BG_COLOR);//[cite: 16]
            ui_draw_string(tile, 75, text_y, " 240MHz/60M", FONT_16, 0x03DF, SCREEN_BG_COLOR);//[cite: 16]
        }

        // 8. 更新模式提示文本 (Y: 256)//[cite: 16]
        text_y = 256;//[cite: 16]
        if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h)) {//[cite: 16]
            ui_draw_string(tile, 30, text_y, "程序更新模式", FONT_20, 0x0000, SCREEN_BG_COLOR);//[cite: 16]
        }
    } else {
        // App 专属画面：当前运行的 App 独立切片绘制//[cite: 11, 14, 15]
        app_t *cur = app_manager_get_current(&g_app_mgr);//[cite: 14, 15]
        if (cur && cur->draw) {//[cite: 11, 14]
            cur->draw(cur, tile);//[cite: 11, 12, 14]
        }
    }

    // ---------------- 图层 2: 抽屉浮层 (展开时覆盖绘制) ----------------
    if (g_app_mgr.drawer.is_open) {//[cite: 9, 10, 14, 15]
        ui_drawer_draw(tile, &g_app_mgr.drawer);//[cite: 9, 10, 14]
    }

    // ---------------- 图层 3: 顶部常驻状态栏 (最顶层保护) ----------------
    ui_status_bar_draw(tile, &g_status_bar);//[cite: 16]
}

/* ================= 5. 裸机主入口与事件循环 ================= */

int main(void) {
    // 1. RP2040 超频至 240MHz//[cite: 16]
    vreg_set_voltage(VREG_VOLTAGE_1_15);//[cite: 16]
    sleep_ms(5);//[cite: 16]
    set_sys_clock_khz(240000, true);//[cite: 16]

    stdio_init_all();//[cite: 16]

    // 2. 心跳指示灯初始化//[cite: 16]
    gpio_init(LED_PIN);//[cite: 16]
    gpio_set_dir(LED_PIN, GPIO_OUT);//[cite: 16]
    gpio_put(LED_PIN, 1);//[cite: 16]

    // 3. SPI 片选保护锁定//[cite: 16]
    gpio_init(PIN_W25_CS); gpio_set_dir(PIN_W25_CS, GPIO_OUT); gpio_put(PIN_W25_CS, 1);//[cite: 16]
    gpio_init(PIN_LCD_CS); gpio_set_dir(PIN_LCD_CS, GPIO_OUT); gpio_put(PIN_LCD_CS, 1);//[cite: 16]

    // 4. 屏幕、触控与显存初始化//[cite: 16]
    LCD_Init();//[cite: 16]
    LCD_Clear(WHITE);//[cite: 16]
    LCD_WaitDMA();//[cite: 16]

    g_w25_ok = Chinese_Init();//[cite: 16]
    Touch_Init();//[cite: 16]
    ui_tile_init();//[cite: 16]
    chip_temp_init();//[cite: 16]

    // 5. 初始化 UI 布局与应用注册//[cite: 16]
    app_ui_custom_init();

    // 开机预读一次温度并填入状态栏，消除 0℃ 初值//[cite: 16]
    int16_t init_temp = chip_temp_read();//[cite: 16]
    ui_status_bar_set_telemetry(&g_status_bar, 0, init_temp, 60);//[cite: 16]

    // 6. 首帧全屏渲染基底//[cite: 16]
    ui_render_frame(render_custom_scene);//[cite: 16]

    uint32_t last_led_tick    = 0;//[cite: 16]
    uint32_t last_sec_tick    = 0;//[cite: 16]
    uint64_t busy_time_us_sum = 0;//[cite: 16]

    Touch_Data_t tp;//[cite: 16]
    int16_t last_tx = 0, last_ty = 0;//[cite: 16]

    // 7. 纯局部切片剪裁事件驱动主循环//[cite: 16]
    while (1) {
        uint64_t loop_start_us = time_us_64();//[cite: 16]
        uint32_t now = to_ms_since_boot(get_absolute_time());//[cite: 16]

        // A. App 后台轮询更新//[cite: 11, 14, 15]
        app_manager_update(&g_app_mgr, now);//[cite: 14, 15]

        // ================= B. 秒级 CPU 统计与温度采样推流 =================
        if (now - last_sec_tick >= 1000) {//[cite: 16]
            g_cpu_usage_real = (uint32_t)(busy_time_us_sum * 100ULL / 1000000ULL);//[cite: 16]
            if (g_cpu_usage_real > 100) g_cpu_usage_real = 100;//[cite: 16]
            busy_time_us_sum = 0;//[cite: 16]
            last_sec_tick    = now;//[cite: 16]

            int16_t cur_temp = chip_temp_read();//[cite: 16]

            if ((uint8_t)g_cpu_usage_real != g_last_cpu_val || 
                (uint8_t)g_sys_fps_real != g_last_fps_val || 
                cur_temp != g_last_temp_val) 
            {//[cite: 16]
                g_last_cpu_val  = (uint8_t)g_cpu_usage_real;//[cite: 16]
                g_last_fps_val  = (uint8_t)g_sys_fps_real;//[cite: 16]
                g_last_temp_val = cur_temp;//[cite: 16]

                ui_status_bar_set_telemetry(&g_status_bar, g_last_cpu_val, cur_temp, g_last_fps_val);//[cite: 16]

                LCD_WaitDMA();//[cite: 16]
                ui_rect_t telem_dirty = ui_status_bar_get_telemetry_rect(&g_status_bar);//[cite: 16]
                ui_render_dirty_clipped(&telem_dirty, render_custom_scene);//[cite: 16]
            }
        }

        // C. 心跳指示灯 (500ms 翻转)//[cite: 16]
        if (now - last_led_tick >= 500) {//[cite: 16]
            last_led_tick = now;//[cite: 16]
            gpio_xor_mask(1u << LED_PIN);//[cite: 16]
        }

        // D. 硬件触控采集//[cite: 16]
        bool read_ok = Touch_ReadData(&tp);//[cite: 16]
        bool is_pressing = read_ok && (tp.touch_num > 0);//[cite: 16]
        int16_t tx = is_pressing ? (int16_t)tp.x0 : last_tx;//[cite: 16]
        int16_t ty = is_pressing ? (int16_t)tp.y0 : last_ty;//[cite: 16]
        if (is_pressing) {//[cite: 16]
            last_tx = tx;//[cite: 16]
            last_ty = ty;//[cite: 16]
        }

        // E. 状态栏 Touch 靶心图标微矩形推流 (仅 20x22px)//[cite: 16]
        if (is_pressing != g_last_touch_val) {//[cite: 16]
            g_last_touch_val = is_pressing;//[cite: 16]
            ui_status_bar_set_touch(&g_status_bar, is_pressing);//[cite: 16]

            LCD_WaitDMA();//[cite: 16]
            ui_rect_t touch_dirty = ui_status_bar_get_touch_icon_rect(&g_status_bar);//[cite: 16]
            ui_render_dirty_clipped(&touch_dirty, render_custom_scene);//[cite: 16]
        }

        // ================= F. ? 局部脏矩形捕获分发 =================
        ui_rect_t dirty_box = ui_rect_make(0, 0, 0, 0);//[cite: 16]
        bool is_full_frame = false;//[cite: 14]

        // 1. 送入 AppManager (处理边缘右滑出抽屉 / 边缘左滑回主屏 / 运行中 App 局部触控)//[cite: 14]
        (void)app_manager_process_touch_dirty(
            &g_app_mgr, is_pressing, tx, ty, now, &dirty_box, &is_full_frame//[cite: 14, 15]
        );

        // 2. 状态栏触控 (抽屉未打开时点击标题栏展开)//[cite: 14, 16]
        if (!g_app_mgr.drawer.is_open) {//[cite: 10, 14, 15]
            if (ui_status_bar_process_touch(&g_status_bar, is_pressing, tx, ty, now)) {//[cite: 16]
                // 仅刷新抽屉展开区域//[cite: 14]
                ui_rect_union(&dirty_box, &g_app_mgr.drawer.rect, &dirty_box);//[cite: 10, 14, 15, 16]
            }

            // 3. 主桌面专属控件 (更新按钮 + 小狗动图)//[cite: 16]
            if (g_app_mgr.is_home) {//[cite: 14, 15]
                if (ui_btn_process_touch(&g_btn_sl, is_pressing, tx, ty, now)) {//[cite: 16]
                    ui_rect_t btn_dirty = ui_btn_get_dirty_rect(&g_btn_sl);//[cite: 16]
                    ui_rect_union(&dirty_box, &btn_dirty, &dirty_box);//[cite: 16]
                }
                ui_rect_t pic_dirty;//[cite: 16]
                if (ui_picture_update(&g_pic_dog, now, &pic_dirty)) {//[cite: 16]
                    ui_rect_union(&dirty_box, &pic_dirty, &dirty_box);//[cite: 16]
                }
            }
        }

        // ================= G. 局部切片微推流执行 =================
        if (is_full_frame) {//[cite: 14]
            // ? 仅在应用切换（进入 App / 返回桌面）时才执行全屏刷新//[cite: 14]
            LCD_WaitDMA();//[cite: 16]
            ui_render_frame(render_custom_scene);//[cite: 16]
        } 
        else if (dirty_box.w > 0 && dirty_box.h > 0) {//[cite: 16]
            // ? 常规按键、开关、抽屉滑动：仅推流局部脏矩形切片//[cite: 12, 14, 16]
            uint64_t t_render_start = time_us_64();//[cite: 16]
            LCD_WaitDMA();//[cite: 16]
            ui_render_dirty_clipped(&dirty_box, render_custom_scene);//[cite: 16]
            uint64_t render_busy_us = time_us_64() - t_render_start;//[cite: 16]

            if (render_busy_us > 0) {//[cite: 16]
                g_sys_fps_real = (uint32_t)(1000000ULL / render_busy_us);//[cite: 16]
                if (g_sys_fps_real > 999) g_sys_fps_real = 999;//[cite: 16]
            }
        }

        
        uint64_t loop_busy_us = time_us_64() - loop_start_us;//[cite: 16]
        busy_time_us_sum += loop_busy_us;//[cite: 16]

        uint32_t frame_budget_us = (is_pressing || dirty_box.w > 0 || is_full_frame || (g_app_mgr.is_home && g_pic_dog.is_playing)) ? 16666 : 50000;//[cite: 14, 15, 16]
        if (loop_busy_us < frame_budget_us) {//[cite: 16]
            sleep_us((uint32_t)(frame_budget_us - loop_busy_us));//[cite: 16]
        }
    }

    return 0;//[cite: 16]
}