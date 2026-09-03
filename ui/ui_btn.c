#include "ui_btn.h"
#include "ui_draw.h"
#include <stdlib.h>
#include <string.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

/* ================= 1. 辅助几何与文本测量 ================= */

static inline bool ui_btn_hit_test(const ui_rect_t *rect, int16_t x, int16_t y) {
    return (x >= rect->x && x < (rect->x + rect->w) &&
            y >= rect->y && y < (rect->y + rect->h));
}

static uint8_t ui_btn_get_font_height(FontSize_t size) {
    switch (size) {
        case FONT_16: return 16;
        case FONT_20: return 20;
        case FONT_24: return 24;
        default:      return 16;
    }
}

static int16_t ui_btn_measure_string_width(const char *str, FontSize_t size) {
    if (!str) return 0;
    uint8_t font_h = ui_btn_get_font_height(size);
    uint8_t half_w = font_h / 2;
    int16_t width = 0;

    while (*str) {
        if ((uint8_t)*str >= 0x80) {
            if (*(str + 1) == '\0') break;
            width += font_h;
            str += 2;
        } else {
            // ASCII 半角字符
            width += half_w;
            str += 1;
        }
    }
    return width;
}

/* ================= 2. 按钮初始化 ================= */

void ui_btn_init(ui_btn_t *btn, int16_t x, int16_t y, int16_t w, int16_t h,
                 const char *text, ui_btn_cb_t on_click) {
    if (!btn) return;

    btn->rect = ui_rect_make(x, y, w, h);

    btn->text           = text;
    btn->font_size      = FONT_16; //默认最小
    btn->mode           = UI_BTN_MODE_PUSH;
    btn->style          = UI_BTN_STYLE_RECT;

    btn->color_bg       = 0x2104; // 默认深灰底
    btn->color_active   = 0x07E0; // 默认亮绿激活色
    btn->color_text     = 0xFFFF; // 默认纯白字

    btn->is_pressed     = false;
    btn->is_checked     = false;
    btn->show_underline = false;

    btn->last_tick      = 0;
    btn->debounce_ms    = 50;

    btn->down_x         = 0;
    btn->down_y         = 0;
    btn->is_touch_down  = false;
    btn->is_dragged     = false;

    btn->on_click       = on_click;
    btn->user_data      = NULL;
}

void ui_btn_set_color(ui_btn_t *btn, uint16_t bg, uint16_t active, uint16_t text) {
    if (!btn) return;
    btn->color_bg     = bg;
    btn->color_active = active;
    btn->color_text   = text;
}

/* ================= 3. 触控状态机 (Touch Slop + Touch Up 触发) ================= */

bool ui_btn_process_touch(ui_btn_t *btn, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    if (!btn) return false;

    bool hit = is_touching && ui_btn_hit_test(&btn->rect, tx, ty);
    bool need_redraw = false;

    if (is_touching) {
        if (!btn->is_touch_down) {
            // A. 初次按下 (Touch Down): 仅在命中本按钮时记录坐标起点
            if (hit) {
                btn->is_touch_down = true;
                btn->is_dragged    = false;
                btn->down_x        = tx;
                btn->down_y        = ty;
                btn->is_pressed    = true;
                need_redraw        = true;
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与移出范围判定
            int16_t dx = abs(tx - btn->down_x);
            int16_t dy = abs(ty - btn->down_y);

            // 位移超限判定为滑动，立即取消按下高亮
            if (!btn->is_dragged && (dx >= TOUCH_SLOP_PX || dy >= TOUCH_SLOP_PX)) {
                btn->is_dragged = true;
                if (btn->is_pressed) {
                    btn->is_pressed = false;
                    need_redraw     = true;
                }
            }

            // 手指滑出按钮区域立即解除高亮
            if (btn->is_pressed && !hit) {
                btn->is_pressed = false;
                need_redraw     = true;
            }
        }
    } else {
        // C.手指松开 (Touch Up): 唯一允许触发业务回调的时机
        if (btn->is_touch_down) {
            btn->is_touch_down = false;

            // 必须满足：未发生拖拽滑动 + 处于按下状态 (或松手点仍在按钮内)
            if (!btn->is_dragged && (btn->is_pressed || hit)) {
                btn->is_pressed = false;

                // Toggle 模式翻转状态
                if (btn->mode == UI_BTN_MODE_TOGGLE) {
                    btn->is_checked = !btn->is_checked;
                }

                // 冷却防抖判定
                if (now_tick - btn->last_tick >= btn->debounce_ms) {
                    btn->last_tick = now_tick;
                    if (btn->on_click) {
                        btn->on_click(btn, btn->user_data);
                    }
                }
                btn->is_dragged = false;
                return true;
            }

            // 滑动取消后的抬手释放
            if (btn->is_pressed) {
                btn->is_pressed = false;
                need_redraw     = true;
            }
            btn->is_dragged = false;
        }
    }

    return need_redraw;
}

/* =================切片安全光栅化渲染================= */

void ui_btn_draw(ui_tile_t *tile, const ui_btn_t *btn) {
    if (!tile || !btn) return;

    // 与当前切片显存求物理交集
    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &btn->rect, &overlap)) {
        return;
    }

    //状态颜色解析
    bool is_active = btn->is_pressed || btn->is_checked;
    uint16_t current_bg = (btn->style == UI_BTN_STYLE_TEXT) ? btn->color_bg 
                                                             : (is_active ? btn->color_active : btn->color_bg);
    uint16_t current_fg = (btn->style == UI_BTN_STYLE_TEXT && is_active) ? btn->color_active 
                                                                         : btn->color_text;

    //形状光栅化
    if (btn->style == UI_BTN_STYLE_RECT) {
        ui_draw_fill_rect(tile, &overlap, current_bg);
        ui_draw_rect(tile, &btn->rect, is_active ? 0x07FF : 0x4208);
    } 
    else if (btn->style == UI_BTN_STYLE_ROUND) {
        int16_t r = btn->rect.h / 2;
        int16_t left_cx  = btn->rect.x + r;
        int16_t right_cx = btn->rect.x + btn->rect.w - r;
        int16_t cy       = btn->rect.y + r;

        ui_draw_fill_circle(tile, left_cx, cy, r, current_bg);
        ui_draw_fill_circle(tile, right_cx, cy, r, current_bg);

        if (btn->rect.w > btn->rect.h) {
            ui_rect_t mid_rect = ui_rect_make(left_cx, btn->rect.y, (int16_t)(btn->rect.w - 2 * r), btn->rect.h);
            ui_rect_t mid_ov;
            if (ui_rect_intersect(&tile->rect, &mid_rect, &mid_ov)) {
                ui_draw_fill_rect(tile, &mid_ov, current_bg);
            }
        }
    }

    // 4. 🔤 文字居中计算与视口安全渲染
    if (btn->text && btn->text[0] != '\0') {
        uint8_t font_h = ui_btn_get_font_height(btn->font_size);
        int16_t text_w = ui_btn_measure_string_width(btn->text, btn->font_size);

        int16_t tx = btn->rect.x + (btn->rect.w - text_w) / 2;
        int16_t ty = btn->rect.y + (btn->rect.h - font_h) / 2;

        if (tx < btn->rect.x) tx = btn->rect.x + 2;

        // 仅当文字垂直跨度与当前切片重叠时才执行字库点阵光栅化
        if ((ty + font_h) > tile->rect.y && ty < (tile->rect.y + tile->rect.h)) {
            ui_draw_string(tile, tx, ty, btn->text, btn->font_size, current_fg, current_bg);

            // 5选中下划线
            if (is_active && btn->show_underline) {
                int16_t line_y = ty + font_h + 2;
                if (line_y >= tile->rect.y && line_y < (tile->rect.y + tile->rect.h)) {
                    ui_draw_fast_hline(tile, tx, line_y, text_w, btn->color_active);
                }
            }
        }
    }
}