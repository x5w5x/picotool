#include "app_dap.h"
#include "app_shared_ram.h"
#include "hardware/gpio.h"
#include "ui_tabview.h"
#include "ui_scrollview.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_dropdown.h"
#include "ui_pin_config.h"
#include "fun_swd.h"
#include "fun_pio_swd.h"
#include "fun_swd_config.h"
#include "cmsis_dap.h"
#include "tusb.h"
#include "lcd.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static const char *s_dap_tabs[] = { "状态", "配置", "终端" };
#define DAP_TAB_COUNT ((uint8_t)(sizeof(s_dap_tabs) / sizeof(s_dap_tabs[0])))
#define DAP_CONTENT_H 420

typedef struct {
    /* 必须放在第一个字段，app_manager 依赖它获取脏矩形。 */
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab;
    ui_scrollview_t scroll[DAP_TAB_COUNT];
    ui_btn_t probe;
    ui_btn_t pio_probe;
    ui_btn_t apply_pins;
    ui_switch_t dap_switch;
    ui_dropdown_t pio_clock;
    ui_pin_config_t swclk_pin;
    ui_pin_config_t swdio_pin;
    fun_pio_swd_t pio_swd;
    uint8_t pio_ack;
    uint8_t pio_ctrl_ack;
    bool pio_powered;
    bool connected;
    uint32_t dp_id, cpuid, dbg_id, flash_size;
    uint32_t pio_ctrl_stat;
    uint32_t last_dap_transfer_count;
    uint32_t last_dap_fallback_count;
    uint8_t last_dap_ack;
    uint8_t last_dap_stage;
    uint32_t last_dap_command_count;
    uint8_t last_dap_command;
    uint8_t last_dap_command_busy;
} dap_data_t;

static void dap_pin_changed(ui_pin_config_t *config, uint8_t gpio, void *user_data)
{
    (void)config; (void)gpio;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    uint8_t clk = ui_pin_config_get_selected(&data->swclk_pin);
    uint8_t io = ui_pin_config_get_selected(&data->swdio_pin);
    ui_pin_config_set_excluded_mask(&data->swclk_pin, 1u << io);
    ui_pin_config_set_excluded_mask(&data->swdio_pin, 1u << clk);
    if (clk != UI_PIN_CONFIG_INVALID_GPIO && io != UI_PIN_CONFIG_INVALID_GPIO)
        (void)fun_swd_config_set_pins(clk, io);
    data->dirty_rect = data->tabview.content_rect;
}

static void dap_apply_pins_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    uint8_t clk = ui_pin_config_get_selected(&data->swclk_pin);
    uint8_t io = ui_pin_config_get_selected(&data->swdio_pin);
    if (clk == UI_PIN_CONFIG_INVALID_GPIO ||
        io == UI_PIN_CONFIG_INVALID_GPIO || clk == io) return;
    if (fun_swd_config_set_pins(clk, io))
        data->dirty_rect = data->tabview.content_rect;
}

static const char *s_pio_clock_items[] = { "500 kHz", "1 MHz", "2 MHz", "5 MHz", "10 MHz" };
static const uint32_t s_pio_clock_hz[] = { 500000u, 1000000u, 2000000u, 5000000u, 10000000u };

static void dap_pio_clock_changed(ui_dropdown_t *dd, int8_t idx, void *user_data)
{
    (void)dd;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || idx < 0 || idx >= (int8_t)(sizeof(s_pio_clock_hz) / sizeof(s_pio_clock_hz[0]))) return;
    (void)fun_pio_swd_set_clock(&data->pio_swd, s_pio_clock_hz[(uint8_t)idx]);
    data->dirty_rect = data->tabview.content_rect;
}

static app_t s_dap;

static void dap_pio_probe_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    cmsis_dap_set_backend(CMSIS_DAP_BACKEND_PIO);
    data->dp_id = 0;
    data->pio_ctrl_stat = 0;
    data->pio_ack = 0;
    data->pio_ctrl_ack = 0;
    data->pio_powered = false;
    data->cpuid = 0;
    data->dbg_id = 0;
    data->flash_size = 0;
    /*
     * PIO 当前只负责已经验证过的 DP 读取。每次探测前先完全释放
     * 状态机，再用软件 SWD 执行线路复位、清 sticky error 和 DP
     * 上电握手，避免上一次 PIO 事务留下的线路状态影响下一次探测。
     */
    bool opened = data->pio_swd.opened
        ? fun_pio_swd_recover(&data->pio_swd)
        : fun_pio_swd_open_auto(&data->pio_swd,
                                pio_manager_get_default(),
                                fun_swd_config_get()->swclk_gpio,
                                fun_swd_config_get()->swdio_gpio,
                                fun_swd_config_get()->clock_hz);
    data->connected = opened && fun_pio_swd_read_dp_idcode(
        &data->pio_swd, &data->dp_id, &data->pio_ack);
    if (data->connected) {
        data->pio_powered = fun_pio_swd_power_up(&data->pio_swd);
        (void)fun_pio_swd_read_dp_register(
            &data->pio_swd, 0x04u,
            &data->pio_ctrl_stat, &data->pio_ctrl_ack);
        if (data->pio_powered) {
            (void)fun_pio_swd_read_memory32(
                &data->pio_swd, 0xE000ED00u, &data->cpuid);
            (void)fun_pio_swd_read_memory32(
                &data->pio_swd, 0xE0042000u, &data->dbg_id);
            (void)fun_pio_swd_read_memory32(
                &data->pio_swd, 0x1FFFF7E0u, &data->flash_size);
            data->flash_size &= 0xFFFFu;
        }
    }
    /* 当前只验证单笔 PIO DP 读取；不要在一次点击中追加第二笔事务。 */
    data->dirty_rect = data->tabview.content_rect;
}

static void dap_switch_changed(ui_switch_t *sw, bool enabled, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (data->pio_swd.opened) fun_pio_swd_close(&data->pio_swd);
    g_dap_enabled = enabled;
    if (enabled) {
        /* CMSIS-DAP开启时默认使用PIO后端；失败由后端自动回退软件SWD。 */
        cmsis_dap_set_backend(CMSIS_DAP_BACKEND_PIO);
        gpio_init(FUN_SWD_SWCLK_PIN);
        gpio_init(FUN_SWD_SWDIO_PIN);
        gpio_set_dir(FUN_SWD_SWCLK_PIN, GPIO_OUT);
        gpio_set_dir(FUN_SWD_SWDIO_PIN, GPIO_OUT);
        gpio_put(FUN_SWD_SWCLK_PIN, 1);
        gpio_put(FUN_SWD_SWDIO_PIN, 1);
        gpio_pull_up(FUN_SWD_SWDIO_PIN);
        tud_connect();
    } else {
        cmsis_dap_set_backend(CMSIS_DAP_BACKEND_SOFTWARE);
        tud_disconnect();
        gpio_set_dir(FUN_SWD_SWCLK_PIN, GPIO_IN);
        gpio_set_dir(FUN_SWD_SWDIO_PIN, GPIO_IN);
    }
    sw->is_checked = enabled;
    ui_pin_config_set_enabled(&data->swclk_pin, !enabled);
    ui_pin_config_set_enabled(&data->swdio_pin, !enabled);
    data->dirty_rect = data->scroll[1].view_rect;
}

static void dap_probe_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (data->pio_swd.opened) fun_pio_swd_close(&data->pio_swd);
    cmsis_dap_set_backend(CMSIS_DAP_BACKEND_SOFTWARE);
    data->connected = fun_swd_init();
    if (data->connected) {
        data->dp_id = fun_swd_read(0xE000EDF0u);
        data->cpuid = fun_swd_read(0xE000ED00u);
        data->dbg_id = fun_swd_read(0xE0042000u);
        data->flash_size = fun_swd_read(0x1FFFF7E0u) & 0xFFFFu;
    }
    data->dirty_rect = data->tabview.content_rect;
}

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
    ui_draw_string(tile, x, y, s_dap_tabs[data->tabview.active_tab_idx], FONT_20, 0x4208, 0xFFFF);
    if (data->tabview.active_tab_idx == 0) {
        ui_draw_string(tile, x, y + 42, data->connected ? "SWD已连接" : "未探测目标", FONT_16, 0x4208, 0xFFFF);
        char text[48];
        snprintf(text, sizeof(text), "DP ID: %08lX", (unsigned long)data->dp_id);
        ui_draw_string(tile, x, y + 68, text, FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "CPUID: %08lX", (unsigned long)data->cpuid);
        ui_draw_string(tile, x, y + 92, text, FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "DBGMCU: %08lX", (unsigned long)data->dbg_id);
        ui_draw_string(tile, x, y + 116, text, FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "Flash Size: %lu KB", (unsigned long)data->flash_size);
        ui_draw_string(tile, x, y + 140, text, FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "PIO ACK: %u  PIO: %u  Clock: %luHz",
                 data->pio_ack,
                 data->pio_swd.opened ? data->pio_swd.pio_index : 0u,
                 (unsigned long)data->pio_swd.clock_hz);
        ui_draw_string(tile, x, y + 164, text, FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "PIO CTRL: %08lX ACK:%u",
                 (unsigned long)data->pio_ctrl_stat,
                 data->pio_ctrl_ack);
        ui_draw_string(tile, x, y + 188, text, FONT_16, 0x632C, 0xFFFF);
        ui_draw_string(tile, x, y + 212,
                       data->pio_powered ? "PIO POWER: OK" : "PIO POWER: FAIL",
                       FONT_16, 0x632C, 0xFFFF);
        snprintf(text, sizeof(text), "WRITE ACK:%u STAGE:%u",
                 data->pio_swd.last_write_ack,
                 data->pio_swd.power_stage);
        ui_draw_string(tile, x, y + 236, text, FONT_16, 0x632C, 0xFFFF);
    } else if (data->tabview.active_tab_idx == 1) {
        ui_draw_string(tile, x, y + 48, "CMSIS-DAP USB", FONT_20, 0x4208, 0xFFFF);
        ui_draw_string(tile, x, y + 82, g_dap_enabled ? "状态: 已开启" : "状态: 已关闭", FONT_16, 0x632C, 0xFFFF);
        ui_draw_string(tile, x, y + 108,
                       cmsis_dap_get_backend() == CMSIS_DAP_BACKEND_PIO
                           ? "SWD后端: PIO" : "SWD后端: 软件",
                       FONT_16, 0x632C, 0xFFFF);
        char diag[64];
        snprintf(diag, sizeof(diag), "DAP TX:%lu FB:%lu",
                 (unsigned long)g_dap_pio_transfer_count,
                 (unsigned long)g_dap_pio_fallback_count);
        ui_draw_string(tile, x, y + 136, diag, FONT_16, 0x632C, 0xFFFF);
        snprintf(diag, sizeof(diag), "REQ:%02X A:%u S:%u",
                 g_dap_pio_last_request,
                 g_dap_pio_last_ack,
                 g_dap_pio_last_stage);
        ui_draw_string(tile, x, y + 162, diag, FONT_16, 0x632C, 0xFFFF);
        snprintf(diag, sizeof(diag), "CMD:%02X N:%lu BUSY:%u",
                 g_dap_last_command,
                 (unsigned long)g_dap_command_count,
                 g_dap_command_busy);
        ui_draw_string(tile, x, y + 188, diag, FONT_16, 0x632C, 0xFFFF);
        snprintf(diag, sizeof(diag), "DAP CLOCK:%lu Hz",
                 (unsigned long)g_dap_cur_clock_hz);
        ui_draw_string(tile, x, y + 214, diag, FONT_16, 0x632C, 0xFFFF);
        snprintf(diag, sizeof(diag), "PIO:%s CLK:%lu",
                 g_dap_pio_active ? "ACTIVE" : "IDLE",
                 (unsigned long)g_dap_pio_actual_clock_hz);
        ui_draw_string(tile, x, y + 240, diag, FONT_16, 0x632C, 0xFFFF);
        snprintf(diag, sizeof(diag), "PIO R:%lu W:%lu",
                 (unsigned long)g_dap_pio_read_count,
                 (unsigned long)g_dap_pio_write_count);
        ui_draw_string(tile, x, y + 266, diag, FONT_16, 0x632C, 0xFFFF);
    }
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
    ui_btn_init(&data->probe, 140, 224, 260, 34, "软件 SWD 探测", dap_probe_click);
    data->probe.user_data = data;
    data->probe.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->probe, 0xDEFB, 0xBDF7, 0x4208);
    ui_btn_init(&data->pio_probe, 140, 266, 260, 34,
                "PIO 探测 DP ID", dap_pio_probe_click);
    data->pio_probe.user_data = data;
    data->pio_probe.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->pio_probe, 0xDEFB, 0xBDF7, 0x4208);
    fun_pio_swd_init(&data->pio_swd);
    ui_dropdown_init(&data->pio_clock, 140, 182, 260, 34,
                     s_pio_clock_items, 5, 4, dap_pio_clock_changed);
    data->pio_clock.user_data = data;
    ui_dropdown_set_max_visible_items(&data->pio_clock, 5);
    ui_dropdown_set_color(&data->pio_clock, 0xF7BE, 0x7BEF, 0x4208, 0xD6BA);
    ui_switch_init(&data->dap_switch, 350, 102, 64, 32,
                   g_dap_enabled, dap_switch_changed);
    data->dap_switch.user_data = data;
    ui_btn_init(&data->apply_pins, 140, 254, 280, 34,
                "应用引脚配置", dap_apply_pins_click);
    data->apply_pins.user_data = data;
    data->apply_pins.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->apply_pins, 0xDEFB, 0xBDF7, 0x4208);
    const fun_swd_config_t *pins = fun_swd_config_get();
    ui_pin_config_init(&data->swclk_pin, 140, 150, 280, "SWCLK Pin",
                       pin_manager_get_default(), PIN_OWNER_SWD,
                       PIN_SOURCE_PIO, PIN_FUNC_PIO, 0u,
                       pins->swclk_gpio, dap_pin_changed);
    data->swclk_pin.user_data = data;
    ui_pin_config_init(&data->swdio_pin, 140, 202, 280, "SWDIO Pin",
                       pin_manager_get_default(), PIN_OWNER_SWD,
                       PIN_SOURCE_PIO, PIN_FUNC_PIO, 0u,
                       pins->swdio_gpio, dap_pin_changed);
    data->swdio_pin.user_data = data;
    ui_pin_config_set_excluded_mask(&data->swclk_pin, 1u << pins->swdio_gpio);
    ui_pin_config_set_excluded_mask(&data->swdio_pin, 1u << pins->swclk_gpio);
    ui_pin_config_set_enabled(&data->swclk_pin, !g_dap_enabled);
    ui_pin_config_set_enabled(&data->swdio_pin, !g_dap_enabled);
}

static void dap_exit(app_t *self)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (data && data->pio_swd.opened) fun_pio_swd_close(&data->pio_swd);
    self->user_data = NULL;
}
static void dap_update(app_t *self, uint32_t now_tick)
{
    (void)now_tick;
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return;
    if (data->last_dap_transfer_count != g_dap_pio_transfer_count ||
        data->last_dap_fallback_count != g_dap_pio_fallback_count ||
        data->last_dap_ack != g_dap_pio_last_ack ||
        data->last_dap_stage != g_dap_pio_last_stage ||
        data->last_dap_command_count != g_dap_command_count ||
        data->last_dap_command != g_dap_last_command ||
        data->last_dap_command_busy != g_dap_command_busy) {
        data->last_dap_transfer_count = g_dap_pio_transfer_count;
        data->last_dap_fallback_count = g_dap_pio_fallback_count;
        data->last_dap_ack = g_dap_pio_last_ack;
        data->last_dap_stage = g_dap_pio_last_stage;
        data->last_dap_command_count = g_dap_command_count;
        data->last_dap_command = g_dap_last_command;
        data->last_dap_command_busy = g_dap_command_busy;
        if (data->tabview.active_tab_idx == 0u)
            data->dirty_rect = data->scroll[0].view_rect;
    }
}

static bool dap_process_touch(app_t *self, bool touching, int16_t x,
                              int16_t y, uint32_t tick)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return false;
    if (data->swclk_pin.modal.is_open)
        return ui_pin_config_process_modal_touch(&data->swclk_pin,
                                                  touching, x, y, tick);
    if (data->swdio_pin.modal.is_open)
        return ui_pin_config_process_modal_touch(&data->swdio_pin,
                                                  touching, x, y, tick);
    if (ui_tabview_process_touch(&data->tabview, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.rect;
        return true;
    }
    uint8_t tab = data->tabview.active_tab_idx;
    if (tab == 0 && ui_dropdown_process_touch(&data->pio_clock, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.content_rect;
        return true;
    }
    if (ui_scrollview_process_touch(&data->scroll[tab], touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect,
                                          data->scroll[tab].view_rect);
        return true;
    }
    if (tab == 0 && ui_btn_process_touch(&data->probe, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.content_rect;
        return true;
    }
    if (tab == 0 && ui_btn_process_touch(&data->pio_probe, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.content_rect;
        return true;
    }
    if (tab == 1 && ui_switch_process_touch(&data->dap_switch, touching, x, y, tick)) {
        data->dirty_rect = data->scroll[1].view_rect;
        return true;
    }
    if (tab == 1 && ui_btn_process_touch(&data->apply_pins, touching, x, y, tick)) {
        data->dirty_rect = data->tabview.content_rect;
        return true;
    }
    if (tab == 1 &&
        (ui_pin_config_process_touch(&data->swclk_pin, touching, x, y, tick) ||
         ui_pin_config_process_touch(&data->swdio_pin, touching, x, y, tick))) {
        data->dirty_rect = data->tabview.content_rect;
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
    if (data->tabview.active_tab_idx == 0) {
        ui_btn_draw(tile, &data->probe);
        ui_btn_draw(tile, &data->pio_probe);
        ui_dropdown_draw(tile, &data->pio_clock);
    }
    if (data->tabview.active_tab_idx == 1) {
        ui_switch_draw(tile, &data->dap_switch);
        ui_pin_config_draw(tile, &data->swclk_pin);
        ui_pin_config_draw(tile, &data->swdio_pin);
        ui_btn_draw(tile, &data->apply_pins);
    }
    ui_pin_config_draw_modal(tile, &data->swclk_pin);
    ui_pin_config_draw_modal(tile, &data->swdio_pin);
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
