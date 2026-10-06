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
#include "config.h"
#include "pio_manager.h"
#include "pin_manager.h"
// 硬件驱动层
#include "board_pins.h"
#include "spi_bus.h"
#include "lcd.h"
#include "touch.h"
#include "chinese.h"
#include "fun_esp.h"
   
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
 #include "ui_input.h"
// App 管理与独立应用模块
#include "app_manager.h"
#include "app_led.h"
#include "app_template.h"
#include "app_uart_term.h"
#include "app_swd_probe.h"
#include "app_sd_msc.h"
#include "app_adc.h"
#include "app_pwm.h"
#include "app_i2c_spi.h"
#include "app_pinout.h"
#include "app_audio.h"
#include "app_loader.h"
#include "app_settings.h"
#include "app_uart.h"
#include "app_dap.h"
#include "app_hid_km.h"
#include "app_esp.h"
#include "app_script.h"
#define LED_PIN PICO_DEFAULT_LED_PIN
#define CHIP_TEMP_OFFSET_C 13.0f

// 屏幕物理背景色: 纯白 (0xFFFF)
#define SCREEN_BG_COLOR 0xFFFF

/* ================= 1. 全局控件实体与性能遥测 ================= */
extern "C"
{
    app_manager_t g_app_mgr; // 去掉 static，并用 extern "C" 导出符号
}
// static app_manager_t g_app_mgr;      // 应用管理器核心
static ui_status_bar_t g_status_bar; // 顶部常驻状态栏 (480x32)
static ui_picture_t g_pic_dog;       // 图像组件
static ui_btn_t g_btn_sl;            // 烧录模式更新按键

static uint32_t g_sys_fps_real = 60;    // 状态栏实时显示 FPS
static uint32_t g_cpu_usage_real = 0;   // 状态栏实时显示 CPU%
static uint8_t g_last_cpu_val = 0xFF;   // 上次状态栏推流 CPU 值
static uint8_t g_last_fps_val = 60;     // 上次状态栏推流 FPS 值
static int16_t g_last_temp_val = -99;   // 上次状态栏推流温度值
static uint64_t g_last_fps_tick_us = 0; // 上一帧物理时标 (用于实时计算 Delta T)
static uint32_t g_fps_smoothed = 60;    // 定点数平滑 FPS
static uint32_t g_cpu_ema_x10 = 0;      // 定点数平滑 CPU (0~1000 对应 0~100.0%)

static bool g_last_touch_val = false; //
static bool g_w25_ok = false;         //

/* ================= 2. 硬件传感器与交互回调 ================= */
 
// 片内 ADC 温度传感器初始化
static void chip_temp_init(void)
{
    adc_init();                        //
    adc_set_temp_sensor_enabled(true); //
}

// 高精度平滑采样的片内温度读取
static int16_t chip_temp_read(void)
{
    adc_select_input(4);

    // 1. 预采样并丢弃首帧，让采样电容充分稳定
    (void)adc_read();
    sleep_us(10);

    // 2. 16 次过采样均值滤波
    uint32_t raw_sum = 0;
    for (int i = 0; i < 16; i++)
    {
        raw_sum += adc_read();
        sleep_us(5);
    }
    float raw_avg = (float)raw_sum / 16.0f;

    // 3. 实际 3V3 轨基准换算
    const float vref = 3.28f;
    float voltage = raw_avg * (vref / 4096.0f);

    // 4. 官方温感公式计算
    float temp_c = 27.0f - (voltage - 0.706f) / 0.001721f+CHIP_TEMP_OFFSET_C;

    return (int16_t)(temp_c + 0.5f);
}

// 状态栏点击交互：点击标题区域呼出/关闭抽屉菜单
static void on_status_bar_click(ui_status_bar_t *sb, ui_sb_hit_t hit_area, void *user_data)
{
    (void)sb;
    (void)user_data;
    if (hit_area == UI_SB_HIT_TITLE)
    {
        app_manager_toggle_drawer(&g_app_mgr);
    }
}

// 按键点击直接进入 BOOTSEL 模式
static void on_btn_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    (void)user_data;
    reset_usb_boot(1u << LED_PIN, 0);
}

/* ================= 3. UI 布局与应用注册初始化 ================= */

void app_ui_custom_init(void)
{
    // 1. 初始化 App 管理器 (抽屉宽 140px，Y:32 下方展开)
    app_manager_init(&g_app_mgr);

    // 2. 注册已开发的独立 App (按顺序在抽屉菜单中排列)
    // app_manager_register(&g_app_mgr, app_led_get_instance());
    // app_manager_register(&g_app_mgr, app_template_get_instance());
     app_manager_register(&g_app_mgr, app_settings_get_instance());

    // app_manager_register(&g_app_mgr, app_uart_term_get_instance());
    // app_manager_register(&g_app_mgr, app_swd_probe_get_instance());
    app_manager_register(&g_app_mgr, app_sd_msc_get_instance());

    // app_manager_register(&g_app_mgr, app_adc_get_instance());

    // app_manager_register(&g_app_mgr, app_pwm_get_instance());
    // app_manager_register(&g_app_mgr, app_i2c_spi_get_instance());
    // app_manager_register(&g_app_mgr, app_pinout_get_instance());
    // app_manager_register(&g_app_mgr, app_audio_get_instance());
    // app_manager_register(&g_app_mgr, app_loader_get_instance());
    // 3. 初始化顶部状态栏 (X:0, Y:0, W:480, H:32)

  
  
    app_manager_register(&g_app_mgr, app_script_get_instance());
    app_manager_register(&g_app_mgr, app_esp_get_instance());
    app_manager_register(&g_app_mgr, app_uart_get_instance());
    app_manager_register(&g_app_mgr, app_dap_get_instance());
    app_manager_register(&g_app_mgr, app_hid_km_get_instance());
    ui_status_bar_init(&g_status_bar, 0, 0, LCD_WIDTH, 32, "多功能嵌入式终端", on_status_bar_click);

    // 4. 初始化更新按键 (X:54, Y:280, W:75, H:36)
    ui_btn_init(&g_btn_sl, 54, 280, 75, 36, "更新", on_btn_click);
    g_btn_sl.style = UI_BTN_STYLE_ROUND;
    g_btn_sl.mode = UI_BTN_MODE_PUSH;
    g_btn_sl.font_size = FONT_20;
    ui_btn_set_color(&g_btn_sl, 0xBF5E, 0xC618, 0x1A7B);

    // 5. 初始化图像/动图 (X:230, Y:60)
    int16_t img_x = 230;
    int16_t img_y = 60;
#if DOG_IS_ANIM
    ui_picture_init_anim(&g_pic_dog, img_x, img_y, DOG_IMG_W, DOG_IMG_H,
                         g_dog_frames, DOG_IMG_FRAMES, 60, false, 0, NULL); //
#else
    ui_picture_init_static(&g_pic_dog, img_x, img_y, DOG_IMG_W, DOG_IMG_H,
                           g_dog_frame_0, false, 0, NULL); //
#endif
}
  
/* ================= 4. 全局场景切片光栅化合成 ================= */
static void ui_update_fps_ema(void)
{
    uint64_t now_us = time_us_64();
    if (g_last_fps_tick_us != 0)
    {
        uint32_t dt_us = (uint32_t)(now_us - g_last_fps_tick_us);
        if (dt_us > 0 && dt_us < 1000000)
        { // 过滤过大卡顿跳变
            uint32_t inst_fps = 1000000U / dt_us;
            if (inst_fps > 60)
                inst_fps = 60; // 物理屏幕刷新上限限制为 60
            g_fps_smoothed = (g_fps_smoothed * 8 + inst_fps * 2) / 10;
            g_sys_fps_real = g_fps_smoothed;
        }
    }
    g_last_fps_tick_us = now_us;
}

static void render_custom_scene(ui_tile_t *tile)
{
    // ---------------- 图层 1: 主桌面或当前 App ----------------
    if (g_app_mgr.is_home)
    {
        // 1. 纯白底色填充
        ui_draw_fill_rect(tile, &tile->rect, SCREEN_BG_COLOR);

        // 2. 绘制图像与按键
        ui_picture_draw(tile, &g_pic_dog);
        ui_btn_draw(tile, &g_btn_sl);

        // 3. 欢迎标题 (Y: 40)
        int16_t text_y = 40; //
        if ((text_y + 24) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
        {
            ui_draw_string(tile, 30, text_y, "欢迎回来,轩", FONT_24, 0x0000, SCREEN_BG_COLOR);
        }

        // 4. 装饰分割线 (Y: 75)
        ui_rect_t div_rect = ui_rect_make(30, 75, 180, 1);
        ui_rect_t div_ov; //
        if (ui_rect_intersect(&tile->rect, &div_rect, &div_ov))
        {
            ui_draw_fill_rect(tile, &div_ov, 0xC618);
        }

        // 5. W25 Flash 挂载状态 (Y: 88)
        text_y = 88;
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
        {
            ui_draw_string(tile, 30, text_y, "存储:", FONT_20, 0x4208, SCREEN_BG_COLOR);
            if (g_w25_ok)
            {
                ui_draw_string(tile, 75, text_y, " W25Q64 [已挂载]", FONT_16, 0x05E0, SCREEN_BG_COLOR);
            }
            else
            {
                ui_draw_string(tile, 75, text_y, "Flash [未连接]", FONT_16, 0xF800, SCREEN_BG_COLOR);
            }
        }

        // 6. 触控芯片状态 (Y: 114)
        text_y = 114; //
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
        {
            ui_draw_string(tile, 30, text_y, "触控:", FONT_20, 0x4208, SCREEN_BG_COLOR);
            ui_draw_string(tile, 75, text_y, " FT6336 [就绪]", FONT_16, 0x05E0, SCREEN_BG_COLOR);
        }

        // 7. 系统主频 (Y: 140)
        text_y = 140;
        if ((text_y + 16) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
        {
            ui_draw_string(tile, 30, text_y, "主频:", FONT_20, 0x4208, SCREEN_BG_COLOR);
            ui_draw_string(tile, 75, text_y, " 240MHz/60M", FONT_16, 0x03DF, SCREEN_BG_COLOR);
        }

        // 8. 更新模式提示文本 (Y: 256)
        text_y = 256;
        if ((text_y + 20) > tile->rect.y && text_y < (tile->rect.y + tile->rect.h))
        {
            ui_draw_string(tile, 30, text_y, "程序更新模式", FONT_20, 0x0000, SCREEN_BG_COLOR);
        }
    }
    else
    {
        // App 专属画面：当前运行的 App 独立切片绘制
        app_t *cur = app_manager_get_current(&g_app_mgr);
        if (cur && cur->draw)
        {
            cur->draw(cur, tile);
        }
    }

    // ---------------- 图层 2: 抽屉浮层 (展开时覆盖绘制) ----------------
    if (g_app_mgr.drawer.is_open)
    {
        ui_drawer_draw(tile, &g_app_mgr.drawer);
    }

    // ---------------- 图层 3: 顶部常驻状态栏 (最顶层保护) ----------------
    ui_status_bar_draw(tile, &g_status_bar);
}
static void sys_process_keyboard_line(const char *line)
{
    if (strncmp(line, "#KB:D,", 6) != 0) return;

    int ascii = 0;
    unsigned int hid_code = 0, mod = 0;
    if (sscanf(line, "#KB:D,%d,0x%x,0x%x", &ascii, &hid_code, &mod) >= 2) {
        // 尝试注入当前焦点输入框
        if (ui_input_inject_key(ascii, (uint8_t)hid_code)) {
            if (g_focused_input) {
                LCD_WaitDMA();
                // ? 关键修复：如果是按了 Enter 回车，刷新整个 App 内容区（终端+输入框）；普通打字只刷输入框
                if (ascii == '\n' || hid_code == 0x28) {
                    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
                    ui_render_dirty_clipped(&body, render_custom_scene);
                } else {
                    ui_rect_t r = g_focused_input->rect;
                    ui_render_dirty_clipped(&r, render_custom_scene);
                }
            }
            return;
        }
    }
}
/* ================= 5. 裸机主入口与事件循环 ================= */
int main(void)
{
    
    // 1. RP2040 超频至 240MHz
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(5); //
    set_sys_clock_khz(240000, true);
    sleep_ms(50); //等待自动更新的枚举
    stdio_init_all();
    pio_manager_system_init();
    pin_manager_system_init();
    if(fun_esp_init())esp_send_string("\r\n");
    
    
//    esp_send_string("#PWM:10\r\n");
//    esp_send_string("#PWM:10\r\n");
//    esp_send_string("#PWM:10\r\n");

    // 2. 心跳指示灯初始化
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);
   
    // 3. SPI 片选保护锁定
    gpio_init(PIN_W25_CS);
    gpio_set_dir(PIN_W25_CS, GPIO_OUT);
    gpio_put(PIN_W25_CS, 1);
    gpio_init(PIN_LCD_CS);
    gpio_set_dir(PIN_LCD_CS, GPIO_OUT);
    gpio_put(PIN_LCD_CS, 1);

    // 4. 屏幕、触控与显存初始化
    LCD_Init();
    LCD_Clear(WHITE);
    LCD_WaitDMA();

    g_w25_ok = Chinese_Init();
    Touch_Init();
    ui_tile_init();
    chip_temp_init();

    // 5. 初始化 UI 布局与应用注册
    app_ui_custom_init();

    int16_t init_temp = chip_temp_read();
    ui_status_bar_set_telemetry(&g_status_bar, 0, init_temp, 60);

    // 6. 首帧全屏渲染基底
    ui_render_frame(render_custom_scene);

    uint32_t last_led_tick = 0;
    uint32_t last_sec_tick = 0;

    Touch_Data_t tp;
    int16_t last_tx = 0, last_ty = 0;
esp_send_string("#PWM:204\r\n"); //80%亮度
// esp_send_string("#PWM:10\r\n");
    // 7. 纯局部切片剪裁事件驱动主循环
    while (1)
    {
        uint64_t loop_start_us = time_us_64();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        char kb_line[48];
        while (esp_pop_keyboard_line(kb_line, sizeof(kb_line))) {
            sys_process_keyboard_line(kb_line);
        }
        // ================= A. 秒级状态栏遥测推流 =================
        if (now - last_sec_tick >= 1000)
        {
            last_sec_tick = now; //
            int16_t cur_temp = chip_temp_read();
            
            if ((uint8_t)g_cpu_usage_real != g_last_cpu_val ||
                (uint8_t)g_sys_fps_real != g_last_fps_val ||
                cur_temp != g_last_temp_val)
            { //
                g_last_cpu_val = (uint8_t)g_cpu_usage_real;
                g_last_fps_val = (uint8_t)g_sys_fps_real;
                g_last_temp_val = cur_temp;

                ui_status_bar_set_telemetry(&g_status_bar, g_last_cpu_val, cur_temp, g_last_fps_val);

                LCD_WaitDMA(); //
                ui_rect_t telem_dirty = ui_status_bar_get_telemetry_rect(&g_status_bar);
                ui_render_dirty_clipped(&telem_dirty, render_custom_scene);
            }

            // 剔除状态栏自身重绘时间，避免污染业务 CPU 统计
            loop_start_us = time_us_64();
        }

        // B. App 后台轮询更新
        app_manager_update(&g_app_mgr, now);

        // C. 心跳指示灯 (500ms 翻转)
        if (sys_led_blink&& now - last_led_tick >= 500)
        {
            last_led_tick = now; //
            gpio_xor_mask(1u << LED_PIN);
        }

        // D. 硬件触控采集
        bool read_ok = Touch_ReadData(&tp);
        bool is_pressing = read_ok && (tp.touch_num > 0);
        int16_t tx = is_pressing ? (int16_t)tp.x0 : last_tx;
        int16_t ty = is_pressing ? (int16_t)tp.y0 : last_ty;
        if (is_pressing)
        {
            last_tx = tx;
            last_ty = ty;
        }

        // E. 状态栏 Touch 靶心图标微矩形推流 (仅 20x22px)
        if (is_pressing != g_last_touch_val)
        {
            g_last_touch_val = is_pressing;
            ui_status_bar_set_touch(&g_status_bar, is_pressing);

            LCD_WaitDMA();
            ui_rect_t touch_dirty = ui_status_bar_get_touch_icon_rect(&g_status_bar);
            ui_render_dirty_clipped(&touch_dirty, render_custom_scene);
        }

        // ================= F. 局部脏矩形捕获分发 =================
        ui_rect_t dirty_box = ui_rect_make(0, 0, 0, 0);
        bool is_full_frame = false;

        // 1. 送入 AppManager (处理边缘右滑出抽屉 / 边缘左滑回主屏 / 运行中 App 局部触控)
        (void)app_manager_process_touch_dirty(
            &g_app_mgr, is_pressing, tx, ty, now, &dirty_box, &is_full_frame);

        // 2. 状态栏触控 (抽屉未打开时点击标题栏展开)
        if (!g_app_mgr.drawer.is_open)
        {
            if (ui_status_bar_process_touch(&g_status_bar, is_pressing, tx, ty, now))
            {
                ui_rect_union(&dirty_box, &g_app_mgr.drawer.rect, &dirty_box);
            }

            // 3. 主桌面专属控件 (更新按钮 + 小狗动图)
            if (g_app_mgr.is_home)
            {
                if (ui_btn_process_touch(&g_btn_sl, is_pressing, tx, ty, now))
                {
                    ui_rect_t btn_dirty = ui_btn_get_dirty_rect(&g_btn_sl);
                    ui_rect_union(&dirty_box, &btn_dirty, &dirty_box);
                }
                ui_rect_t pic_dirty;
                if (ui_picture_update(&g_pic_dog, now, &pic_dirty))
                {
                    ui_rect_union(&dirty_box, &pic_dirty, &dirty_box);
                }
            }
        }

        if (is_full_frame || g_app_mgr.needs_full_redraw)
        {
            g_app_mgr.needs_full_redraw = false;
            LCD_WaitDMA();
            ui_render_frame(render_custom_scene);
        }
        else if (dirty_box.w > 0 && dirty_box.h > 0)
        {
            LCD_WaitDMA(); //
            ui_render_dirty_clipped(&dirty_box, render_custom_scene);
        }

        // ================= H. 物理 FPS 实时测量 =================
        ui_update_fps_ema();

        // 动态帧时间预算：交互/动画时 60 FPS (16.6ms)，静止待机时 20 FPS (50ms)
        uint64_t loop_busy_us = time_us_64() - loop_start_us;
        uint32_t frame_budget_us = (is_pressing || dirty_box.w > 0 || is_full_frame || (g_app_mgr.is_home && g_pic_dog.is_playing)) ? 16666 : 50000; //

        // ================= I. 纯定点整数 CPU EMA 计算 (零浮点开销) =================
        uint32_t inst_busy_x10 = (loop_busy_us >= frame_budget_us)
                                     ? 1000
                                     : (uint32_t)(loop_busy_us * 1000ULL / frame_budget_us);

        g_cpu_ema_x10 = (g_cpu_ema_x10 * 9 + inst_busy_x10) / 10;
        g_cpu_usage_real = (g_cpu_ema_x10 + 5) / 10; // 四舍五入为 0~100%
        if (g_cpu_usage_real > 100)
            g_cpu_usage_real = 100;

        // 帧时钟睡眠补偿
        if (loop_busy_us < frame_budget_us)
        { //
            sleep_us((uint32_t)(frame_budget_us - loop_busy_us));
        }
    }

    return 0;
}