#include "app_hid_km.h"
#include "app_shared_ram.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_toast.h"
#include "lcd.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "tusb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define REPORT_ID_KEYBOARD      1
#define REPORT_ID_MOUSE         2

#define HID_QUEUE_SIZE          32
#define MACRO_BTN_COUNT         8

/* ============================================================================
 * 1. 浅色调色板 (纯白背景 + 0xDEFB 组件底色)
 * ============================================================================ */
#define KM_COL_CANVAS           0xFFFF  // ⚡ 全局背景：纯白底板
#define KM_COL_PANEL_BG         0xDEFB  // ⚡ 触控板/主面板底色：0xDEFB
#define KM_COL_KEY_NORMAL       0xDEFB  // ⚡ 键帽/按键常态底色：0xDEFB
#define KM_COL_TOPBAR_BG        0xDEFB  // ⚡ 顶栏背景底色：0xDEFB
#define KM_COL_WHEEL_BG         0xCE59  // 滚轮滑轨凹槽底色 (略深一层形成视觉槽)

#define KM_COL_BORDER           0xCE59  // 控件微弱浅灰边框
#define KM_COL_BORDER_MUTED     0xA514  // 辅助边框
#define KM_COL_TEXT_TITLE       0x1A7B  // 深藏青标题
#define KM_COL_TEXT_LABEL       0x4208  // 经典炭灰按键文本 (与串口工具完全一致)
#define KM_COL_TEXT_MUTED       0x8410  // 辅助暗灰字符
#define KM_COL_STATUS_OK        0x03E0  // 在线状态翠绿

#define KM_COL_ACCENT           0x03DF  // 科技青蓝 (模式按钮/左键/敲下高亮)
#define KM_COL_ACCENT_HOVER     0x0275  // 深青
#define KM_COL_KEY_PRESSED      0x03DF  // 键帽敲下高亮
#define KM_COL_KEY_LOCKED       0xFDE0  // 琥珀金 (Ctrl/Shift锁定高亮)

/* 全屏几何布局 (y: 32 ~ 320) */
#define TOP_BAR_Y               32
#define TOP_BAR_H               34

#define PAD_X                   8
#define PAD_Y                   70
#define PAD_W                   418
#define PAD_H                   184

#define WHEEL_STRIP_X           434
#define WHEEL_STRIP_W           38
#define WHEEL_STRIP_H           PAD_H

#define M_BTN_Y                 260
#define M_BTN_H                 52
#define M_L_X                   8
#define M_L_W                   228
#define M_R_X                   244
#define M_R_W                   228

typedef enum {
    KM_MODE_TRACKPAD = 0,
    KM_MODE_KEYBOARD,
    KM_MODE_MACRO,
    KM_MODE_MAX
} km_mode_t;

typedef enum {
    HID_EVT_NONE = 0,
    HID_EVT_MOUSE_MOVE,
    HID_EVT_MOUSE_BTN,
    HID_EVT_MOUSE_WHEEL,
    HID_EVT_KEY_TAP
} hid_evt_type_t;

typedef struct {
    uint8_t type;
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;
    uint8_t mouse_buttons;
    uint8_t modifier;
    uint8_t keycode;
} hid_event_t;

typedef struct {
    int16_t x, y, w, h;
    const char *label;
    uint8_t keycode;
} direct_key_t;

typedef struct {
    ui_rect_t       dirty_rect;
    km_mode_t       cur_mode;

    ui_btn_t        btn_mode_switch;

    /* 触控板追踪 */
    int16_t         touch_last_x;
    int16_t         touch_last_y;
    int16_t         touch_down_x;
    int16_t         touch_down_y;
    uint32_t        touch_down_tick;
    bool            touch_in_pad;
    bool            touch_in_wheel;
    bool            touch_is_drag;

    ui_btn_t        btn_mouse_l;
    ui_btn_t        btn_mouse_r;

    bool            mod_ctrl;
    bool            mod_shift;
    bool            mod_alt;
    int8_t          pressed_key_idx;

    ui_btn_t        macro_btns[MACRO_BTN_COUNT];

    ui_toast_t      toast;
    uint32_t        sent_mouse_packets;
    uint32_t        sent_key_packets;

    volatile bool   core1_worker_run;
    volatile bool   core1_quiescent;
} hid_km_data_t;

static hid_event_t      s_evt_queue[HID_QUEUE_SIZE];
static volatile uint8_t s_evt_head = 0;
static volatile uint8_t s_evt_tail = 0;
static volatile bool    s_km_session_active = false;

static app_t            s_app_hid_km;
extern void             usb_switch_mode(uint8_t mode);

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

static bool hid_queue_push(const hid_event_t *evt)
{
    uint8_t next = (uint8_t)((s_evt_head + 1) % HID_QUEUE_SIZE);
    if (next == s_evt_tail) return false;
    s_evt_queue[s_evt_head] = *evt;
    s_evt_head = next;
    return true;
}

static void hid_post_key(uint8_t modifier, uint8_t keycode)
{
    hid_event_t evt = {
        .type     = HID_EVT_KEY_TAP,
        .modifier = modifier,
        .keycode  = keycode
    };
    hid_queue_push(&evt);
}

/* ============================================================================
 * 2. Core 1: 专用 USB 协议处理线程
 * ============================================================================ */
static void hid_km_core1_worker(void)
{
    hid_km_data_t *data = (hid_km_data_t *)g_static_app_ram;

    while (true) {
        if (!s_km_session_active || !data->core1_worker_run) {
            data->core1_quiescent = true;
            sleep_ms(20);
            continue;
        }

        data->core1_quiescent = false;

        while (s_evt_tail != s_evt_head) {
            if (!tud_hid_ready()) {
                sleep_ms(2);
                break;
            }

            hid_event_t evt = s_evt_queue[s_evt_tail];
            s_evt_tail = (uint8_t)((s_evt_tail + 1) % HID_QUEUE_SIZE);

            switch (evt.type) {
            case HID_EVT_MOUSE_MOVE:
            case HID_EVT_MOUSE_BTN:
            case HID_EVT_MOUSE_WHEEL:
                tud_hid_mouse_report(REPORT_ID_MOUSE, evt.mouse_buttons, evt.dx, evt.dy, evt.wheel, 0);
                data->sent_mouse_packets++;
                break;

            case HID_EVT_KEY_TAP: {
                uint8_t keys[6] = { evt.keycode, 0 };
                while (!tud_hid_ready()) {
                    sleep_ms(1);
                }
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, evt.modifier, keys);

                sleep_ms(15);
                while (!tud_hid_ready()) {
                    sleep_ms(1);
                }
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
                data->sent_key_packets++;
                break;
            }
            default:
                break;
            }
        }
        sleep_ms(2);
    }
}

/* ============================================================================
 * 3. 顶部工具栏绘制
 * ============================================================================ */
static void top_bar_draw(ui_tile_t *tile, const hid_km_data_t *data)
{
    ui_rect_t bar_rect = ui_rect_make(0, TOP_BAR_Y, LCD_WIDTH, TOP_BAR_H);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &bar_rect, &ov)) {
        ui_draw_fill_rect(tile, &ov, KM_COL_TOPBAR_BG);
        ui_draw_line(tile, 0, TOP_BAR_Y + TOP_BAR_H - 1, LCD_WIDTH, TOP_BAR_Y + TOP_BAR_H - 1, KM_COL_BORDER);

        bool ready = tud_hid_ready();
        char info[64];
        snprintf(info, sizeof(info), "USB键鼠 %s  M:%lu  K:%lu",
                 ready ? "[在线]" : "[未挂载]",
                 (unsigned long)data->sent_mouse_packets,
                 (unsigned long)data->sent_key_packets);
        ui_draw_string(tile, 10, TOP_BAR_Y + 9, info, FONT_16, 0xFFFF, KM_COL_TOPBAR_BG);
    }

    ui_btn_draw(tile, &data->btn_mode_switch);
}

static void on_mode_switch_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    hid_km_data_t *data = (hid_km_data_t *)user_data;
    if (!data) return;

    data->cur_mode = (km_mode_t)((data->cur_mode + 1) % KM_MODE_MAX);

    switch (data->cur_mode) {
    case KM_MODE_TRACKPAD:
        data->btn_mode_switch.text = "模式: 触控板";
        ui_toast_show(&data->toast, "切至: 触控板模式", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
        break;
    case KM_MODE_KEYBOARD:
        data->btn_mode_switch.text = "模式: 直通键盘";
        ui_toast_show(&data->toast, "切至: 直通键盘模式", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
        break;
    case KM_MODE_MACRO:
        data->btn_mode_switch.text = "模式: 快捷宏";
        ui_toast_show(&data->toast, "切至: 快捷宏面板", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
        break;
    default:
        break;
    }

    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

/* ============================================================================
 * 4. 模式 0: 全屏触控板与双向无级滚轮
 * ============================================================================ */
static void trackpad_draw(ui_tile_t *tile, const hid_km_data_t *data)
{
    // 主触控板：0xDEFB 灰底，嵌在纯白画布上
    ui_rect_t pad_rect = ui_rect_make(PAD_X, PAD_Y, PAD_W, PAD_H);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &pad_rect, &ov)) {
        ui_draw_fill_rect(tile, &ov, KM_COL_PANEL_BG);
        ui_draw_rect(tile, &pad_rect, KM_COL_BORDER);
        ui_draw_string(tile, PAD_X + PAD_W / 2 - 40, PAD_Y + PAD_H / 2 - 8, "[ 触控面板 ]", FONT_16, 0xFFFF, KM_COL_PANEL_BG);
    }

    // 右侧滑轨滚轮
    ui_rect_t wheel_rect = ui_rect_make(WHEEL_STRIP_X, PAD_Y, WHEEL_STRIP_W, WHEEL_STRIP_H);
    if (ui_rect_intersect(&tile->rect, &wheel_rect, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0xDEFB);
        ui_draw_rect(tile, &wheel_rect, KM_COL_BORDER);
        ui_draw_string(tile, WHEEL_STRIP_X + 15, PAD_Y + 10, "+", FONT_16, 0xFFFF, 0xDEFB);
        ui_draw_string(tile, WHEEL_STRIP_X + 11, PAD_Y + PAD_H / 2 - 8, "滚", FONT_16, 0xFFFF, 0xDEFB);
        ui_draw_string(tile, WHEEL_STRIP_X + 15, PAD_Y + PAD_H - 24, "-", FONT_16, 0xFFFF, 0xDEFB);
    }

    ui_btn_draw(tile, &data->btn_mouse_l);
    ui_btn_draw(tile, &data->btn_mouse_r);
}

static bool trackpad_process_touch(hid_km_data_t *data, bool touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    ui_rect_t pad_rect   = ui_rect_make(PAD_X, PAD_Y, PAD_W, PAD_H);
    ui_rect_t wheel_rect = ui_rect_make(WHEEL_STRIP_X, PAD_Y, WHEEL_STRIP_W, WHEEL_STRIP_H);

    if (ui_btn_process_touch(&data->btn_mouse_l, touching, tx, ty, now_tick)) {
        uint8_t mask = data->btn_mouse_l.is_pressed ? MOUSE_BUTTON_LEFT : 0;
        hid_event_t evt = { .type = HID_EVT_MOUSE_BTN, .mouse_buttons = mask };
        hid_queue_push(&evt);
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_mouse_l.rect);
        return true;
    }
    if (ui_btn_process_touch(&data->btn_mouse_r, touching, tx, ty, now_tick)) {
        uint8_t mask = data->btn_mouse_r.is_pressed ? MOUSE_BUTTON_RIGHT : 0;
        hid_event_t evt = { .type = HID_EVT_MOUSE_BTN, .mouse_buttons = mask };
        hid_queue_push(&evt);
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_mouse_r.rect);
        return true;
    }

    if (touching) {
        if (!data->touch_in_pad && !data->touch_in_wheel) {
            if (ui_rect_contains_point(&wheel_rect, tx, ty)) {
                data->touch_in_wheel = true;
                data->touch_last_y   = ty;
                return true;
            } else if (ui_rect_contains_point(&pad_rect, tx, ty)) {
                data->touch_in_pad    = true;
                data->touch_down_x    = tx;
                data->touch_down_y    = ty;
                data->touch_last_x    = tx;
                data->touch_last_y    = ty;
                data->touch_down_tick = now_tick;
                data->touch_is_drag   = false;
                return true;
            }
        } else if (data->touch_in_wheel) {
            int16_t dy = ty - data->touch_last_y;
            if (abs(dy) >= 8) {
                int8_t step = (dy < 0) ? 1 : -1;
                hid_event_t evt = { .type = HID_EVT_MOUSE_WHEEL, .wheel = step };
                hid_queue_push(&evt);
                data->touch_last_y = ty;
            }
            return true;
        } else if (data->touch_in_pad) {
            int16_t dx = tx - data->touch_last_x;
            int16_t dy = ty - data->touch_last_y;
            if (abs(tx - data->touch_down_x) > 4 || abs(ty - data->touch_down_y) > 4) {
                data->touch_is_drag = true;
            }
            if (dx != 0 || dy != 0) {
                hid_event_t evt = {
                    .type = HID_EVT_MOUSE_MOVE,
                    .dx   = (int8_t)(dx * 2),
                    .dy   = (int8_t)(dy * 2)
                };
                hid_queue_push(&evt);
                data->touch_last_x = tx;
                data->touch_last_y = ty;
            }
            return true;
        }
    } else {
        if (data->touch_in_wheel) {
            data->touch_in_wheel = false;
            return true;
        }
        if (data->touch_in_pad) {
            data->touch_in_pad = false;
            if (!data->touch_is_drag && (now_tick - data->touch_down_tick <= 220)) {
                hid_event_t down = { .type = HID_EVT_MOUSE_BTN, .mouse_buttons = MOUSE_BUTTON_LEFT };
                hid_event_t up   = { .type = HID_EVT_MOUSE_BTN, .mouse_buttons = 0 };
                hid_queue_push(&down);
                hid_queue_push(&up);
            }
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * 5. 模式 1: 全屏直通键盘 (0xDEFB 键帽底色)
 * ============================================================================ */
static const direct_key_t s_direct_keys[] = {
    // Row 0
    { 6,   70, 36, 44, "Esc", HID_KEY_ESCAPE },
    { 46,  70, 34, 44, "1",   HID_KEY_1 },
    { 84,  70, 34, 44, "2",   HID_KEY_2 },
    { 122, 70, 34, 44, "3",   HID_KEY_3 },
    { 160, 70, 34, 44, "4",   HID_KEY_4 },
    { 198, 70, 34, 44, "5",   HID_KEY_5 },
    { 236, 70, 34, 44, "6",   HID_KEY_6 },
    { 274, 70, 34, 44, "7",   HID_KEY_7 },
    { 312, 70, 34, 44, "8",   HID_KEY_8 },
    { 350, 70, 34, 44, "9",   HID_KEY_9 },
    { 388, 70, 34, 44, "0",   HID_KEY_0 },
    { 426, 70, 48, 44, "Bksp",HID_KEY_BACKSPACE },

    // Row 1
    { 6,   119, 40, 44, "Tab", HID_KEY_TAB },
    { 50,  119, 34, 44, "Q",   HID_KEY_Q },
    { 88,  119, 34, 44, "W",   HID_KEY_W },
    { 126, 119, 34, 44, "E",   HID_KEY_E },
    { 164, 119, 34, 44, "R",   HID_KEY_R },
    { 202, 119, 34, 44, "T",   HID_KEY_T },
    { 240, 119, 34, 44, "Y",   HID_KEY_Y },
    { 278, 119, 34, 44, "U",   HID_KEY_U },
    { 316, 119, 34, 44, "I",   HID_KEY_I },
    { 354, 119, 34, 44, "O",   HID_KEY_O },
    { 392, 119, 34, 44, "P",   HID_KEY_P },
    { 430, 119, 44, 44, "\\",  HID_KEY_BACKSLASH },

    // Row 2
    { 6,   168, 46, 44, "Caps",HID_KEY_CAPS_LOCK },
    { 56,  168, 34, 44, "A",   HID_KEY_A },
    { 94,  168, 34, 44, "S",   HID_KEY_S },
    { 132, 168, 34, 44, "D",   HID_KEY_D },
    { 170, 168, 34, 44, "F",   HID_KEY_F },
    { 208, 168, 34, 44, "G",   HID_KEY_G },
    { 246, 168, 34, 44, "H",   HID_KEY_H },
    { 284, 168, 34, 44, "J",   HID_KEY_J },
    { 322, 168, 34, 44, "K",   HID_KEY_K },
    { 360, 168, 34, 44, "L",   HID_KEY_L },
    { 398, 168, 76, 44, "Enter", HID_KEY_ENTER },

    // Row 3
    { 6,   217, 56, 44, "Shift", HID_KEY_SHIFT_LEFT },
    { 66,  217, 32, 44, "Z",     HID_KEY_Z },
    { 102, 217, 32, 44, "X",     HID_KEY_X },
    { 138, 217, 32, 44, "C",     HID_KEY_C },
    { 174, 217, 32, 44, "V",     HID_KEY_V },
    { 210, 217, 32, 44, "B",     HID_KEY_B },
    { 246, 217, 32, 44, "N",     HID_KEY_N },
    { 282, 217, 34, 44, "M",     HID_KEY_M },
    { 320, 217, 32, 44, ",",     HID_KEY_COMMA },
    { 356, 217, 32, 44, ".",     HID_KEY_PERIOD },
    { 392, 217, 32, 44, "/",     HID_KEY_SLASH },
    { 428, 217, 46, 44, "^",     HID_KEY_ARROW_UP },

    // Row 4
    { 6,   266, 42, 44, "Ctrl",  HID_KEY_CONTROL_LEFT },
    { 52,  266, 40, 44, "Win",   HID_KEY_GUI_LEFT },
    { 96,  266, 42, 44, "Alt",   HID_KEY_ALT_LEFT },
    { 142, 266, 140,44, "Space", HID_KEY_SPACE },
    { 286, 266, 32, 44, "-",     HID_KEY_MINUS },
    { 322, 266, 32, 44, "=",     HID_KEY_EQUAL },
    { 358, 266, 36, 44, "<",     HID_KEY_ARROW_LEFT },
    { 398, 266, 36, 44, "v",     HID_KEY_ARROW_DOWN },
    { 438, 266, 36, 44, ">",     HID_KEY_ARROW_RIGHT }
};
#define DIRECT_KEY_COUNT (sizeof(s_direct_keys) / sizeof(s_direct_keys[0]))

static void direct_keypad_draw(ui_tile_t *tile, const hid_km_data_t *data)
{
    for (size_t i = 0; i < DIRECT_KEY_COUNT; i++) {
        const direct_key_t *k = &s_direct_keys[i];
        ui_rect_t kr = ui_rect_make(k->x, k->y, k->w, k->h);
        ui_rect_t ov;
        if (!ui_rect_intersect(&tile->rect, &kr, &ov)) continue;

        bool pressed = (data->pressed_key_idx == (int8_t)i);
        bool locked  = false;

        if (k->keycode == HID_KEY_CONTROL_LEFT && data->mod_ctrl)  locked = true;
        if (k->keycode == HID_KEY_SHIFT_LEFT   && data->mod_shift) locked = true;
        if (k->keycode == HID_KEY_ALT_LEFT     && data->mod_alt)   locked = true;

        // 键帽背景 0xDEFB，锁定为金黄 0xFDE0，按压为青蓝 0x03DF
        uint16_t bg = locked ? KM_COL_KEY_LOCKED : (pressed ? KM_COL_KEY_PRESSED : KM_COL_KEY_NORMAL);
        uint16_t tc = locked ? 0x0000 : (pressed ? 0xFFFF : KM_COL_TEXT_LABEL);
        uint16_t bc = locked ? KM_COL_BORDER_MUTED : (pressed ? KM_COL_ACCENT_HOVER : KM_COL_BORDER);

        ui_draw_fill_rect(tile, &ov, bg);
        ui_draw_rect(tile, &kr, bc);

        int16_t lx = k->x + (k->w - (int16_t)strlen(k->label) * 8) / 2;
        int16_t ly = k->y + (k->h - 16) / 2;
        ui_draw_string(tile, lx, ly, k->label, FONT_16, tc, bg);
    }
}

static bool direct_keypad_touch(hid_km_data_t *data, bool touching, int16_t tx, int16_t ty)
{
    static bool s_was_touching = false;

    if (touching && !s_was_touching) {
        s_was_touching = true;
        for (size_t i = 0; i < DIRECT_KEY_COUNT; i++) {
            const direct_key_t *k = &s_direct_keys[i];
            ui_rect_t kr = ui_rect_make(k->x, k->y, k->w, k->h);
            if (ui_rect_contains_point(&kr, tx, ty)) {
                data->pressed_key_idx = (int8_t)i;

                if (k->keycode == HID_KEY_CONTROL_LEFT) {
                    data->mod_ctrl = !data->mod_ctrl;
                    data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
                    return true;
                }
                if (k->keycode == HID_KEY_SHIFT_LEFT) {
                    if (!data->mod_shift) {
                        data->mod_shift = true;
                    } else {
                        data->mod_shift = false;
                        hid_post_key(KEYBOARD_MODIFIER_LEFTSHIFT, 0);
                        ui_toast_show(&data->toast, "中/英切换", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
                    }
                    data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
                    return true;
                }
                if (k->keycode == HID_KEY_ALT_LEFT) {
                    data->mod_alt = !data->mod_alt;
                    data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
                    return true;
                }
                if (k->keycode == HID_KEY_GUI_LEFT) {
                    hid_post_key(KEYBOARD_MODIFIER_LEFTGUI, 0);
                    ui_toast_show(&data->toast, "开始菜单", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
                    data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
                    return true;
                }

                uint8_t mod = 0;
                if (data->mod_ctrl)  mod |= KEYBOARD_MODIFIER_LEFTCTRL;
                if (data->mod_shift) mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
                if (data->mod_alt)   mod |= KEYBOARD_MODIFIER_LEFTALT;

                hid_post_key(mod, k->keycode);

                data->mod_ctrl  = false;
                data->mod_shift = false;
                data->mod_alt   = false;

                data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
                return true;
            }
        }
    } else if (!touching && s_was_touching) {
        s_was_touching = false;
        if (data->pressed_key_idx >= 0) {
            data->pressed_key_idx = -1;
            data->dirty_rect = ui_rect_make(0, 68, LCD_WIDTH, LCD_HEIGHT - 68);
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * 6. 模式 2: 快捷宏面板 (0xDEFB 底色大按键)
 * ============================================================================ */
static void on_macro_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    hid_km_data_t *data = (hid_km_data_t *)user_data;
    if (!data) return;

    for (int i = 0; i < MACRO_BTN_COUNT; i++) {
        if (btn == &data->macro_btns[i]) {
            switch (i) {
            case 0: hid_post_key(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_C); ui_toast_show(&data->toast, "Ctrl+C 复制", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 1: hid_post_key(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_V); ui_toast_show(&data->toast, "Ctrl+V 粘贴", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 2: hid_post_key(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_A); ui_toast_show(&data->toast, "Ctrl+A 全选", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 3: hid_post_key(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_Z); ui_toast_show(&data->toast, "Ctrl+Z 撤销", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 4: hid_post_key(KEYBOARD_MODIFIER_LEFTGUI,  HID_KEY_D); ui_toast_show(&data->toast, "Win+D 桌面", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 5: hid_post_key(KEYBOARD_MODIFIER_LEFTGUI,  HID_KEY_L); ui_toast_show(&data->toast, "Win+L 锁屏", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time())); break;
            case 6: hid_post_key(0, HID_KEY_F5); ui_toast_show(&data->toast, "F5 刷新", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time())); break;
            case 7: hid_post_key(KEYBOARD_MODIFIER_LEFTALT,  HID_KEY_F4); ui_toast_show(&data->toast, "Alt+F4 关闭", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time())); break;
            }
            break;
        }
    }
}

static void macro_panel_draw(ui_tile_t *tile, const hid_km_data_t *data)
{
    for (int i = 0; i < MACRO_BTN_COUNT; i++) {
        ui_btn_draw(tile, &data->macro_btns[i]);
    }
}

/* ============================================================================
 * 7. app_t 生命周期与全屏调度
 * ============================================================================ */
static void km_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void km_enter(app_t *self)
{
    hid_km_data_t *data = (hid_km_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(hid_km_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->cur_mode   = KM_MODE_TRACKPAD;
    data->pressed_key_idx = -1;

    // 1. 顶部模式切换按钮
    ui_btn_init(&data->btn_mode_switch, 336, TOP_BAR_Y + 3, 136, 28, "模式: 触控板", on_mode_switch_click);
    data->btn_mode_switch.user_data = data;
    data->btn_mode_switch.style = UI_BTN_STYLE_ROUND;
    data->btn_mode_switch.font_size = FONT_16;
    ui_btn_set_color(&data->btn_mode_switch, 0xDEFB, KM_COL_ACCENT_HOVER, 0xFFFF);

    // 2. 模式 0 触控板底部双键 (左键青蓝高亮，右键 0xDEFB 灰底)
    ui_btn_init(&data->btn_mouse_l, M_L_X, M_BTN_Y, M_L_W, M_BTN_H, "鼠标左键", NULL);
    data->btn_mouse_l.style = UI_BTN_STYLE_ROUND;
    data->btn_mouse_l.font_size = FONT_16;
    ui_btn_set_color(&data->btn_mouse_l, 0xDEFB, KM_COL_ACCENT_HOVER, 0xFFFF);

    ui_btn_init(&data->btn_mouse_r, M_R_X, M_BTN_Y, M_R_W, M_BTN_H, "鼠标右键", NULL);
    data->btn_mouse_r.style = UI_BTN_STYLE_ROUND;
    data->btn_mouse_r.font_size = FONT_16;
    ui_btn_set_color(&data->btn_mouse_r, KM_COL_KEY_NORMAL, KM_COL_BORDER, 0xFFFF);

    // 3. 模式 2 快捷宏矩阵 (0xDEFB 底色)
    const char *macro_names[MACRO_BTN_COUNT] = {
        "复制 (Ctrl+C)", "粘贴 (Ctrl+V)", "全选 (Ctrl+A)", "撤销 (Ctrl+Z)",
        "显示桌面 (Win+D)", "锁定系统 (Win+L)", "F5 页面刷新", "Alt+F4 关闭窗口"
    };
    int16_t bw = 222, bh = 50, gap_x = 12, gap_y = 10, sx = 12, sy = 74;
    for (int i = 0; i < MACRO_BTN_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        ui_btn_init(&data->macro_btns[i], sx + col * (bw + gap_x), sy + row * (bh + gap_y), bw, bh, macro_names[i], on_macro_click);
        data->macro_btns[i].user_data = data;
        data->macro_btns[i].style = UI_BTN_STYLE_ROUND;
        data->macro_btns[i].font_size = FONT_16;
        ui_btn_set_color(&data->macro_btns[i], KM_COL_KEY_NORMAL, KM_COL_BORDER, KM_COL_TEXT_LABEL);
    }

    ui_toast_init(&data->toast);

    // 4. Core 1 异步后台引擎点火
    s_evt_head = 0;
    s_evt_tail = 0;
    s_km_session_active = true;
    data->core1_worker_run = true;

    multicore_reset_core1();
    multicore_launch_core1(hid_km_core1_worker);

    // 进入 App 自动切入复合键鼠模式 (PID 0xD005)
    usb_switch_mode(2);
}

static void km_exit(app_t *self)
{
    hid_km_data_t *data = (hid_km_data_t *)self->user_data;
    if (data) {
        if (tud_hid_ready()) {
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
            tud_hid_mouse_report(REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
        }

        data->core1_worker_run = false;
        s_km_session_active = false;
        multicore_reset_core1();

        // 退出 App 立即切回默认模式 (PID 0xD003: DAP + CDC 串口)
        usb_switch_mode(0);
    }
    self->user_data = NULL;
}

static void km_update(app_t *self, uint32_t now_tick)
{
    hid_km_data_t *data = (hid_km_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_toast_get_dirty_union(&data->toast));
    }

    static uint32_t s_last_sec = 0;
    if (now_tick - s_last_sec >= 500) {
        s_last_sec = now_tick;
        ui_rect_t top_info = ui_rect_make(0, TOP_BAR_Y, 320, TOP_BAR_H);
        data->dirty_rect = rect_union_box(data->dirty_rect, top_info);
    }
}

static bool km_process_touch(app_t *self, bool touching, int16_t tx, int16_t ty, uint32_t now_tick)
{
    hid_km_data_t *data = (hid_km_data_t *)self->user_data;
    if (!data) return false;

    // 1. 顶部模式切换按钮
    if (ui_btn_process_touch(&data->btn_mode_switch, touching, tx, ty, now_tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->btn_mode_switch.rect);
        return true;
    }

    // 2. 模式路由分发
    bool handled = false;
    switch (data->cur_mode) {
    case KM_MODE_TRACKPAD:
        handled = trackpad_process_touch(data, touching, tx, ty, now_tick);
        break;

    case KM_MODE_KEYBOARD:
        handled = direct_keypad_touch(data, touching, tx, ty);
        break;

    case KM_MODE_MACRO:
        for (int i = 0; i < MACRO_BTN_COUNT; i++) {
            if (ui_btn_process_touch(&data->macro_btns[i], touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, data->macro_btns[i].rect);
                handled = true;
                break;
            }
        }
        break;

    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void km_draw(app_t *self, ui_tile_t *tile)
{
    hid_km_data_t *data = (hid_km_data_t *)self->user_data;
    if (!data) return;

    // 1. 绘制全局纯白基底
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (ui_rect_intersect(&tile->rect, &body, &ov)) {
        ui_draw_fill_rect(tile, &ov, KM_COL_CANVAS);
    }

    // 2. 顶部工具栏
    top_bar_draw(tile, data);

    // 3. 模式分发绘制
    switch (data->cur_mode) {
    case KM_MODE_TRACKPAD:
        trackpad_draw(tile, data);
        break;
    case KM_MODE_KEYBOARD:
        direct_keypad_draw(tile, data);
        break;
    case KM_MODE_MACRO:
        macro_panel_draw(tile, data);
        break;
    default:
        break;
    }

    ui_toast_draw(tile, &data->toast);
}

app_t *app_hid_km_get_instance(void)
{
    s_app_hid_km.id            = APP_ID_CUSTOM_0 + 2;
    s_app_hid_km.name          = "键鼠";
    s_app_hid_km.init          = km_init;
    s_app_hid_km.enter         = km_enter;
    s_app_hid_km.exit          = km_exit;
    s_app_hid_km.update        = km_update;
    s_app_hid_km.process_touch = km_process_touch;
    s_app_hid_km.draw          = km_draw;
    return &s_app_hid_km;
}