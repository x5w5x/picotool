#include "ui_waveform.h"
#include "ui_draw.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TOUCH_SLOP_PX   8 // 8 像素滑动防抖死区

/* ================= 1. 默认通道配色表 ================= */
static const uint16_t k_default_ch_colors[UI_WAVE_MAX_CHANNELS] = {
    0x07E0, // CH0: 亮绿
    0x07FF, // CH1: 青蓝
    0xFFE0, // CH2: 亮黄
    0xFD20, // CH3: 橙色
    0xF81F, // CH4: 紫红
    0x3BFF, // CH5: 冰蓝
    0xFA60, // CH6: 暖橙
    0xFFFF  // CH7: 纯白
};

/* ================= 2. 初始化与绑定 ================= */

void ui_waveform_init(ui_waveform_t *wf, int16_t x, int16_t y, int16_t w, int16_t h,
                      ui_waveform_mode_t mode, uint32_t sample_rate_hz) {
    if (!wf) return;

    wf->rect = ui_rect_make(x, y, w, h);
    wf->mode = mode;

    wf->digital_data        = NULL;
    wf->analog_data         = NULL;
    wf->total_samples       = 0;

    wf->view_offset         = 0;
    wf->samples_per_pixel   = 1;
    wf->active_channels     = 8;
    wf->sample_rate_hz      = (sample_rate_hz > 0) ? sample_rate_hz : 10000000; // 默认 10MHz

    wf->show_cursors        = true;
    wf->cursor_a_x          = x + w / 4;
    wf->cursor_b_x          = x + (w * 3) / 4;

    wf->drag_state          = UI_WAVE_DRAG_NONE;
    wf->down_x              = 0;
    wf->down_y              = 0;
    wf->touch_start_x       = 0;
    wf->view_start_offset   = 0;
    wf->is_touch_down       = false;
    wf->is_dragged          = false;

    wf->color_bg            = 0x0000; // 纯黑底
    wf->color_grid          = 0x2104; // 深暗灰网格线
    wf->color_cursor_a      = 0xFFE0; // 金黄游标 A
    wf->color_cursor_b      = 0xF81F; // 紫红游标 B

    for (int i = 0; i < UI_WAVE_MAX_CHANNELS; i++) {
        wf->color_channels[i] = k_default_ch_colors[i];
    }

    wf->on_change           = NULL;
    wf->user_data           = NULL;
}

void ui_waveform_bind_digital(ui_waveform_t *wf, const uint8_t *data, uint32_t total_samples) {
    if (!wf) return;
    wf->digital_data  = data;
    wf->total_samples = total_samples;
}

void ui_waveform_bind_analog(ui_waveform_t *wf, const int16_t *data, uint32_t total_samples) {
    if (!wf) return;
    wf->analog_data   = data;
    wf->total_samples = total_samples;
}

void ui_waveform_set_scale(ui_waveform_t *wf, uint16_t samples_per_pixel, uint8_t active_channels) {
    if (!wf) return;
    wf->samples_per_pixel = (samples_per_pixel > 0) ? samples_per_pixel : 1;
    wf->active_channels   = (active_channels > 0 && active_channels <= UI_WAVE_MAX_CHANNELS) ? active_channels : 8;
}

/* ================= 3. 触控状态机 (游标抓取 + 时间轴平移) ================= */

bool ui_waveform_process_touch(ui_waveform_t *wf, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!wf) return false;

    bool need_redraw = false;

    if (is_touching) {
        if (!wf->is_touch_down) {
            // A. 初次按下 (Touch Down): 必须命中波形视口
            if (ui_rect_contains_point(&wf->rect, tx, ty)) {
                wf->is_touch_down     = true;
                wf->down_x            = tx;
                wf->down_y            = ty;
                wf->touch_start_x     = tx;
                wf->view_start_offset = wf->view_offset;

                // 优先判定是否命中测量游标 (±12 像素容差)
                if (wf->show_cursors && abs(tx - wf->cursor_a_x) <= 12) {
                    wf->drag_state = UI_WAVE_DRAG_CURSOR_A;
                    wf->is_dragged = true;
                } else if (wf->show_cursors && abs(tx - wf->cursor_b_x) <= 12) {
                    wf->drag_state = UI_WAVE_DRAG_CURSOR_B;
                    wf->is_dragged = true;
                } else {
                    wf->drag_state = UI_WAVE_DRAG_PAN;
                    wf->is_dragged = false;
                }
            }
        } else {
            // B. 持续移动 (Touch Move): 8px 防抖死区与平滑追踪
            int16_t delta_x = tx - wf->touch_start_x;

            if (wf->drag_state == UI_WAVE_DRAG_CURSOR_A) {
                int16_t nx = tx;
                if (nx < wf->rect.x) nx = wf->rect.x;
                if (nx >= wf->rect.x + wf->rect.w) nx = (int16_t)(wf->rect.x + wf->rect.w - 1);
                if (wf->cursor_a_x != nx) {
                    wf->cursor_a_x = nx;
                    need_redraw = true;
                }
            } else if (wf->drag_state == UI_WAVE_DRAG_CURSOR_B) {
                int16_t nx = tx;
                if (nx < wf->rect.x) nx = wf->rect.x;
                if (nx >= wf->rect.x + wf->rect.w) nx = (int16_t)(wf->rect.x + wf->rect.w - 1);
                if (wf->cursor_b_x != nx) {
                    wf->cursor_b_x = nx;
                    need_redraw = true;
                }
            } else if (wf->drag_state == UI_WAVE_DRAG_PAN) {
                if (!wf->is_dragged && (abs(tx - wf->down_x) >= TOUCH_SLOP_PX || abs(ty - wf->down_y) >= TOUCH_SLOP_PX)) {
                    wf->is_dragged = true;
                }

                if (wf->is_dragged) {
                    // 水平平移波形时间轴
                    int32_t shift_samples = (int32_t)delta_x * wf->samples_per_pixel;
                    int32_t new_offset = (int32_t)wf->view_start_offset - shift_samples;
                    if (new_offset < 0) new_offset = 0;

                    int32_t max_offset = (int32_t)wf->total_samples - ((int32_t)wf->rect.w * wf->samples_per_pixel);
                    if (max_offset < 0) max_offset = 0;
                    if (new_offset > max_offset) new_offset = max_offset;

                    if (wf->view_offset != (uint32_t)new_offset) {
                        wf->view_offset = (uint32_t)new_offset;
                        need_redraw = true;
                    }
                }
            }

            if (need_redraw && wf->on_change) {
                wf->on_change(wf, wf->user_data);
            }
        }
        return need_redraw;
    } else {
        // C. 手指松开 (Touch Up)
        if (wf->is_touch_down) {
            wf->is_touch_down = false;
            wf->is_dragged    = false;
            if (wf->drag_state != UI_WAVE_DRAG_NONE) {
                wf->drag_state = UI_WAVE_DRAG_NONE;
                return true;
            }
        }
    }

    return false;
}

/* ================= 4. 切片光栅化渲染核心 ================= */

static void format_time_delta(char *buf, size_t buf_size, uint32_t delta_samples, uint32_t sample_rate) {
    if (sample_rate == 0) sample_rate = 1;
    double dt_sec = (double)delta_samples / (double)sample_rate;

    if (dt_sec < 1e-6) {
        snprintf(buf, buf_size, "dT:%.1fns", dt_sec * 1e9);
    } else if (dt_sec < 1e-3) {
        snprintf(buf, buf_size, "dT:%.2fus", dt_sec * 1e6);
    } else if (dt_sec < 1.0) {
        snprintf(buf, buf_size, "dT:%.2fms", dt_sec * 1e3);
    } else {
        snprintf(buf, buf_size, "dT:%.2fs", dt_sec);
    }
}

void ui_waveform_draw(ui_tile_t *tile, const ui_waveform_t *wf) {
    if (!tile || !wf) return;

    ui_rect_t overlap;
    if (!ui_rect_intersect(&tile->rect, &wf->rect, &overlap)) {
        return;
    }

    // 1. 擦除波形视口背景与绘制边框
    ui_draw_fill_rect(tile, &overlap, wf->color_bg);
    ui_draw_rect(tile, &wf->rect, 0x39E7);

    // 2. 锁定像素级视口裁剪
    ui_draw_set_clip(&wf->rect);

    // ================= A. 8 通道数字逻辑时序渲染 =================
    if (wf->mode == UI_WAVE_MODE_DIGITAL_8CH && wf->digital_data) {
        uint8_t ch_count = wf->active_channels;
        if (ch_count == 0) ch_count = 1;

        int16_t ch_slot_h = (int16_t)((wf->rect.h - 20) / ch_count); // 预留顶部 20px 标尺栏
        int16_t wave_h = (ch_slot_h > 10) ? (int16_t)(ch_slot_h - 6) : 6;

        int16_t draw_start_x = (int16_t)(wf->rect.x + 36); // 左侧留出 36px 绘制通道名
        int16_t draw_end_x   = (int16_t)(wf->rect.x + wf->rect.w);

        // ⚡ 核心算术剪枝：仅处理与当前切片相交的通道
        int16_t first_ch = (tile->rect.y - (wf->rect.y + 20)) / ch_slot_h;
        int16_t last_ch  = (tile->rect.y + tile->rect.h - 1 - (wf->rect.y + 20)) / ch_slot_h;

        if (first_ch < 0) first_ch = 0;
        if (last_ch >= (int16_t)ch_count) last_ch = (int16_t)ch_count - 1;

        for (int16_t ch = first_ch; ch <= last_ch; ch++) {
            int16_t slot_top = (int16_t)(wf->rect.y + 20 + ch * ch_slot_h);
            int16_t y_low  = (int16_t)(slot_top + wave_h);
            int16_t y_high = (int16_t)(slot_top + 2);

            // 绘制通道分割基准线
            int16_t grid_y = (int16_t)(slot_top + ch_slot_h - 1);
            if (grid_y >= tile->rect.y && grid_y < (tile->rect.y + tile->rect.h)) {
                ui_draw_fast_hline(tile, wf->rect.x, grid_y, wf->rect.w, wf->color_grid);
            }

            // 绘制左侧通道标签 (如 D0, D1...)
            int16_t label_y = (int16_t)(slot_top + (ch_slot_h - 16) / 2);
            if ((label_y + 16) > tile->rect.y && label_y < (tile->rect.y + tile->rect.h)) {
                char label[6];
                snprintf(label, sizeof(label), "D%d", ch);
                ui_draw_string(tile, (int16_t)(wf->rect.x + 6), label_y, 
                               label, FONT_16, wf->color_channels[ch], wf->color_bg);
            }

            // 光栅化渲染方波电平与跃变边沿
            uint8_t mask = (uint8_t)(1 << ch);
            int16_t prev_y = -1;

            for (int16_t px = draw_start_x; px < draw_end_x; px++) {
                uint32_t s_idx = wf->view_offset + (uint32_t)(px - draw_start_x) * wf->samples_per_pixel;
                if (s_idx >= wf->total_samples) break;

                bool is_high = (wf->digital_data[s_idx] & mask) != 0;
                int16_t cur_y = is_high ? y_high : y_low;

                // 绘制水平电平点
                if (cur_y >= tile->rect.y && cur_y < (tile->rect.y + tile->rect.h)) {
                    ui_draw_point(tile, px, cur_y, wf->color_channels[ch]);
                }

                // 绘制电平跳变垂直边沿
                if (prev_y != -1 && prev_y != cur_y) {
                    int16_t v_top = (prev_y < cur_y) ? prev_y : cur_y;
                    int16_t v_h   = (int16_t)(abs(cur_y - prev_y) + 1);
                    ui_draw_fast_vline(tile, px, v_top, v_h, wf->color_channels[ch]);
                }
                prev_y = cur_y;
            }
        }
    }

    // ================= B. 模拟示波器曲线渲染 =================
    else if (wf->mode == UI_WAVE_MODE_ANALOG_1CH && wf->analog_data) {
        int16_t mid_y = (int16_t)(wf->rect.y + wf->rect.h / 2);
        int16_t prev_x = -1, prev_y = -1;

        // 绘制中心基准线
        if (mid_y >= tile->rect.y && mid_y < (tile->rect.y + tile->rect.h)) {
            ui_draw_fast_hline(tile, wf->rect.x, mid_y, wf->rect.w, wf->color_grid);
        }

        for (int16_t px = wf->rect.x; px < (wf->rect.x + wf->rect.w); px++) {
            uint32_t s_idx = wf->view_offset + (uint32_t)(px - wf->rect.x) * wf->samples_per_pixel;
            if (s_idx >= wf->total_samples) break;

            int16_t val = wf->analog_data[s_idx];
            int16_t py = (int16_t)(mid_y - val);

            if (prev_x != -1) {
                ui_draw_line(tile, prev_x, prev_y, px, py, 0x07E0);
            }
            prev_x = px;
            prev_y = py;
        }
    }

    // ================= C. 双游标测量线与测量看板 =================
    if (wf->show_cursors) {
        // 绘制 Cursor A (金黄线)
        if (wf->cursor_a_x >= wf->rect.x && wf->cursor_a_x < (wf->rect.x + wf->rect.w)) {
            ui_draw_fast_vline(tile, wf->cursor_a_x, wf->rect.y, wf->rect.h, wf->color_cursor_a);
            if ((wf->rect.y + 16) > tile->rect.y && wf->rect.y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, (int16_t)(wf->cursor_a_x + 2), (int16_t)(wf->rect.y + 2), "A", FONT_16, wf->color_cursor_a, wf->color_bg);
            }
        }

        // 绘制 Cursor B (紫红线)
        if (wf->cursor_b_x >= wf->rect.x && wf->cursor_b_x < (wf->rect.x + wf->rect.w)) {
            ui_draw_fast_vline(tile, wf->cursor_b_x, wf->rect.y, wf->rect.h, wf->color_cursor_b);
            if ((wf->rect.y + 16) > tile->rect.y && wf->rect.y < (tile->rect.y + tile->rect.h)) {
                ui_draw_string(tile, (int16_t)(wf->cursor_b_x + 2), (int16_t)(wf->rect.y + 2), "B", FONT_16, wf->color_cursor_b, wf->color_bg);
            }
        }

        // 顶部测量标尺看板 (显示 dT)
        int16_t draw_start_x = (wf->mode == UI_WAVE_MODE_DIGITAL_8CH) ? (int16_t)(wf->rect.x + 36) : wf->rect.x;
        int16_t ca = (wf->cursor_a_x > draw_start_x) ? (int16_t)(wf->cursor_a_x - draw_start_x) : 0;
        int16_t cb = (wf->cursor_b_x > draw_start_x) ? (int16_t)(wf->cursor_b_x - draw_start_x) : 0;

        uint32_t delta_pts = (uint32_t)abs(ca - cb) * wf->samples_per_pixel;
        char time_str[32];
        format_time_delta(time_str, sizeof(time_str), delta_pts, wf->sample_rate_hz);

        ui_rect_t banner_rect = ui_rect_make((int16_t)(wf->rect.x + wf->rect.w - 140), (int16_t)(wf->rect.y + 2), 136, 16);
        ui_rect_t ban_ov;
        if (ui_rect_intersect(&tile->rect, &banner_rect, &ban_ov)) {
            ui_draw_fill_rect(tile, &ban_ov, 0x18C3);
            ui_draw_string(tile, (int16_t)(banner_rect.x + 4), banner_rect.y, time_str, FONT_16, 0x07FF, 0x18C3);
        }
    }

    ui_draw_reset_clip();
}