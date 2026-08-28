#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_PIN_BCLK        20  // H1 Pin 13 (GP20 位时钟)
#define I2S_PIN_LRCK        21  // H1 Pin 14 (GP21 左右声道/WS)
#define I2S_PIN_DOUT        22  // H1 Pin 15 (GP22 PCM数据)

#define I2S_SAMPLE_RATE     48000
// ⚡ 扩大至 1024 采样点 (4KB)，彻底杜绝 SD 卡读取断流
#define I2S_BUFFER_SAMPLES  1024

typedef enum {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_TRIANGLE,
    WAVE_NOISE,
    WAVE_COUNT
} audio_wave_type_t;

typedef struct {
    bool     is_playing;
    bool     is_paused;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t total_samples;
    uint32_t played_samples;
    uint32_t duration_sec;
    uint32_t cur_sec;
    char     filename[64];
} wav_info_t;

void audio_i2s_pins_safe(void);
void audio_i2s_init(void);
void audio_i2s_deinit(void);

void audio_i2s_set_enabled(bool enabled);
bool audio_i2s_is_enabled(void);
void audio_i2s_set_freq(float freq_hz);
void audio_i2s_set_volume(uint8_t volume_pct);
void audio_i2s_set_wave_type(audio_wave_type_t type);

void audio_i2s_play_melody(uint8_t melody_idx);
void audio_i2s_stop_melody(void);
void audio_i2s_update_playback(uint32_t now_ms);

bool audio_i2s_play_wav(const char *filepath);
void audio_i2s_stop_wav(void);
void audio_i2s_pause_wav(bool pause);
void audio_i2s_update_wav(void);
const wav_info_t *audio_i2s_get_wav_info(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_I2S_H