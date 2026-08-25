#include "app_swd_probe.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_tabview.h"
#include "lcd.h"
#include "chinese.h"
#include "cmsis_dap.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#define PIN_SWCLK           2
#define PIN_SWDIO           3

#define REG_CPUID           0xE000ED00
#define REG_DBGMCU_IDCODE   0xE0042000
#define REG_STM32F0_DBGMCU  0x40015800
#define REG_STM32F1_FLASH   0x1FFFF7E0
#define REG_STM32F4_FLASH   0x1FFF7A22

#define F103_FLASH_KEYR     0x40022004
#define F103_FLASH_SR       0x4002200C
#define F103_FLASH_CR       0x40022010
#define FLASH_KEY1          0x45670123
#define FLASH_KEY2          0xCDEF89AB

typedef enum {
    TAB_PROBE = 0,
    TAB_CTRL  = 1,
    TAB_FLASH = 2,
    TAB_DAP   = 3,
    TAB_HELP  = 4,
    TAB_COUNT = 5
} swd_tab_idx_t;

typedef struct {
    uint32_t dp_idcode;
    uint32_t cpuid;
    uint32_t dbgmcu_id;
    uint16_t flash_kb;
    char     core_name[24];
    char     chip_model[32];
    bool     target_connected;
    char     diag_msg[48];

    uint32_t current_pc;
    bool     is_halted;
    bool     flash_unlocked;
    char     flash_msg[32];
} swd_dev_state_t;

typedef struct {
    ui_rect_t       dirty_rect;
    ui_tabview_t    tabview;
    swd_dev_state_t state;

    ui_btn_t        btn_probe;
    ui_btn_t        btn_halt;
    ui_btn_t        btn_run;
    ui_btn_t        btn_step;
    ui_btn_t        btn_reset;
    ui_btn_t        btn_unlock;
    ui_btn_t        btn_erase;
    ui_btn_t        btn_toggle_dap;
} app_swd_data_t;

static app_swd_data_t s_swd_data;
static app_t          s_app_swd;
static volatile uint32_t s_swd_delay_cnt = 25;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

#define SWCLK_HIGH()        gpio_put(PIN_SWCLK, 1)
#define SWCLK_LOW()         gpio_put(PIN_SWCLK, 0)
#define SWDIO_HIGH()        gpio_put(PIN_SWDIO, 1)
#define SWDIO_LOW()         gpio_put(PIN_SWDIO, 0)
#define SWDIO_READ()        gpio_get(PIN_SWDIO)

static inline void SWD_Delay(void) {
    for (volatile uint32_t i = 0; i < s_swd_delay_cnt; i++) {
        __asm volatile("nop");
    }
}

static inline void SWDIO_OUT(void) { gpio_set_dir(PIN_SWDIO, GPIO_OUT); }
static inline void SWDIO_IN(void)  { gpio_set_dir(PIN_SWDIO, GPIO_IN); gpio_pull_up(PIN_SWDIO); }

// ⚡ 恢复原始标准的 SWD 写位时序
static void SWD_WriteBit(uint8_t bit) {
    if (bit) SWDIO_HIGH(); else SWDIO_LOW();
    SWD_Delay();
    SWCLK_LOW();
    SWD_Delay();
    SWCLK_HIGH();
}

// ⚡ 恢复原始标准的 SWD 读位时序（低电平采样）
static uint8_t SWD_ReadBit(void) {
    uint8_t bit;
    SWCLK_LOW();
    SWD_Delay();
    bit = SWDIO_READ() ? 1 : 0;
    SWCLK_HIGH();
    SWD_Delay();
    return bit;
}

static void SWD_WriteBits(uint32_t data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) SWD_WriteBit((data >> i) & 0x01);
}

static void SWD_LineReset(void) {
    SWDIO_OUT();
    for (int i = 0; i < 64; i++) SWD_WriteBit(1);
    SWD_WriteBits(0xE79E, 16);
    for (int i = 0; i < 64; i++) SWD_WriteBit(1);
    SWD_WriteBits(0, 16);
}

static uint8_t Cal_Parity(uint32_t val) {
    uint8_t p = 0;
    for (int i = 0; i < 32; i++) if (val & (1U << i)) p++;
    return p & 1;
}

static uint8_t Gen_Req(uint8_t is_ap, uint8_t is_read, uint8_t addr_2_3) {
    uint8_t req = 0x81;
    if (is_ap)   req |= (1 << 1);
    if (is_read) req |= (1 << 2);
    req |= (addr_2_3 & 0x0C) << 1;
    uint8_t p = 0;
    for (int i = 1; i <= 4; i++) if (req & (1 << i)) p++;
    if (p & 1) req |= (1 << 5);
    return req;
}

static uint8_t SWD_Trans(uint8_t req, uint32_t *dat, uint8_t rd) {
    uint8_t ack = 0, p = 0;
    for (int i = 0; i < 8; i++) SWD_WriteBit((req >> i) & 1);

    SWDIO_IN();
    SWD_Delay(); SWCLK_LOW(); SWD_Delay(); SWCLK_HIGH();
    ack |= SWD_ReadBit() << 0;
    ack |= SWD_ReadBit() << 1;
    ack |= SWD_ReadBit() << 2;

    if (ack != 1) { SWDIO_OUT(); return ack; }

    if (rd) {
        uint32_t v = 0;
        for (int i = 0; i < 32; i++) {
            uint8_t b = SWD_ReadBit();
            if (b) v |= (1U << i);
            p ^= b;
        }
        SWD_ReadBit();
        *dat = v;
        SWD_Delay(); SWCLK_LOW(); SWD_Delay(); SWCLK_HIGH();
        SWDIO_OUT();
    } else {
        SWD_Delay(); SWCLK_LOW(); SWD_Delay(); SWCLK_HIGH();
        SWDIO_OUT();
        p = Cal_Parity(*dat);
        for (int i = 0; i < 32; i++) SWD_WriteBit((*dat >> i) & 1);
        SWD_WriteBit(p);
    }

    SWDIO_LOW();
    for (int i = 0; i < 12; i++) {
        SWCLK_LOW(); SWD_Delay(); SWCLK_HIGH(); SWD_Delay();
    }
    return ack;
}

static void Target_Abort(void) {
    uint32_t val = 0x1E;
    SWD_Trans(Gen_Req(0, 0, 0x00), &val, 0);
}

static uint8_t SWD_Trans_Robust(uint8_t req, uint32_t *dat, uint8_t rd) {
    uint8_t ack = 0;
    for (int retry = 0; retry < 50; retry++) {
        ack = SWD_Trans(req, dat, rd);
        if (ack == 1) return 1;
        if (ack == 2) continue;
        if (ack == 4) { Target_Abort(); continue; }
        break;
    }
    return ack;
}

static uint8_t Target_PowerUp(void) {
    uint32_t status = 0;
    uint32_t req = (1U << 28) | (1U << 30);
    Target_Abort();
    if (SWD_Trans_Robust(Gen_Req(0, 0, 0x04), &req, 0) != 1) return 0;
    for (int i = 0; i < 500; i++) {
        if (SWD_Trans_Robust(Gen_Req(0, 1, 0x04), &status, 1) == 1) {
            if ((status & (1U << 29)) && (status & (1U << 31))) return 1;
        }
    }
    return 0;
}

static void Target_Write(uint32_t addr, uint32_t val) {
    uint32_t tmp = 0;
    tmp = 0x00000000; SWD_Trans_Robust(Gen_Req(0, 0, 0x08), &tmp, 0);
    tmp = 0x23000002; SWD_Trans_Robust(Gen_Req(1, 0, 0x00), &tmp, 0);
    tmp = addr;       SWD_Trans_Robust(Gen_Req(1, 0, 0x04), &tmp, 0);
    SWD_Trans_Robust(Gen_Req(1, 0, 0x0C), &val, 0);
}

static uint32_t Target_Read(uint32_t addr) {
    uint32_t tmp = 0, val = 0;
    tmp = 0x00000000; SWD_Trans_Robust(Gen_Req(0, 0, 0x08), &tmp, 0);
    tmp = 0x23000002; SWD_Trans_Robust(Gen_Req(1, 0, 0x00), &tmp, 0);
    tmp = addr;       SWD_Trans_Robust(Gen_Req(1, 0, 0x04), &tmp, 0);
    SWD_Trans_Robust(Gen_Req(1, 1, 0x0C), &val, 1);
    SWD_Trans_Robust(Gen_Req(0, 1, 0x0C), &val, 1);
    return val;
}

static uint32_t SWD_Read_PC(void) {
    Target_Write(0xE000EDF4, 0x0F);
    return Target_Read(0xE000EDF8);
}

static void swd_decode_target(swd_dev_state_t *t) {
    uint16_t part_no = (t->cpuid >> 4) & 0xFFF;
    switch (part_no) {
        case 0xC20: strcpy(t->core_name, "Cortex-M0"); break;
        case 0xC60: strcpy(t->core_name, "Cortex-M0+"); break;
        case 0xC23: strcpy(t->core_name, "Cortex-M3"); break;
        case 0xC24: strcpy(t->core_name, "Cortex-M4"); break;
        case 0xC27: strcpy(t->core_name, "Cortex-M7"); break;
        case 0xD21: strcpy(t->core_name, "Cortex-M33"); break;
        default:    snprintf(t->core_name, sizeof(t->core_name), "ARM Part 0x%03X", part_no); break;
    }

    uint16_t dev_id = t->dbgmcu_id & 0xFFF;
    t->flash_kb = 0;

    switch (dev_id) {
        case 0x412: case 0x410: case 0x414:
            strcpy(t->chip_model, "STM32F103/F10x 系列");
            t->flash_kb = (uint16_t)(Target_Read(REG_STM32F1_FLASH) & 0xFFFF);
            break;
        case 0x413: case 0x419: case 0x423: case 0x431:
            snprintf(t->chip_model, sizeof(t->chip_model), "STM32F4xx (0x%03X)", dev_id);
            t->flash_kb = (uint16_t)(Target_Read(REG_STM32F4_FLASH) & 0xFFFF);
            break;
        default:
            snprintf(t->chip_model, sizeof(t->chip_model), "ARM 目标 (0x%03X)", dev_id);
            break;
    }
}

static bool swd_probe_target(swd_dev_state_t *t) {
    memset(t, 0, sizeof(swd_dev_state_t));
    gpio_init(PIN_SWCLK); gpio_init(PIN_SWDIO);
    gpio_set_dir(PIN_SWCLK, GPIO_OUT); gpio_set_dir(PIN_SWDIO, GPIO_OUT);
    SWCLK_HIGH(); SWDIO_HIGH(); gpio_pull_up(PIN_SWDIO);
    sleep_ms(20);

    s_swd_delay_cnt = 25;
    for (int i = 0; i < 5; i++) { SWD_LineReset(); sleep_ms(2); }

    uint32_t target_id = 0;
    if (SWD_Trans_Robust(Gen_Req(0, 1, 0x00), &target_id, 1) != 1 || target_id == 0) {
        strcpy(t->diag_msg, "未收到 ACK 应答 / 未接通");
        return false;
    }

    t->dp_idcode = target_id;
    if (!Target_PowerUp()) {
        t->target_connected = true;
        strcpy(t->chip_model, "DP在线 但 Power-Up 失败");
        return true;
    }

    s_swd_delay_cnt = 2; // 提速至稳定读写
    Target_Write(0xE000EDF0, 0xA05F0003);
    sleep_ms(1);
    t->cpuid = Target_Read(REG_CPUID);
    t->dbgmcu_id = Target_Read(REG_DBGMCU_IDCODE);
    if ((t->dbgmcu_id & 0xFFF) == 0) t->dbgmcu_id = Target_Read(REG_STM32F0_DBGMCU);
    t->current_pc = SWD_Read_PC();
    t->is_halted = true;
    Target_Write(0xE000EDF0, 0xA05F0001);
    t->is_halted = false;

    t->target_connected = true;
    strcpy(t->diag_msg, "探测成功 | CoreSight 已就绪");
    swd_decode_target(t);
    return true;
}

static void on_probe_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    swd_probe_target(&data->state);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_halt_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(0xE000EDF0, 0xA05F0003);
    sleep_ms(5);
    data->state.current_pc = SWD_Read_PC();
    data->state.is_halted = true;
    data->dirty_rect = data->tabview.content_rect;
}

static void on_run_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(0xE000EDF0, 0xA05F0001);
    data->state.is_halted = false;
    data->dirty_rect = data->tabview.content_rect;
}

static void on_step_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(0xE000EDF0, 0xA05F0005);
    sleep_ms(5);
    Target_Write(0xE000EDF0, 0xA05F0003);
    sleep_ms(5);
    data->state.current_pc = SWD_Read_PC();
    data->state.is_halted = true;
    data->dirty_rect = data->tabview.content_rect;
}

static void on_reset_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(0xE000ED0C, 0x05FA0004);
    Target_Write(0xE000EDF0, 0xA05F0000);
    data->state.is_halted = false;
    data->dirty_rect = data->tabview.content_rect;
}

static void on_unlock_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(F103_FLASH_KEYR, FLASH_KEY1);
    Target_Write(F103_FLASH_KEYR, FLASH_KEY2);
    data->state.flash_unlocked = true;
    strcpy(data->state.flash_msg, "FPEC 解锁成功 (可写入)");
    data->dirty_rect = data->tabview.content_rect;
}

static void on_erase_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    Target_Write(F103_FLASH_KEYR, FLASH_KEY1);
    Target_Write(F103_FLASH_KEYR, FLASH_KEY2);
    Target_Write(F103_FLASH_CR, (1 << 2) | (1 << 6));

    uint32_t timeout = 0;
    while ((Target_Read(F103_FLASH_SR) & 0x01) && timeout < 200) {
        sleep_ms(5);
        timeout++;
    }
    Target_Write(F103_FLASH_CR, 0x00);
    strcpy(data->state.flash_msg, (timeout >= 200) ? "擦除超时" : "全片擦除完成");
    data->dirty_rect = data->tabview.content_rect;
}

// ⚡ 安全开关：仅操作标志位与软插拔，不再复位 Core 1
static void on_toggle_dap_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    g_dap_enabled = !g_dap_enabled;

    if (g_dap_enabled) {
        gpio_init(PIN_SWCLK);
        gpio_init(PIN_SWDIO);
        gpio_set_dir(PIN_SWCLK, GPIO_OUT);
        gpio_set_dir(PIN_SWDIO, GPIO_OUT);
        SWCLK_HIGH();
        SWDIO_HIGH();
        g_dap_packet_counter = 0;

        tud_connect();

        data->btn_toggle_dap.text = "停止 DAP 联机";
        ui_btn_set_color(&data->btn_toggle_dap, 0xF800, 0xB000, 0xFFFF);
    } else {
        tud_disconnect();

        gpio_set_dir(PIN_SWCLK, GPIO_IN);
        gpio_set_dir(PIN_SWDIO, GPIO_IN);

        data->btn_toggle_dap.text = "启动 DAP 联机调试";
        ui_btn_set_color(&data->btn_toggle_dap, 0x05E0, 0x03E0, 0xFFFF);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_tab_change(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)old_tab; (void)new_tab;
    app_swd_data_t *data = (app_swd_data_t *)user_data;
    data->dirty_rect = tv->content_rect;
}

static const char *s_tab_titles[TAB_COUNT] = {
    "芯片探测",
    "内核控制",
    "闪存管理",
    "DAP联机",
    "接线说明"
};

static void app_swd_init(app_t *self) {
    self->user_data = &s_swd_data;
    app_swd_data_t *data = &s_swd_data;

    memset(&data->state, 0, sizeof(swd_dev_state_t));
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32, 
                    108, 44, s_tab_titles, TAB_COUNT, TAB_PROBE, on_tab_change);
    data->tabview.user_data = data;

    ui_btn_init(&data->btn_probe, 140, 240, 310, 42, "一键探测目标芯片", on_probe_click);
    data->btn_probe.user_data = data;
    data->btn_probe.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_probe, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_halt, 130, 140, 150, 40, "暂停 (HALT)", on_halt_click);
    data->btn_halt.user_data = data; data->btn_halt.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_halt, 0xFA00, 0xC800, 0xFFFF);

    ui_btn_init(&data->btn_run, 300, 140, 150, 40, "运行 (RUN)", on_run_click);
    data->btn_run.user_data = data; data->btn_run.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_run, 0x05E0, 0x0400, 0xFFFF);

    ui_btn_init(&data->btn_step, 130, 200, 150, 40, "单步 (STEP)", on_step_click);
    data->btn_step.user_data = data; data->btn_step.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_step, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_reset, 300, 200, 150, 40, "复位 (RESET)", on_reset_click);
    data->btn_reset.user_data = data; data->btn_reset.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_reset, 0x8410, 0x4208, 0xFFFF);

    ui_btn_init(&data->btn_unlock, 150, 150, 280, 44, "解锁 FPEC 闪存", on_unlock_click);
    data->btn_unlock.user_data = data; data->btn_unlock.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_unlock, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_erase, 150, 215, 280, 44, "全片擦除 (Mass Erase)", on_erase_click);
    data->btn_erase.user_data = data; data->btn_erase.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_erase, 0xF800, 0xB000, 0xFFFF);

    ui_btn_init(&data->btn_toggle_dap, 140, 220, 310, 48, "启动 DAP 联机调试", on_toggle_dap_click);
    data->btn_toggle_dap.user_data = data;
    data->btn_toggle_dap.style = UI_BTN_STYLE_ROUND;
    data->btn_toggle_dap.font_size = FONT_20;
    ui_btn_set_color(&data->btn_toggle_dap, 0x05E0, 0x03E0, 0xFFFF);
}

static void app_swd_enter(app_t *self) {
    app_swd_data_t *data = (app_swd_data_t *)self->user_data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

static void app_swd_exit(app_t *self) {
    app_swd_data_t *data = (app_swd_data_t *)self->user_data;
    if (g_dap_enabled) {
        g_dap_enabled = false;
        tud_disconnect();
        gpio_set_dir(PIN_SWCLK, GPIO_IN);
        gpio_set_dir(PIN_SWDIO, GPIO_IN);
        data->btn_toggle_dap.text = "启动 DAP 联机调试";
        ui_btn_set_color(&data->btn_toggle_dap, 0x05E0, 0x03E0, 0xFFFF);
    }
}

static void app_swd_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_swd_data_t *data = (app_swd_data_t *)self->user_data;

    static uint32_t s_last_cnt = 0;
    if (data->tabview.active_tab_idx == TAB_DAP && g_dap_enabled) {
        if (g_dap_packet_counter != s_last_cnt) {
            s_last_cnt = g_dap_packet_counter;
            data->dirty_rect = ui_rect_make(120, 44, 345, 160);
        }
    }
}

static bool app_swd_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_swd_data_t *data = (app_swd_data_t *)self->user_data;

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->tabview.bar_rect);
        return true;
    }

    bool handled = false;
    switch (data->tabview.active_tab_idx) {
        case TAB_PROBE:
            if (ui_btn_process_touch(&data->btn_probe, is_touching, tx, ty, now_tick)) handled = true;
            break;
        case TAB_CTRL:
            if (ui_btn_process_touch(&data->btn_halt, is_touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_run, is_touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_step, is_touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_reset, is_touching, tx, ty, now_tick)) handled = true;
            break;
        case TAB_FLASH:
            if (ui_btn_process_touch(&data->btn_unlock, is_touching, tx, ty, now_tick)) handled = true;
            if (ui_btn_process_touch(&data->btn_erase, is_touching, tx, ty, now_tick)) handled = true;
            break;
        case TAB_DAP:
            if (ui_btn_process_touch(&data->btn_toggle_dap, is_touching, tx, ty, now_tick)) handled = true;
            break;
        default:
            break;
    }
    return handled;
}

static void app_swd_draw(app_t *self, ui_tile_t *tile) {
    app_swd_data_t *data = (app_swd_data_t *)self->user_data;
    ui_rect_t overlap;

    ui_tabview_draw(tile, &data->tabview);

    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case TAB_PROBE: {
            ui_rect_t card = ui_rect_make(120, 44, 345, 180);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);
                if (data->state.target_connected) {
                    ui_draw_string(tile, 135, 54, "状态: 已成功探测", FONT_16, 0x05E0, 0xEF7D);
                    snprintf(str_buf, sizeof(str_buf), "芯片型号: %s", data->state.chip_model);
                    ui_draw_string(tile, 135, 78, str_buf, FONT_16, 0x1A7B, 0xEF7D);
                    snprintf(str_buf, sizeof(str_buf), "ARM 内核: %s", data->state.core_name);
                    ui_draw_string(tile, 135, 102, str_buf, FONT_16, 0x1A7B, 0xEF7D);
                    snprintf(str_buf, sizeof(str_buf), "DP IDCODE: 0x%08lX", data->state.dp_idcode);
                    ui_draw_string(tile, 135, 126, str_buf, FONT_16, 0x03DF, 0xEF7D);
                    snprintf(str_buf, sizeof(str_buf), "片上 Flash: %u KB", data->state.flash_kb);
                    ui_draw_string(tile, 135, 150, str_buf, FONT_16, 0xF800, 0xEF7D);
                } else {
                    ui_draw_string(tile, 135, 54, "状态: 未连接", FONT_16, 0x8410, 0xEF7D);
                    ui_draw_string(tile, 135, 94, "请将 GP2 接 SWCLK, GP3 接 SWDIO", FONT_16, 0x4208, 0xEF7D);
                    ui_draw_string(tile, 135, 124, "必须与目标共地(GND)且目标板已供电", FONT_16, 0x4208, 0xEF7D);
                }
            }
            ui_btn_draw(tile, &data->btn_probe);
            break;
        }

        case TAB_CTRL: {
            ui_rect_t card = ui_rect_make(120, 44, 345, 75);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xDEFB);
                ui_draw_rect(tile, &card, 0x03DF);
                snprintf(str_buf, sizeof(str_buf), "CPU 状态: %s", data->state.is_halted ? "HALTED (暂停)" : "RUNNING (运行中)");
                ui_draw_string(tile, 135, 54, str_buf, FONT_16, data->state.is_halted ? 0xF800 : 0x05E0, 0xDEFB);
                snprintf(str_buf, sizeof(str_buf), "当前 PC: 0x%08lX", data->state.current_pc);
                ui_draw_string(tile, 135, 82, str_buf, FONT_16, 0x1A7B, 0xDEFB);
            }
            ui_btn_draw(tile, &data->btn_halt);
            ui_btn_draw(tile, &data->btn_run);
            ui_btn_draw(tile, &data->btn_step);
            ui_btn_draw(tile, &data->btn_reset);
            break;
        }

        case TAB_FLASH: {
            ui_rect_t card = ui_rect_make(120, 44, 345, 90);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);
                ui_draw_string(tile, 135, 54, "FPEC 状态: ", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 220, 54, data->state.flash_unlocked ? "已解锁" : "已锁定", FONT_16, data->state.flash_unlocked ? 0x05E0 : 0xF800, 0xEF7D);
                if (strlen(data->state.flash_msg) > 0) ui_draw_string(tile, 135, 84, data->state.flash_msg, FONT_16, 0x03DF, 0xEF7D);
            }
            ui_btn_draw(tile, &data->btn_unlock);
            ui_btn_draw(tile, &data->btn_erase);
            break;
        }

        case TAB_DAP: {
            ui_rect_t card = ui_rect_make(120, 44, 345, 150);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &card, 0x07FF);

                if (g_dap_enabled) {
                    ui_draw_string(tile, 135, 56, "● DAP 调试通道: [ 运行中 ]", FONT_16, 0x07FF, 0x10A2);
                    snprintf(str_buf, sizeof(str_buf), "传输数据包: %lu 帧", g_dap_packet_counter);
                    ui_draw_string(tile, 135, 84, str_buf, FONT_16, 0xFFFF, 0x10A2);
                    snprintf(str_buf, sizeof(str_buf), "SWD 频率: %.1f MHz", (float)g_dap_cur_clock_hz / 1000000.0f);
                    ui_draw_string(tile, 135, 112, str_buf, FONT_16, 0x05E0, 0x10A2);
                    ui_draw_string(tile, 135, 140, "电脑端 Keil/OpenOCD 可直接连接", FONT_16, 0x8410, 0x10A2);
                } else {
                    ui_draw_string(tile, 135, 56, "○ DAP 调试通道: [ 已待机 ]", FONT_16, 0x8410, 0x10A2);
                    ui_draw_string(tile, 135, 90, "点击下方按钮开启 DAP 联机通道", FONT_16, 0xFFFF, 0x10A2);
                    ui_draw_string(tile, 135, 124, "开启后 Pico 将作为免驱调试器工作", FONT_16, 0x8410, 0x10A2);
                }
            }
            ui_btn_draw(tile, &data->btn_toggle_dap);
            break;
        }

        case TAB_HELP: {
            ui_rect_t card = ui_rect_make(120, 44, 345, 235);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);
                ui_draw_string(tile, 135, 54, "接线标准说明:", FONT_16, 0x03DF, 0xEF7D);
                ui_draw_string(tile, 135, 80, "Pin 4 (GP2) -> 目标板 SWCLK (PA14)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 106, "Pin 5 (GP3) -> 目标板 SWDIO (PA13)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 132, "GND 针脚    -> 目标板 GND (必须连)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 158, "3.3V 输出   -> 目标板 3.3V (可选)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 135, 194, "支持 Keil/OpenOCD/PyOCD 免驱联机", FONT_16, 0x05E0, 0xEF7D);
                ui_draw_string(tile, 135, 220, "退出 App 或切标签将自动保护断开", FONT_16, 0x8410, 0xEF7D);
            }
            break;
        }
        default:
            break;
    }
}

app_t *app_swd_probe_get_instance(void) {
    s_app_swd.id            = APP_ID_CUSTOM_0;
    s_app_swd.name          = "SWD调试器";
    s_app_swd.init          = app_swd_init;
    s_app_swd.enter         = app_swd_enter;
    s_app_swd.exit          = app_swd_exit;
    s_app_swd.update        = app_swd_update;
    s_app_swd.process_touch = app_swd_process_touch;
    s_app_swd.draw          = app_swd_draw;
    return &s_app_swd;
}