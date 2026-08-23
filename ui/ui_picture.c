#include "ui_picture.h"
#include <string.h>

void ui_picture_init_static(ui_picture_t *pic, int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t *bitmap, bool has_trans, uint16_t trans_color,
                            ui_pic_click_cb_t on_click) {
    if (!pic) return;
    pic->rect           = ui_rect_make(x, y, w, h);
    pic->mode           = UI_PIC_MODE_STATIC;
    pic->static_bitmap  = bitmap;
    pic->anim_frames    = NULL;
    pic->frame_count    = 1;
    pic->cur_frame      = 0;
    pic->frame_delay_ms = 0;
    pic->last_tick      = 0;
    pic->is_playing     = false;
    pic->loop           = false;
    pic->has_trans      = has_trans;
    pic->trans_color    = trans_color;
    pic->on_click       = on_click;
    pic->user_data      = NULL;
}

void ui_picture_init_anim(ui_picture_t *pic, int16_t x, int16_t y, int16_t w, int16_t h,
                          const uint16_t **frames, uint8_t frame_count, uint32_t frame_delay_ms,
                          bool has_trans, uint16_t trans_color, ui_pic_click_cb_t on_click) {
    if (!pic) return;
    pic->rect           = ui_rect_make(x, y, w, h);
    pic->mode           = UI_PIC_MODE_ANIM;
    pic->static_bitmap  = NULL;
    pic->anim_frames    = frames;
    pic->frame_count    = frame_count;
    pic->cur_frame      = 0;
    pic->frame_delay_ms = frame_delay_ms;
    pic->last_tick      = 0;
    pic->is_playing     = true;
    pic->loop           = true;
    pic->has_trans      = has_trans;
    pic->trans_color    = trans_color;
    pic->on_click       = on_click;
    pic->user_data      = NULL;
}

bool ui_picture_update(ui_picture_t *pic, uint32_t now_tick, ui_rect_t *out_dirty) {
    if (!pic || pic->mode != UI_PIC_MODE_ANIM || !pic->is_playing || pic->frame_count <= 1) {
        return false;
    }

    if (now_tick - pic->last_tick >= pic->frame_delay_ms) {
        pic->last_tick = now_tick;
        uint8_t next_f = pic->cur_frame + 1;
        if (next_f >= pic->frame_count) {
            if (pic->loop) {
                next_f = 0;
            } else {
                pic->is_playing = false;
                return false;
            }
        }
        pic->cur_frame = next_f;

        if (out_dirty) {
            *out_dirty = pic->rect;
        }
        return true;
    }
    return false;
}

bool ui_picture_process_touch(ui_picture_t *pic, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    (void)now_tick;
    if (!pic || !pic->on_click) return false;

    if (!is_touching && ui_rect_contains_point(&pic->rect, tx, ty)) {
        pic->on_click(pic, pic->user_data);
        return true;
    }
    return false;
}

void ui_picture_draw(ui_tile_t *tile, const ui_picture_t *pic) {
    if (!tile || !pic) return;

    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &pic->rect, &ov)) return;

    const uint16_t *src_frame = (pic->mode == UI_PIC_MODE_STATIC) 
                                ? pic->static_bitmap 
                                : (pic->anim_frames ? pic->anim_frames[pic->cur_frame] : NULL);
    if (!src_frame) return;

    for (int16_t row = 0; row < ov.h; row++) {
        int16_t cur_y = ov.y + row;
        int16_t src_x = ov.x - pic->rect.x;
        int16_t src_y = cur_y - pic->rect.y;

        const uint16_t *src_line = src_frame + (src_y * pic->rect.w + src_x);
        // 使用 tile->rect.w 代替不存在的 stride
        uint16_t *dst_line = tile->buf + (cur_y - tile->rect.y) * tile->rect.w + (ov.x - tile->rect.x);

        if (!pic->has_trans) {
            memcpy(dst_line, src_line, ov.w * sizeof(uint16_t));
        } else {
            for (int16_t col = 0; col < ov.w; col++) {
                if (src_line[col] != pic->trans_color) {
                    dst_line[col] = src_line[col];
                }
            }
        }
    }
}

void ui_picture_play(ui_picture_t *pic) {
    if (pic && pic->mode == UI_PIC_MODE_ANIM) pic->is_playing = true;
}

void ui_picture_pause(ui_picture_t *pic) {
    if (pic && pic->mode == UI_PIC_MODE_ANIM) pic->is_playing = false;
}

void ui_picture_set_frame(ui_picture_t *pic, uint8_t frame_idx) {
    if (pic && frame_idx < pic->frame_count) pic->cur_frame = frame_idx;
}