#include "audio_i2s.h"
#include "ff.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ⚡ 标准 Philips I2S 16-bit 汇编 (GP20:BCLK, GP21:LRCK, GP22:DOUT)
static uint16_t audio_i2s_instructions[] = {
    0x6001, // 0: out pins, 1     side 0b00 (LRCK=0, BCLK=0)
    0x0840, // 1: jmp x--, 0      side 0b01 (LRCK=0, BCLK=1)
    0x7001, // 2: out pins, 1     side 0b10 (LRCK=1, BCLK=0)
    0xf82e, // 3: set x, 14       side 0b11 (LRCK=1, BCLK=1)
    0x7001, // 4: out pins, 1     side 0b10 (LRCK=1, BCLK=0)
    0x1844, // 5: jmp x--, 4      side 0b11 (LRCK=1, BCLK=1)
    0x6001, // 6: out pins, 1     side 0b00 (LRCK=0, BCLK=0)
    0xe82e  // 7: set x, 14       side 0b01 (LRCK=0, BCLK=1)
};

static struct pio_program audio_i2s_program = {
    .instructions = audio_i2s_instructions,
    .length = 8,
    .origin = -1,
};

static PIO  s_pio = pio1;
static uint s_sm  = 0;
static uint s_dma_chan_a;
static uint s_dma_chan_b;

static uint32_t s_audio_buf_a[I2S_BUFFER_SAMPLES];
static uint32_t s_audio_buf_b[I2S_BUFFER_SAMPLES];
static volatile bool s_need_fill_a = false;
static volatile bool s_need_fill_b = false;

static bool              s_enabled = false;
static float             s_target_freq = 440.0f;
static uint8_t           s_volume = 70;
static audio_wave_type_t s_wave_type = WAVE_SINE;
static float             s_phase = 0.0f;

static FIL        s_wav_fil;
static bool       s_wav_file_open = false;
static wav_info_t s_wav_info = {0};
static uint32_t   s_wav_data_start = 0;
static uint32_t   s_wav_data_bytes = 0;
static uint8_t    s_raw_pcm_temp[I2S_BUFFER_SAMPLES * 4];

typedef struct {
    uint16_t freq;
    uint16_t dur_ms;
} note_t;

static const note_t s_melody_mario[] = {
    {659, 120}, {659, 120}, {0, 120},   {659, 120}, {0, 120},   {523, 120}, {659, 120},
    {784, 240}, {0, 240},   {392, 240}, {0, 240},
    {523, 180}, {0, 60},    {392, 180}, {0, 60},    {330, 180}, {0, 60},
    {440, 120}, {494, 120}, {466, 120}, {440, 180},
    {392, 120}, {659, 120}, {784, 120}, {880, 180}, {698, 120}, {784, 120},
    {0, 0}
};

static const note_t s_melody_tetris[] = {
    {659, 200}, {494, 100}, {523, 100}, {587, 200}, {523, 100}, {494, 100},
    {440, 200}, {440, 100}, {523, 100}, {659, 200}, {587, 100}, {523, 100},
    {494, 300}, {523, 100}, {587, 200}, {659, 200},
    {523, 200}, {440, 200}, {440, 300},
    {0, 0}
};

static const note_t *s_cur_melody = NULL;
static uint16_t      s_melody_idx = 0;
static uint32_t      s_note_end_time = 0;
static void fill_pcm_buffer(uint32_t *buffer) {
    if (s_wav_info.is_playing && !s_wav_info.is_paused && s_wav_file_open) {
        // ⚡ 单声道读取 2KB (I2S_BUFFER_SAMPLES * 2)，双声道读取 4KB (I2S_BUFFER_SAMPLES * 4)
        UINT bytes_to_read = (s_wav_info.channels == 1) ? (I2S_BUFFER_SAMPLES * 2) : (I2S_BUFFER_SAMPLES * 4);
        UINT bytes_read = 0;

        FRESULT res = f_read(&s_wav_fil, s_raw_pcm_temp, bytes_to_read, &bytes_read);
        if (res != FR_OK || bytes_read == 0) {
            audio_i2s_stop_wav();
            memset(buffer, 0, sizeof(uint32_t) * I2S_BUFFER_SAMPLES);
            return;
        }

        // ⚡ 循环外预计算 Q15 定点音量 (0~32768)，消灭循环内软除法 (/ 100) 开销
        uint32_t v_q15 = ((uint32_t)s_volume * 32768u) / 100u;

        // ================= 1. 单声道极速直通 (读卡开销减半，左右声道同步输出) =================
        if (s_wav_info.channels == 1) {
            const int16_t *src = (const int16_t *)s_raw_pcm_temp;
            uint32_t sample_cnt = bytes_read >> 1;

            if (v_q15 >= 32768) {
                // 100% 音量零乘法快道
                for (uint32_t i = 0; i < sample_cnt; i++) {
                    uint32_t s = (uint16_t)src[i];
                    buffer[i] = (s << 16) | s;
                }
            } else {
                // 单周期定点硬件乘法
                for (uint32_t i = 0; i < sample_cnt; i++) {
                    int32_t val = ((int32_t)src[i] * (int32_t)v_q15) >> 15;
                    uint32_t s = (uint16_t)(int16_t)val;
                    buffer[i] = (s << 16) | s;
                }
            }

            if (sample_cnt < I2S_BUFFER_SAMPLES) {
                memset(&buffer[sample_cnt], 0, (I2S_BUFFER_SAMPLES - sample_cnt) * 4);
            }
            s_wav_info.played_samples += sample_cnt;
        } 
        // ================= 2. 立体声自动降混为单声道 (L + R)/2 (单喇叭不丢乐器细节) =================
        else {
            const int16_t *src = (const int16_t *)s_raw_pcm_temp;
            uint32_t sample_cnt = bytes_read >> 2;

            for (uint32_t i = 0; i < sample_cnt; i++) {
                int32_t mono_mix = ((int32_t)src[i * 2 + 0] + (int32_t)src[i * 2 + 1]) >> 1;
                int32_t val = (mono_mix * (int32_t)v_q15) >> 15;
                uint32_t s = (uint16_t)(int16_t)val;
                buffer[i] = (s << 16) | s;
            }

            if (sample_cnt < I2S_BUFFER_SAMPLES) {
                memset(&buffer[sample_cnt], 0, (I2S_BUFFER_SAMPLES - sample_cnt) * 4);
            }
            s_wav_info.played_samples += sample_cnt;
        }

        if (s_wav_info.sample_rate > 0) {
            s_wav_info.cur_sec = s_wav_info.played_samples / s_wav_info.sample_rate;
        }
        return;
    }

    // 纯音合成器
    if (!s_enabled || s_volume == 0 || s_target_freq < 1.0f) {
        memset(buffer, 0, sizeof(uint32_t) * I2S_BUFFER_SAMPLES);
        return;
    }

    float phase_inc = (2.0f * (float)M_PI * s_target_freq) / (float)I2S_SAMPLE_RATE;
    float amp_scale = ((float)s_volume / 100.0f) * 24000.0f;

    for (int i = 0; i < I2S_BUFFER_SAMPLES; i++) {
        float sample_val = 0.0f;
        switch (s_wave_type) {
            case WAVE_SINE:     sample_val = sinf(s_phase); break;
            case WAVE_SQUARE:   sample_val = (s_phase < (float)M_PI) ? 0.7f : -0.7f; break;
            case WAVE_TRIANGLE: sample_val = (s_phase < (float)M_PI) ? (-1.0f + 2.0f * (s_phase / (float)M_PI))
                                                                     : (3.0f - 2.0f * (s_phase / (float)M_PI)); break;
            case WAVE_NOISE:    sample_val = ((float)(rand() % 2000) - 1000.0f) / 1000.0f; break;
            default: break;
        }
        s_phase += phase_inc;
        if (s_phase >= 2.0f * (float)M_PI) s_phase -= 2.0f * (float)M_PI;

        int16_t pcm16 = (int16_t)(sample_val * amp_scale);
        uint32_t s = (uint16_t)pcm16;
        buffer[i] = (s << 16) | s;
    }
}
static void dma_i2s_irq_handler(void) {
    if (dma_hw->ints1 & (1u << s_dma_chan_a)) {
        dma_hw->ints1 = (1u << s_dma_chan_a);
        s_need_fill_a = true;
        dma_channel_set_trans_count(s_dma_chan_a, I2S_BUFFER_SAMPLES, false);
        dma_channel_set_read_addr(s_dma_chan_a, s_audio_buf_a, false);
    }
    if (dma_hw->ints1 & (1u << s_dma_chan_b)) {
        dma_hw->ints1 = (1u << s_dma_chan_b);
        s_need_fill_b = true;
        dma_channel_set_trans_count(s_dma_chan_b, I2S_BUFFER_SAMPLES, false);
        dma_channel_set_read_addr(s_dma_chan_b, s_audio_buf_b, false);
    }
}

bool audio_i2s_play_wav(const char *filepath) {
    audio_i2s_stop_wav();
    audio_i2s_stop_melody();
    s_enabled = false;

    if (f_open(&s_wav_fil, filepath, FA_READ) != FR_OK) {
        return false;
    }
    s_wav_file_open = true;

    uint8_t riff_head[12];
    UINT br = 0;
    if (f_read(&s_wav_fil, riff_head, 12, &br) != FR_OK || br < 12) {
        f_close(&s_wav_fil);
        s_wav_file_open = false;
        return false;
    }

    if (memcmp(riff_head, "RIFF", 4) != 0 || memcmp(riff_head + 8, "WAVE", 4) != 0) {
        f_close(&s_wav_fil);
        s_wav_file_open = false;
        return false;
    }

    bool fmt_found = false;
    bool data_found = false;
    uint16_t audio_fmt = 0, channels = 0, bits = 0;
    uint32_t s_rate = 0;

    while (f_tell(&s_wav_fil) < f_size(&s_wav_fil)) {
        uint8_t chunk_hdr[8];
        if (f_read(&s_wav_fil, chunk_hdr, 8, &br) != FR_OK || br < 8) break;

        uint32_t chunk_size = chunk_hdr[4] | (chunk_hdr[5] << 8) | (chunk_hdr[6] << 16) | (chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt_buf[16];
            if (f_read(&s_wav_fil, fmt_buf, 16, &br) != FR_OK || br < 16) break;

            audio_fmt = fmt_buf[0] | (fmt_buf[1] << 8);
            channels  = fmt_buf[2] | (fmt_buf[3] << 8);
            s_rate    = fmt_buf[4] | (fmt_buf[5] << 8) | (fmt_buf[6] << 16) | (fmt_buf[7] << 24);
            bits      = fmt_buf[14] | (fmt_buf[15] << 8);
            fmt_found = true;

            if (chunk_size > 16) {
                f_lseek(&s_wav_fil, f_tell(&s_wav_fil) + (chunk_size - 16));
            }
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            s_wav_data_start = (uint32_t)f_tell(&s_wav_fil);
            s_wav_data_bytes = chunk_size;
            data_found = true;
            break;
        } else {
            f_lseek(&s_wav_fil, f_tell(&s_wav_fil) + chunk_size);
        }
    }

    if (!fmt_found || !data_found || audio_fmt != 1 || bits != 16 || (channels != 1 && channels != 2) || s_rate == 0) {
        f_close(&s_wav_fil);
        s_wav_file_open = false;
        return false;
    }

    f_lseek(&s_wav_fil, s_wav_data_start);

    uint32_t f_sys = clock_get_hz(clk_sys);
    float clk_div = (float)f_sys / ((float)s_rate * 64.0f);
    pio_sm_set_clkdiv(s_pio, s_sm, clk_div);

    const char *p_name = strrchr(filepath, '/');
    strncpy(s_wav_info.filename, p_name ? p_name + 1 : filepath, sizeof(s_wav_info.filename) - 1);
    s_wav_info.sample_rate   = s_rate;
    s_wav_info.channels      = channels;
    s_wav_info.total_samples = s_wav_data_bytes / (channels * 2);
    s_wav_info.played_samples= 0;
    s_wav_info.duration_sec  = s_wav_info.total_samples / s_rate;
    s_wav_info.cur_sec       = 0;
    s_wav_info.is_playing    = true;
    s_wav_info.is_paused     = false;

    fill_pcm_buffer(s_audio_buf_a);
    fill_pcm_buffer(s_audio_buf_b);
    return true;
}

void audio_i2s_stop_wav(void) {
    if (s_wav_file_open) {
        f_close(&s_wav_fil);
        s_wav_file_open = false;
    }
    s_wav_info.is_playing = false;
    s_wav_info.is_paused  = false;
    s_wav_info.played_samples = 0;
    s_wav_info.cur_sec = 0;

    uint32_t f_sys = clock_get_hz(clk_sys);
    pio_sm_set_clkdiv(s_pio, s_sm, (float)f_sys / ((float)I2S_SAMPLE_RATE * 64.0f));
}

void audio_i2s_pause_wav(bool pause) {
    s_wav_info.is_paused = pause;
}

void audio_i2s_update_wav(void) {
    if (s_need_fill_a) {
        s_need_fill_a = false;
        fill_pcm_buffer(s_audio_buf_a);
    }
    if (s_need_fill_b) {
        s_need_fill_b = false;
        fill_pcm_buffer(s_audio_buf_b);
    }
}

const wav_info_t *audio_i2s_get_wav_info(void) {
    return &s_wav_info;
}

void audio_i2s_pins_safe(void) {
    gpio_init(I2S_PIN_BCLK); gpio_set_dir(I2S_PIN_BCLK, GPIO_OUT); gpio_put(I2S_PIN_BCLK, 0);
    gpio_init(I2S_PIN_LRCK); gpio_set_dir(I2S_PIN_LRCK, GPIO_OUT); gpio_put(I2S_PIN_LRCK, 0);
    gpio_init(I2S_PIN_DOUT); gpio_set_dir(I2S_PIN_DOUT, GPIO_OUT); gpio_put(I2S_PIN_DOUT, 0);
}

void audio_i2s_init(void) {
    audio_i2s_pins_safe();

    uint offset = pio_add_program(s_pio, &audio_i2s_program);
    s_pio->instr_mem[offset + 0] = 0x6001;
    s_pio->instr_mem[offset + 1] = 0x0840 | (offset + 0);
    s_pio->instr_mem[offset + 2] = 0x7001;
    s_pio->instr_mem[offset + 3] = 0xf82e;
    s_pio->instr_mem[offset + 4] = 0x7001;
    s_pio->instr_mem[offset + 5] = 0x1840 | (offset + 4);
    s_pio->instr_mem[offset + 6] = 0x6001;
    s_pio->instr_mem[offset + 7] = 0xe82e;

    s_sm = pio_claim_unused_sm(s_pio, true);

    pio_gpio_init(s_pio, I2S_PIN_BCLK);
    pio_gpio_init(s_pio, I2S_PIN_LRCK);
    pio_gpio_init(s_pio, I2S_PIN_DOUT);

    pio_sm_set_consecutive_pindirs(s_pio, s_sm, I2S_PIN_BCLK, 3, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 7);
    sm_config_set_out_pins(&c, I2S_PIN_DOUT, 1);
    sm_config_set_sideset_pins(&c, I2S_PIN_BCLK);
    sm_config_set_sideset(&c, 2, false, false);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    uint32_t f_sys = clock_get_hz(clk_sys);
    sm_config_set_clkdiv(&c, (float)f_sys / ((float)I2S_SAMPLE_RATE * 64.0f));

    pio_sm_init(s_pio, s_sm, offset + 7, &c);

    memset(s_audio_buf_a, 0, sizeof(s_audio_buf_a));
    memset(s_audio_buf_b, 0, sizeof(s_audio_buf_b));

    s_dma_chan_a = dma_claim_unused_channel(true);
    s_dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config ca = dma_channel_get_default_config(s_dma_chan_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, true);
    channel_config_set_write_increment(&ca, false);
    channel_config_set_dreq(&ca, pio_get_dreq(s_pio, s_sm, true));
    channel_config_set_chain_to(&ca, s_dma_chan_b);

    dma_channel_configure(s_dma_chan_a, &ca, &s_pio->txf[s_sm], s_audio_buf_a, I2S_BUFFER_SAMPLES, false);

    dma_channel_config cb = dma_channel_get_default_config(s_dma_chan_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb, true);
    channel_config_set_write_increment(&cb, false);
    channel_config_set_dreq(&cb, pio_get_dreq(s_pio, s_sm, true));
    channel_config_set_chain_to(&cb, s_dma_chan_a);

    dma_channel_configure(s_dma_chan_b, &cb, &s_pio->txf[s_sm], s_audio_buf_b, I2S_BUFFER_SAMPLES, false);

    dma_set_irq1_channel_mask_enabled((1u << s_dma_chan_a) | (1u << s_dma_chan_b), true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_i2s_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start(s_dma_chan_a);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void audio_i2s_deinit(void) {
    audio_i2s_stop_wav();
    s_enabled = false;
    dma_channel_abort(s_dma_chan_a);
    dma_channel_abort(s_dma_chan_b);
    dma_set_irq1_channel_mask_enabled((1u << s_dma_chan_a) | (1u << s_dma_chan_b), false);
    dma_channel_unclaim(s_dma_chan_a);
    dma_channel_unclaim(s_dma_chan_b);

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_unclaim(s_pio, s_sm);
    audio_i2s_pins_safe();
}

void audio_i2s_set_enabled(bool enabled) { s_enabled = enabled; }
bool audio_i2s_is_enabled(void) { return s_enabled; }
void audio_i2s_set_freq(float freq_hz) { s_target_freq = freq_hz; }
void audio_i2s_set_volume(uint8_t volume_pct) { if (volume_pct > 100) volume_pct = 100; s_volume = volume_pct; }
void audio_i2s_set_wave_type(audio_wave_type_t type) { s_wave_type = type; }

void audio_i2s_play_melody(uint8_t melody_idx) {
    audio_i2s_stop_wav();
    s_melody_idx = 0;
    s_cur_melody = (melody_idx == 0) ? s_melody_mario : s_melody_tetris;
    s_note_end_time = 0;
    s_enabled = true;
}

void audio_i2s_stop_melody(void) {
    s_cur_melody = NULL;
    s_enabled = false;
}

void audio_i2s_update_playback(uint32_t now_ms) {
    if (!s_cur_melody) return;
    if (now_ms >= s_note_end_time) {
        const note_t *note = &s_cur_melody[s_melody_idx];
        if (note->dur_ms == 0) {
            s_melody_idx = 0;
            note = &s_cur_melody[0];
        }
        s_target_freq = (float)note->freq;
        s_note_end_time = now_ms + note->dur_ms;
        s_melody_idx++;
    }
}