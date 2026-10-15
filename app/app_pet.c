/* =============================================================
 * DSH-Pet 大肥鱼桌面宠物 (静态 App - 大尺寸 & 零 Heap 内存池版)
 *
 * 特性:
 *   1. 直接借用 app_loader_get_ram_pool() (32K/48K/60K 池)，0 字节 malloc；
 *   2. 支持 220x220 甚至 280x280 超大尺寸动画分条流式直推 LCD；
 *   3. 取消重力下落：手指拖到屏幕任意位置松手即固定驻留；
 *   4. 短按摸头触发互动动作，长按拖拽移动，右下角一键切换动作。
 * ============================================================= */

#include "app_pet.h"
#include "app_loader.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "lcd.h"
#include "ff.h"
#include "tf.h"
#include "ui_draw.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PET_MAX_ANIMS    16
#define PET_FRAME_MS     80u     /* 12.5 FPS 动画步进周期 */

typedef struct {
    uint32_t start_frame;
    uint32_t frame_count;
} pet_anim_t;

typedef struct {
    /* 必须位于首位：供 app_manager 读取脏矩形 */
    ui_rect_t   dirty_rect;

    FATFS       fs;
    FIL         pak_file;
    bool        fs_mounted;
    bool        pak_open;
    bool        need_bg_clear;

    uint16_t    anim_count;
    uint16_t    pet_w;
    uint16_t    pet_h;
    uint16_t    max_band_rows;   /* 单次从 app_loader 内存池最多能装载的行数 */
    uint32_t    header_bytes;
    uint32_t    frame_bytes;
    pet_anim_t  anims[PET_MAX_ANIMS];

    uint8_t     cur_anim;
    uint16_t    cur_frame;
    uint8_t     idle_loops;
    uint32_t    last_frame_ms;

    /* 坐标与拖拽状态 (无重力下落，指哪停哪) */
    int16_t     x, y;
    int16_t     prev_x, prev_y;
    bool        is_dragging;
    bool        was_touching;
    int16_t     drag_off_x, drag_off_y;
    uint32_t    touch_down_ms;
} app_pet_state_t;

static app_pet_state_t *s_p = NULL;
static uint16_t        *s_pool_buf = NULL;
static app_t            s_app_inst;

/* ===================== 局部残影擦除 (拖拽移动时涂黑旧边缘) ===================== */

static void pet_clear_trail(int16_t old_x, int16_t old_y, int16_t new_x, int16_t new_y,
                            uint16_t w, uint16_t h)
{
    if (old_x == new_x && old_y == new_y) return;

    if (new_x >= old_x + w || new_x + w <= old_x ||
        new_y >= old_y + h || new_y + h <= old_y) {
        LCD_FillRect(old_x, old_y, w, h, 0x0000);
        return;
    }

    if (new_x > old_x) {
        LCD_FillRect(old_x, old_y, (uint16_t)(new_x - old_x), h, 0x0000);
    } else if (new_x < old_x) {
        LCD_FillRect((uint16_t)(new_x + w), old_y, (uint16_t)(old_x - new_x), h, 0x0000);
    }

    if (new_y > old_y) {
        LCD_FillRect(old_x, old_y, w, (uint16_t)(new_y - old_y), 0x0000);
    } else if (new_y < old_y) {
        LCD_FillRect(old_x, (uint16_t)(new_y + h), w, (uint16_t)(old_y - new_y), 0x0000);
    }
}

static void pet_set_anim(uint8_t anim_idx)
{
    if (!s_p || s_p->anim_count == 0) return;
    if (anim_idx >= s_p->anim_count) anim_idx = 0;
    s_p->cur_anim  = anim_idx;
    s_p->cur_frame = 0;
}
/* ===================== 分条读取并推送整帧 (加 LCD_WaitDMA 防止覆盖) ===================== */

static void pet_render_current_frame(void)
{
    if (!s_p || !s_p->pak_open || !s_pool_buf) return;

    pet_anim_t *act   = &s_p->anims[s_p->cur_anim];
    uint32_t frame_id = act->start_frame + s_p->cur_frame;
    uint32_t base_off = s_p->header_bytes + frame_id * s_p->frame_bytes;

    if (f_lseek(&s_p->pak_file, base_off) != FR_OK) return;

    uint16_t rows_left = s_p->pet_h;
    uint16_t y_offset  = 0;

    while (rows_left > 0) {
        uint16_t chunk_rows  = (rows_left > s_p->max_band_rows) ? s_p->max_band_rows : rows_left;
        uint32_t chunk_bytes = (uint32_t)chunk_rows * (uint32_t)s_p->pet_w * 2u;
        UINT br = 0;

        /* ⚡ 关键：等待上一次 DMA 传输彻底完成，再去 f_read 覆盖 s_pool_buf！ */
        LCD_WaitDMA();

        if (f_read(&s_p->pak_file, s_pool_buf, chunk_bytes, &br) != FR_OK || br != chunk_bytes) {
            break;
        }

        LCD_PushSubTile(s_p->x, (uint16_t)(s_p->y + y_offset),
                        s_p->pet_w, chunk_rows,
                        s_pool_buf, s_p->pet_w, 0, 0);

        y_offset  += chunk_rows;
        rows_left -= chunk_rows;
    }
    LCD_WaitDMA();
}

/* ===================== 加载 .pak 资源头 ===================== */

static bool pet_open_pak(void)
{
    if (f_open(&s_p->pak_file, "0:/dsh_pet.pak", FA_READ) != FR_OK) {
        if (f_open(&s_p->pak_file, "0:/download/dsh_pet.pak", FA_READ) != FR_OK) {
            return false;
        }
    }

    uint8_t hdr[12];
    UINT br = 0;
    if (f_read(&s_p->pak_file, hdr, 12, &br) != FR_OK || br != 12) {
        f_close(&s_p->pak_file);
        return false;
    }

    if (memcmp(hdr, "DPET", 4) != 0) {
        f_close(&s_p->pak_file);
        return false;
    }

    s_p->anim_count = *(uint16_t *)&hdr[4];
    s_p->pet_w      = *(uint16_t *)&hdr[6];
    s_p->pet_h      = *(uint16_t *)&hdr[8];

    /* ⚡ 放宽尺寸上限至全屏工作区大小 (480 x 288) */
    if (s_p->anim_count == 0 || s_p->anim_count > PET_MAX_ANIMS ||
        s_p->pet_w == 0 || s_p->pet_w > LCD_WIDTH ||
        s_p->pet_h == 0 || s_p->pet_h > (LCD_HEIGHT - 32)) {
        f_close(&s_p->pak_file);
        return false;
    }

    uint32_t table_bytes = (uint32_t)s_p->anim_count * sizeof(pet_anim_t);
    if (f_read(&s_p->pak_file, s_p->anims, table_bytes, &br) != FR_OK || br != table_bytes) {
        f_close(&s_p->pak_file);
        return false;
    }

    s_p->header_bytes = 12u + table_bytes;
    s_p->frame_bytes  = (uint32_t)s_p->pet_w * (uint32_t)s_p->pet_h * 2u;

    /* ⚡ 直接借用 app_loader 闲置的静态内存池，0 字节 malloc！ */
    s_pool_buf = (uint16_t *)app_loader_get_ram_pool();
    uint32_t pool_sz = app_loader_get_ram_pool_size();
    if (!s_pool_buf || pool_sz < (uint32_t)s_p->pet_w * 2u) {
        f_close(&s_p->pak_file);
        return false;
    }

    s_p->max_band_rows = (uint16_t)(pool_sz / ((uint32_t)s_p->pet_w * 2u));
    if (s_p->max_band_rows > s_p->pet_h) {
        s_p->max_band_rows = s_p->pet_h;
    }

    s_p->pak_open = true;
    return true;
}

/* ===================== 生命周期实现 ===================== */

static void app_pet_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void app_pet_enter(app_t *self)
{
    s_p = (app_pet_state_t *)g_static_app_ram;
    memset(s_p, 0, sizeof(*s_p));
    self->user_data = s_p;

    TF_Init();
    if (f_mount(&s_p->fs, "0:", 1) == FR_OK) {
        s_p->fs_mounted = true;
        pet_open_pak();
    }

    uint16_t w = s_p->pak_open ? s_p->pet_w : 200;
    uint16_t h = s_p->pak_open ? s_p->pet_h : 200;

    /* 默认在下方工作区 (480 x 288) 完美居中显示 */
    s_p->x = (LCD_WIDTH - (int16_t)w) / 2;
    s_p->y = 32 + (LCD_HEIGHT - 32 - (int16_t)h) / 2;
    s_p->prev_x = s_p->x;
    s_p->prev_y = s_p->y;

    s_p->need_bg_clear = true;
    s_p->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void app_pet_exit(app_t *self)
{
    if (s_p) {
        if (s_p->pak_open) {
            f_close(&s_p->pak_file);
            s_p->pak_open = false;
        }
        if (s_p->fs_mounted) {
            f_mount(NULL, "0:", 1);
            s_p->fs_mounted = false;
        }
    }
    s_pool_buf = NULL;
    self->user_data = NULL;
}
static void app_pet_update(app_t *self, uint32_t now_tick)
{
    (void)self;
    if (!s_p) return;

    s_p->dirty_rect = ui_rect_make(0, 0, 0, 0);

    if (s_p->need_bg_clear) {
        s_p->need_bg_clear = false;
        s_p->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return;
    }

    if (!s_p->pak_open || !s_pool_buf) return;

    /* 1. 检查拖拽是否改变了坐标，擦除旧位置残影 */
    bool moved = (s_p->x != s_p->prev_x || s_p->y != s_p->prev_y);
    if (moved) {
        pet_clear_trail(s_p->prev_x, s_p->prev_y, s_p->x, s_p->y, s_p->pet_w, s_p->pet_h);
        s_p->prev_x = s_p->x;
        s_p->prev_y = s_p->y;
    }

    /* 2. 按 80ms (12.5 FPS) 步进读取下一帧，或拖拽移动时立即重绘 */
    if (now_tick - s_p->last_frame_ms >= PET_FRAME_MS) {
        s_p->last_frame_ms = now_tick;

        pet_render_current_frame();

        pet_anim_t *act = &s_p->anims[s_p->cur_anim];
        s_p->cur_frame++;
        if (s_p->cur_frame >= act->frame_count) {
            s_p->cur_frame = 0;

            if (s_p->is_dragging) {
                /* 拖拽中保持拖拽动作循环 */
            } else {
                /* ⚡ 严格按顺序循环播放打包进 pak 里的所有动作 */
                s_p->cur_anim = (uint8_t)((s_p->cur_anim + 1) % s_p->anim_count);
            }
        }
    } else if (moved) {
        /* 拖拽过程中立刻在新位置重推画面 */
        pet_render_current_frame();
    }

    /* 保活 16.6ms 主循环采样率 */
    s_p->dirty_rect = ui_rect_make(0, 32, 1, 1);
}
static bool app_pet_process_touch(app_t *self, bool is_touching, int16_t tx,
                                  int16_t ty, uint32_t now_tick)
{
    (void)self;
    if (!s_p || !s_p->pak_open) return false;

    if (is_touching && !s_p->was_touching) {
        /* 初次按下：检查是否点中右下角切动作按钮，或点中了桌宠本体 */
        if (tx > 380 && ty > 270) {
            pet_set_anim((s_p->cur_anim + 1) % s_p->anim_count);
        } else if (tx >= s_p->x && tx < s_p->x + (int16_t)s_p->pet_w &&
                   ty >= s_p->y && ty < s_p->y + (int16_t)s_p->pet_h)
        {
            s_p->is_dragging   = true;
            s_p->drag_off_x    = tx - s_p->x;
            s_p->drag_off_y    = ty - s_p->y;
            s_p->touch_down_ms = now_tick;

            if (s_p->anim_count > 2) {
                pet_set_anim(2);
            }
        }
    } else if (is_touching && s_p->is_dragging) {
        /* 持续按住拖拽：更新桌宠位置，松手后直接停留在当前位置，不再掉落！ */
        int16_t nx = tx - s_p->drag_off_x;
        int16_t ny = ty - s_p->drag_off_y;

        if (nx < 0) nx = 0;
        if (nx > LCD_WIDTH - (int16_t)s_p->pet_w) nx = LCD_WIDTH - (int16_t)s_p->pet_w;
        if (ny < 32) ny = 32;
        if (ny > LCD_HEIGHT - (int16_t)s_p->pet_h) ny = LCD_HEIGHT - (int16_t)s_p->pet_h;

        s_p->x = nx;
        s_p->y = ny;
    } else if (!is_touching && s_p->is_dragging) {
        /* 松开手指：短按 (< 250ms) 判定为点击互动，长按拖拽则直接原地待机 */
        s_p->is_dragging = false;
        if (now_tick - s_p->touch_down_ms < 250 && s_p->anim_count > 1) {
            pet_set_anim(1);
        } else {
            pet_set_anim(0);
        }
    }

    s_p->was_touching = is_touching;
    return false;
}

static void app_pet_draw(app_t *self, ui_tile_t *tile)
{
    (void)self;
    if (!tile || !s_p) return;

    if (tile->rect.w == 1 && tile->rect.h == 1) {
        ui_draw_fill_rect(tile, &tile->rect, 0x0000);
        return;
    }

    ui_draw_fill_rect(tile, &tile->rect, 0x0000);

    if (!s_p->pak_open) {
        ui_draw_string(tile, 60, 130, "未找到 0:/dsh_pet.pak 动画包", FONT_20, 0xF800, 0x0000);
        ui_draw_string(tile, 60, 165, "请运行 make_pet_pak.py 生成并拷入 TF 卡", FONT_16, 0xC618, 0x0000);
    } else {
        ui_draw_string(tile, 388, 294, "[切换动作]", FONT_16, 0x4208, 0x0000);
    }
}

app_t *app_pet_get_instance(void)
{
    s_app_inst.id            = APP_ID_CUSTOM_0 + 6;
    s_app_inst.name          = "电子桌宠";
    s_app_inst.user_data     = g_static_app_ram;
    s_app_inst.init          = app_pet_init;
    s_app_inst.enter         = app_pet_enter;
    s_app_inst.exit          = app_pet_exit;
    s_app_inst.update        = app_pet_update;
    s_app_inst.process_touch = app_pet_process_touch;
    s_app_inst.draw          = app_pet_draw;
    return &s_app_inst;
}