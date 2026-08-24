#ifndef UI_PICTURE_H
#define UI_PICTURE_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_geometry.h"
#include "ui_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PIC_MODE_STATIC = 0, // 静态单图模式 (JPG / PNG)
    UI_PIC_MODE_ANIM        // 多帧动图模式 (GIF)
} ui_pic_mode_t;

typedef struct ui_picture ui_picture_t;
typedef void (*ui_pic_click_cb_t)(ui_picture_t *pic, void *user_data);

struct ui_picture {
    ui_rect_t           rect;           // 显示坐标与尺寸 (X, Y, W, H)
    ui_pic_mode_t       mode;           // 静态 / 动画模式
    
    const uint16_t     *static_bitmap;  // 静态图数据指针
    const uint16_t    **anim_frames;    // 动图帧指针数组
    uint8_t             frame_count;    // 总帧数 (静态图为 1)
    uint8_t             cur_frame;      // 当前显示帧索引
    uint32_t            frame_delay_ms; // 帧间隔时间 (ms)
    uint32_t            last_tick;      // 上次切帧时间戳
    bool                is_playing;     // 动图播放状态
    bool                loop;           // 是否循环播放

    bool                has_trans;      // 是否开启透明色抠图 (PNG 镂空)
    uint16_t            trans_color;    // 透传过滤色 (如 0x0000)

    ui_pic_click_cb_t   on_click;       // 点击事件回调
    void               *user_data;
};

// 静态图初始化
void ui_picture_init_static(ui_picture_t *pic, int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t *bitmap, bool has_trans, uint16_t trans_color,
                            ui_pic_click_cb_t on_click);

// 多帧动图初始化
void ui_picture_init_anim(ui_picture_t *pic, int16_t x, int16_t y, int16_t w, int16_t h,
                          const uint16_t **frames, uint8_t frame_count, uint32_t frame_delay_ms,
                          bool has_trans, uint16_t trans_color, ui_pic_click_cb_t on_click);

// 状态机更新：动图切帧时返回 true 并输出局部脏矩形
bool ui_picture_update(ui_picture_t *pic, uint32_t now_tick, ui_rect_t *out_dirty);

// 触控处理
bool ui_picture_process_touch(ui_picture_t *pic, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);

// 切片光栅化渲染
void ui_picture_draw(ui_tile_t *tile, const ui_picture_t *pic);

// 播放控制 API
void ui_picture_play(ui_picture_t *pic);
void ui_picture_pause(ui_picture_t *pic);
void ui_picture_set_frame(ui_picture_t *pic, uint8_t frame_idx);
// Set animation loop mode. When loop=false, animation plays once and stops (CPU drops to 0).
void ui_picture_set_loop(ui_picture_t *pic, bool loop);

#ifdef __cplusplus
}
#endif

#endif // UI_PICTURE_H