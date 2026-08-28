#include "app_audio.h"
#include "audio_i2s.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_switch.h"
#include "ui_tabview.h"
#include "ui_stepper.h"
#include "ui_file_browser.h"
#include "ui_progressbar.h"
#include "lcd.h"
#include "chinese.h"
#include "ff.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

enum {
    TAB_AUDIO_SD_PLAY = 0, // SD 卡音乐播放
    TAB_AUDIO_SYNTH   = 1, // 纯音/波形发生器
    TAB_AUDIO_MELODY  = 2, // 8-Bit 经典音乐试听
    TAB_AUDIO_PINOUT  = 3, // MAX98357A 接线说明
    TAB_AUDIO_COUNT   = 4
};

static const char *s_audio_tabs[] = {
    "SD卡音乐",
    "波形发生",
    "音乐试听",
    "接线说明"
};

typedef struct {
    ui_rect_t          dirty_rect;     // ⚠️ 必须位于第 1 位
    ui_tabview_t       tabview;
    uint8_t            last_tab_idx;

    // Tab 0: SD 卡音乐播放器
    FATFS              fs;
    bool               fs_mounted;
    char               current_path[256];
    ui_file_browser_t  file_browser;
    ui_btn_t           btn_fs_refresh;
    ui_progressbar_t   prog_bar;
    ui_btn_t           btn_wav_pause;
    ui_btn_t           btn_wav_stop;
    ui_rect_t          play_card;

    // Tab 1: 发生器组件
    bool               synth_enabled;
    int32_t            synth_freq;
    int32_t            synth_vol;
    uint8_t            wave_idx;
    ui_rect_t          synth_card;
    ui_switch_t        sw_synth;
    ui_stepper_t       stepper_freq;
    ui_stepper_t       stepper_vol;
    ui_btn_t           btn_wave_sine;
    ui_btn_t           btn_wave_square;
    ui_btn_t           btn_wave_tri;
    ui_btn_t           btn_wave_noise;

    // Tab 2: 旋律试听
    bool               music_playing;
    uint8_t            cur_song_idx;
    ui_rect_t          music_card;
    ui_btn_t           btn_song_mario;
    ui_btn_t           btn_song_tetris;
    ui_btn_t           btn_music_stop;
} app_audio_data_t;

static app_audio_data_t s_app_audio_data;
static app_t            s_app_audio;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 目录加载 ================= */

static void load_sd_directory(app_audio_data_t *data, const char *path) {
    ui_file_browser_clear(&data->file_browser);

    if (!data->fs_mounted) {
        if (f_mount(&data->fs, "0:", 1) != FR_OK) return;
        data->fs_mounted = true;
    }

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK) return;

    if (strcmp(path, "0:") != 0 && strcmp(path, "0:/") != 0) {
        ui_file_browser_add_item(&data->file_browser, "..", 0, true);
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;
        if (is_dir || strstr(fno.fname, ".wav") || strstr(fno.fname, ".WAV")) {
            ui_file_browser_add_item(&data->file_browser, fno.fname, (uint32_t)fno.fsize, is_dir);
        }
    }
    f_closedir(&dir);
}

/* ================= 交互回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_audio_data_t *data = (app_audio_data_t *)user_data;

    audio_i2s_stop_melody();
    data->music_playing = false;

    if (new_tab == TAB_AUDIO_SD_PLAY) {
        load_sd_directory(data, data->current_path);
    } else if (new_tab == TAB_AUDIO_SYNTH) {
        audio_i2s_stop_wav();
        audio_i2s_set_freq((float)data->synth_freq);
        audio_i2s_set_volume((uint8_t)data->synth_vol);
        audio_i2s_set_wave_type((audio_wave_type_t)data->wave_idx);
        audio_i2s_set_enabled(data->synth_enabled);
    } else {
        audio_i2s_set_enabled(false);
    }
}

static void on_file_click(ui_file_browser_t *fb, int8_t index, const ui_file_item_t *item, void *user_data) {
    (void)fb; (void)index;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    const char *name = item->name;

    if (item->type == UI_FILE_TYPE_DIR) {
        if (strcmp(name, "..") == 0) {
            char *slash = strrchr(data->current_path, '/');
            if (slash && slash != data->current_path + 2) *slash = '\0';
            else strcpy(data->current_path, "0:/");
        } else {
            if (data->current_path[strlen(data->current_path) - 1] != '/') strcat(data->current_path, "/");
            strcat(data->current_path, name);
        }
        load_sd_directory(data, data->current_path);
    } else {
        char full_path[280];
        if (strcmp(data->current_path, "0:/") == 0) {
            snprintf(full_path, sizeof(full_path), "0:/%s", name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", data->current_path, name);
        }
        audio_i2s_play_wav(full_path);
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_wav_pause_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    const wav_info_t *info = audio_i2s_get_wav_info();
    if (info->is_playing) {
        audio_i2s_pause_wav(!info->is_paused);
        data->btn_wav_pause.text = info->is_paused ? "恢复播放" : "暂停播放";
        data->dirty_rect = data->tabview.content_rect;
    }
}

static void on_wav_stop_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    audio_i2s_stop_wav();
    data->dirty_rect = data->tabview.content_rect;
}

static void on_synth_switch(ui_switch_t *sw, bool state, void *user_data) {
    (void)sw;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    data->synth_enabled = state;
    audio_i2s_set_enabled(state);
    data->dirty_rect = data->synth_card;
}

static void on_freq_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    data->synth_freq = val;
    audio_i2s_set_freq((float)val);
    data->dirty_rect = data->synth_card;
}

static void on_vol_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    data->synth_vol = val;
    audio_i2s_set_volume((uint8_t)val);
    data->dirty_rect = data->synth_card;
}

static void on_wave_btn_click(ui_btn_t *btn, void *user_data) {
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    if (btn == &data->btn_wave_sine)     data->wave_idx = WAVE_SINE;
    if (btn == &data->btn_wave_square)   data->wave_idx = WAVE_SQUARE;
    if (btn == &data->btn_wave_tri)      data->wave_idx = WAVE_TRIANGLE;
    if (btn == &data->btn_wave_noise)    data->wave_idx = WAVE_NOISE;
    audio_i2s_set_wave_type((audio_wave_type_t)data->wave_idx);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_song_click(ui_btn_t *btn, void *user_data) {
    app_audio_data_t *data = (app_audio_data_t *)user_data;
    audio_i2s_set_wave_type(WAVE_SQUARE);
    audio_i2s_set_volume((uint8_t)data->synth_vol);

    if (btn == &data->btn_song_mario) {
        data->cur_song_idx = 0;
        audio_i2s_play_melody(0);
        data->music_playing = true;
    } else if (btn == &data->btn_song_tetris) {
        data->cur_song_idx = 1;
        audio_i2s_play_melody(1);
        data->music_playing = true;
    } else if (btn == &data->btn_music_stop) {
        audio_i2s_stop_melody();
        data->music_playing = false;
    }
    data->dirty_rect = data->tabview.content_rect;
}

/* ================= 生命周期管理 ================= */

static void app_audio_init(app_t *self) {
    self->user_data = &s_app_audio_data;
    app_audio_data_t *data = &s_app_audio_data;
    memset(data, 0, sizeof(app_audio_data_t));

    strcpy(data->current_path, "0:/");
    data->dirty_rect    = ui_rect_make(0, 0, 0, 0);
    data->last_tab_idx  = 0;
    data->synth_freq    = 440;
    data->synth_vol     = 70;
    data->wave_idx      = WAVE_SINE;

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_audio_tabs, TAB_AUDIO_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0: SD 卡音乐播放器
    ui_file_browser_init(&data->file_browser, 120, 40, 350, 150, on_file_click);
    data->file_browser.user_data = data;

    data->play_card = ui_rect_make(120, 196, 350, 114);
    ui_progressbar_init(&data->prog_bar, 130, 246, 330, 14, false);
    ui_progressbar_set_progress(&data->prog_bar, 0);

    ui_btn_init(&data->btn_wav_pause, 130, 268, 160, 34, "暂停播放", on_wav_pause_click);
    data->btn_wav_pause.user_data = data; data->btn_wav_pause.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wav_pause, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wav_stop, 300, 268, 160, 34, "停止播放", on_wav_stop_click);
    data->btn_wav_stop.user_data = data; data->btn_wav_stop.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wav_stop, 0xF800, 0xB000, 0xFFFF);

    // Tab 1: 波形发生器
    data->synth_card = ui_rect_make(120, 40, 350, 76);
    ui_switch_init(&data->sw_synth, 396, 46, 64, 32, false, on_synth_switch);
    data->sw_synth.user_data = data;

    ui_stepper_init(&data->stepper_freq, 120, 126, 210, 36, 50, 4000, 440, 50, "Hz", on_freq_change);
    ui_stepper_set_prefix(&data->stepper_freq, "频率:");
    data->stepper_freq.user_data = data;

    ui_stepper_init(&data->stepper_vol, 338, 126, 132, 36, 0, 100, 70, 5, "%", on_vol_change);
    ui_stepper_set_prefix(&data->stepper_vol, "音量:");
    data->stepper_vol.user_data = data;

    ui_btn_init(&data->btn_wave_sine,   120, 174, 82, 36, "正弦波", on_wave_btn_click);
    data->btn_wave_sine.user_data = data; data->btn_wave_sine.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wave_sine, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_square, 208, 174, 82, 36, "方波",   on_wave_btn_click);
    data->btn_wave_square.user_data = data; data->btn_wave_square.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wave_square, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_tri,    296, 174, 82, 36, "三角波", on_wave_btn_click);
    data->btn_wave_tri.user_data = data; data->btn_wave_tri.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wave_tri, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_wave_noise,  384, 174, 86, 36, "白噪声", on_wave_btn_click);
    data->btn_wave_noise.user_data = data; data->btn_wave_noise.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_wave_noise, 0x03DF, 0x0275, 0xFFFF);

    // Tab 2: 旋律试听
    data->music_card = ui_rect_make(120, 40, 350, 110);
    ui_btn_init(&data->btn_song_mario,  120, 166, 170, 40, "超级马里奥 BGM", on_song_click);
    data->btn_song_mario.user_data = data; data->btn_song_mario.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_song_mario, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_song_tetris, 298, 166, 172, 40, "俄罗斯方块 BGM", on_song_click);
    data->btn_song_tetris.user_data = data; data->btn_song_tetris.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_song_tetris, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_music_stop,  120, 220, 350, 38, "停止播放", on_song_click);
    data->btn_music_stop.user_data = data; data->btn_music_stop.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_music_stop, 0xF800, 0xB000, 0xFFFF);

    audio_i2s_pins_safe();
}

static void app_audio_enter(app_t *self) {
    app_audio_data_t *data = (app_audio_data_t *)self->user_data;
    data->dirty_rect   = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = data->tabview.active_tab_idx;

    audio_i2s_init();
    audio_i2s_set_enabled(false);
    audio_i2s_stop_melody();
    audio_i2s_stop_wav();

    load_sd_directory(data, data->current_path);
}

static void app_audio_exit(app_t *self) {
    app_audio_data_t *data = (app_audio_data_t *)self->user_data;
    audio_i2s_stop_melody();
    audio_i2s_stop_wav();
    audio_i2s_set_enabled(false);
    audio_i2s_deinit();

    if (data->fs_mounted) {
        f_mount(NULL, "0:", 1);
        data->fs_mounted = false;
    }
}
static void app_audio_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_audio_data_t *data = (app_audio_data_t *)self->user_data;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);

    // ⚡ 1. 优先读取 SD 卡填满音频缓冲区 (无任何阻塞)
    audio_i2s_update_wav();

    // ⚡ 2. 只有秒数真正跳变时才标记重绘，绝不在每一毫秒死循环重绘
    const wav_info_t *info = audio_i2s_get_wav_info();
    if (info->is_playing) {
        static uint32_t s_last_sec = 0xFFFFFFFF;
        if (info->cur_sec != s_last_sec) {
            s_last_sec = info->cur_sec;
            uint8_t prog = (info->total_samples > 0) ? (uint8_t)((uint64_t)info->played_samples * 100 / info->total_samples) : 0;
            ui_progressbar_set_progress(&data->prog_bar, prog);
            data->dirty_rect = data->play_card; // 仅更新播放卡片区域
        }
    }

    if (data->music_playing) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        audio_i2s_update_playback(now_ms);
    }
}
static bool app_audio_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_audio_data_t *data = (app_audio_data_t *)self->user_data;

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    switch (data->tabview.active_tab_idx) {
        case TAB_AUDIO_SD_PLAY:
            if (ui_file_browser_process_touch(&data->file_browser, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, data->file_browser.rect);
                return true;
            }
            if (ui_btn_process_touch(&data->btn_wav_pause, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wav_stop, is_touching, tx, ty, now_tick)) return true;
            break;

        case TAB_AUDIO_SYNTH:
            if (ui_switch_process_touch(&data->sw_synth, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_switch_get_dirty_rect(&data->sw_synth));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_freq, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_freq));
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_vol, is_touching, tx, ty, now_tick)) {
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_vol));
                return true;
            }
            if (ui_btn_process_touch(&data->btn_wave_sine, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_square, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_tri, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_wave_noise, is_touching, tx, ty, now_tick)) return true;
            break;

        case TAB_AUDIO_MELODY:
            if (ui_btn_process_touch(&data->btn_song_mario, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_song_tetris, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_music_stop, is_touching, tx, ty, now_tick)) return true;
            break;
    }

    return (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

static void app_audio_draw(app_t *self, ui_tile_t *tile) {
    app_audio_data_t *data = (app_audio_data_t *)self->user_data;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case TAB_AUDIO_SD_PLAY: {
            ui_file_browser_draw(tile, &data->file_browser);

            if (ui_rect_intersect(&tile->rect, &data->play_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->play_card, 0x07FF);

                const wav_info_t *info = audio_i2s_get_wav_info();
                if (info->is_playing) {
                    snprintf(str_buf, sizeof(str_buf), "%s: %s", info->is_paused ? "已暂停" : "正在播放", info->filename);
                    ui_draw_string(tile, 130, 202, str_buf, FONT_16, info->is_paused ? 0xFA00 : 0x05E0, 0x10A2);

                    snprintf(str_buf, sizeof(str_buf), "%luHz / %s | %02lu:%02lu / %02lu:%02lu",
                             info->sample_rate, (info->channels == 2) ? "立体声" : "单声道",
                             info->cur_sec / 60, info->cur_sec % 60,
                             info->duration_sec / 60, info->duration_sec % 60);
                    ui_draw_string(tile, 130, 224, str_buf, FONT_16, 0xCE59, 0x10A2);
                } else {
                    ui_draw_string(tile, 130, 204, "SD 卡播放器 [请在上方列表点击 .wav 播放]", FONT_16, 0x07FF, 0x10A2);
                    ui_draw_string(tile, 130, 226, "支持 16-Bit PCM 格式无损音频", FONT_16, 0x8410, 0x10A2);
                }
            }

            ui_progressbar_draw(tile, &data->prog_bar);
            ui_btn_draw(tile, &data->btn_wav_pause);
            ui_btn_draw(tile, &data->btn_wav_stop);
            break;
        }

        case TAB_AUDIO_SYNTH: {
            if (ui_rect_intersect(&tile->rect, &data->synth_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->synth_card, 0x07FF);

                ui_draw_string(tile, 132, 48, "MAX98357A I2S 音频发生器", FONT_16, 0x07FF, 0x10A2);
                const char *wave_names[] = {"正弦波", "方波", "三角波", "白噪声"};
                snprintf(str_buf, sizeof(str_buf), "输出: %ldHz | 音量: %ld%% | 波形: %s",
                         (long)data->synth_freq, (long)data->synth_vol, wave_names[data->wave_idx]);
                ui_draw_string(tile, 132, 72, str_buf, FONT_16, data->synth_enabled ? 0x05E0 : 0x8410, 0x10A2);
            }

            ui_switch_draw(tile, &data->sw_synth);
            ui_stepper_draw(tile, &data->stepper_freq);
            ui_stepper_draw(tile, &data->stepper_vol);
            ui_btn_draw(tile, &data->btn_wave_sine);
            ui_btn_draw(tile, &data->btn_wave_square);
            ui_btn_draw(tile, &data->btn_wave_tri);
            ui_btn_draw(tile, &data->btn_wave_noise);
            break;
        }

        case TAB_AUDIO_MELODY: {
            if (ui_rect_intersect(&tile->rect, &data->music_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->music_card, 0xC618);
                ui_draw_string(tile, 132, 50, "8-Bit 复古音乐旋律试听", FONT_16, 0x1A7B, 0xEF7D);
                if (data->music_playing) {
                    const char *song_names[] = {"超级马里奥主题曲", "俄罗斯方块 Korobeiniki"};
                    snprintf(str_buf, sizeof(str_buf), "正在播放: %s", song_names[data->cur_song_idx]);
                    ui_draw_string(tile, 132, 78, str_buf, FONT_16, 0x05E0, 0xEF7D);
                } else {
                    ui_draw_string(tile, 132, 78, "当前状态: 停止播放", FONT_16, 0x8410, 0xEF7D);
                }
            }
            ui_btn_draw(tile, &data->btn_song_mario);
            ui_btn_draw(tile, &data->btn_song_tetris);
            ui_btn_draw(tile, &data->btn_music_stop);
            break;
        }

        case TAB_AUDIO_PINOUT: {
            ui_rect_t card = ui_rect_make(120, 40, 350, 240);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);
                ui_draw_string(tile, 132, 48, "MAX98357A I2S 功放模块接线:", FONT_16, 0x03DF, 0xEF7D);
                ui_draw_string(tile, 132, 74, "BCLK -> H1 Pin 13 (GP20 位时钟)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 132, 98, "LRC  -> H1 Pin 14 (GP21 左右声道/WS)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 132, 122, "DIN  -> H1 Pin 15 (GP22 PCM数据线)", FONT_16, 0x1A7B, 0xEF7D);
                ui_draw_string(tile, 132, 148, "VIN  -> H1 Pin 1  (5V 供电输出3W)", FONT_16, 0xF800, 0xEF7D);
                ui_draw_string(tile, 132, 172, "GND  -> H1 Pin 16 (系统电源地)", FONT_16, 0x0000, 0xEF7D);
                ui_draw_string(tile, 132, 202, "SD_MODE -> 接 H1 Pin 2 (3.3V 唤醒)", FONT_16, 0x05E0, 0xEF7D);
                ui_draw_string(tile, 132, 226, "GAIN    -> 接 H1 Pin 16 (GND +15dB)", FONT_16, 0x05E0, 0xEF7D);
            }
            break;
        }
    }
}

app_t *app_audio_get_instance(void) {
    s_app_audio.id            = APP_ID_CUSTOM_0 + 6;
    s_app_audio.name          = "I2S音频";
    s_app_audio.init          = app_audio_init;
    s_app_audio.enter         = app_audio_enter;
    s_app_audio.exit          = app_audio_exit;
    s_app_audio.update        = app_audio_update;
    s_app_audio.process_touch = app_audio_process_touch;
    s_app_audio.draw          = app_audio_draw;
    return &s_app_audio;
}