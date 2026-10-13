/* Minimal offline dinosaur runner PAPP. Core 1 advances physics; Core 0 draws/touches. */
#include "app_api.h"

#define SCREEN_W 480
#define SCREEN_TOP 32
#define GROUND_Y 252
#define DINO_X 48
#define DINO_W 28
#define DINO_H 34
#define CACTUS_W 18
#define FRAME_MS 20u
#define REDRAW_MS 40u
#define OBSTACLE_SPEED 4
#define FIRST_OBSTACLE_OFFSET 100
#define NEXT_OBSTACLE_MIN_GAP 130
#define NEXT_OBSTACLE_RANDOM_GAP 180
#define COLOR_SKY 0x9E7F
#define COLOR_GROUND 0x39E7
#define COLOR_DINO 0x18C3
#define COLOR_DINO_ACC 0x2D6B
#define COLOR_CACTUS 0x2D6B
#define COLOR_TEXT 0x18C3

typedef struct {
    const pico_host_api_t *api;
    volatile uint8_t state;
    volatile int16_t dino_y;
    volatile int8_t jump_v;
    volatile int16_t cactus_x;
    volatile uint8_t cactus_h;
    volatile uint16_t score;
    volatile uint16_t ground_scroll;
    uint32_t last_frame;
    uint32_t last_redraw;
    uint32_t rng;
    bool touch_was_down;
} dino_runner_t;

static dino_runner_t s_game;

static uint32_t random_next(dino_runner_t *g) {
    uint32_t x = g->rng ? g->rng : 0x6D2B79F5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return x;
}

static void request_redraw(dino_runner_t *g) {
    if (g->api && g->api->request_redraw) g->api->request_redraw();
}

static void start_run(dino_runner_t *g) {
    g->state = 1;
    g->dino_y = GROUND_Y - DINO_H;
    g->jump_v = 0;
    g->cactus_x = SCREEN_W + FIRST_OBSTACLE_OFFSET;
    g->cactus_h = (uint8_t)(30u + random_next(g) % 19u);
    g->score = 0;
    g->ground_scroll = 0;
    g->last_frame = g->api && g->api->get_time_ms ? g->api->get_time_ms() : 0;
    g->last_redraw = g->last_frame;
    request_redraw(g);
}

static void fill_rect(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_game.api || !s_game.api->ui_draw_fill_rect || w <= 0 || h <= 0) return;
    ui_rect_t r = ui_rect_make(x, y, w, h);
    s_game.api->ui_draw_fill_rect(tile, &r, color);
}

static bool dino_runner_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_game.api = api;
    s_game.rng = (api->get_time_us ? (uint32_t)api->get_time_us() : 0x5317u) ^ 0xC0FFEEu;
    s_game.state = 0;
    s_game.dino_y = GROUND_Y - DINO_H;
    s_game.jump_v = 0;
    s_game.cactus_x = SCREEN_W + FIRST_OBSTACLE_OFFSET;
    s_game.cactus_h = 36;
    s_game.score = 0;
    s_game.ground_scroll = 0;
    s_game.touch_was_down = false;
    s_game.last_frame = 0;
    s_game.last_redraw = 0;
    return true;
}

static void dino_runner_update_core1(void) {
    dino_runner_t *g = &s_game;
    if (!g->api || !g->api->get_time_ms) return;
    uint32_t now = g->api->get_time_ms();
    if (g->state == 1 && (uint32_t)(now - g->last_frame) >= FRAME_MS) {
        g->last_frame = now;
        if (g->jump_v != 0 || g->dino_y < GROUND_Y - DINO_H) {
            g->dino_y = (int16_t)(g->dino_y + g->jump_v);
            ++g->jump_v;
            if (g->dino_y >= GROUND_Y - DINO_H) {
                g->dino_y = GROUND_Y - DINO_H;
                g->jump_v = 0;
            }
        }
        g->cactus_x = (int16_t)(g->cactus_x - OBSTACLE_SPEED);
        g->ground_scroll = (uint16_t)((g->ground_scroll + OBSTACLE_SPEED) % 32u);
        ++g->score;
        /* Slightly forgiving hitbox to match the visible sprite better. */
        int16_t dino_right = DINO_X + DINO_W - 7;
        int16_t dino_left = DINO_X + 7;
        int16_t dino_bottom = g->dino_y + DINO_H;
        int16_t cactus_top = GROUND_Y - g->cactus_h;
        if (g->cactus_x < dino_right && g->cactus_x + CACTUS_W > dino_left && dino_bottom > cactus_top + 6)
            g->state = 2;
        if (g->cactus_x + CACTUS_W < 0) {
            g->cactus_x = (int16_t)(SCREEN_W + NEXT_OBSTACLE_MIN_GAP + random_next(g) % NEXT_OBSTACLE_RANDOM_GAP);
            g->cactus_h = (uint8_t)(30u + random_next(g) % 19u);
        }
        if ((uint32_t)(now - g->last_redraw) >= REDRAW_MS || g->state != 1) {
            g->last_redraw = now;
            request_redraw(g);
        }
    }
    if (g->api->delay_ms) g->api->delay_ms(5);
}

static void dino_runner_process_touch(bool is_touch, int16_t x, int16_t y) {
    (void)x; (void)y;
    if (is_touch && !s_game.touch_was_down) {
        if (s_game.state != 1) start_run(&s_game);
        else if (s_game.dino_y >= GROUND_Y - DINO_H) s_game.jump_v = -12;
    }
    s_game.touch_was_down = is_touch;
}

static void draw_dinosaur(ui_tile_t *tile, int16_t y, bool run_frame) {
    fill_rect(tile, DINO_X + 4, y + 12, 20, 20, COLOR_DINO);
    fill_rect(tile, DINO_X + 15, y + 2, 17, 17, COLOR_DINO);
    fill_rect(tile, DINO_X + 27, y + 8, 3, 3, COLOR_SKY);
    fill_rect(tile, DINO_X + 1, y + 17, 7, 5, COLOR_DINO_ACC);
    fill_rect(tile, DINO_X + 8, y + (run_frame ? 30 : 29), 5, run_frame ? 4 : 5, COLOR_DINO);
    fill_rect(tile, DINO_X + 20, y + (run_frame ? 29 : 30), 5, run_frame ? 5 : 4, COLOR_DINO);
}

static void draw_cactus(ui_tile_t *tile, int16_t x, uint8_t height) {
    int16_t top = GROUND_Y - height;
    fill_rect(tile, x + 6, top, 8, height, COLOR_CACTUS);
    fill_rect(tile, x, top + 13, 7, 5, COLOR_CACTUS);
    fill_rect(tile, x, top + 7, 5, 11, COLOR_CACTUS);
    fill_rect(tile, x + 13, top + 20, 5, 5, COLOR_CACTUS);
    fill_rect(tile, x + 13, top + 14, 5, 11, COLOR_CACTUS);
}

static void draw_score(ui_tile_t *tile, uint16_t score) {
    char text[16], rev[6];
    uint8_t n = 0, p = 6;
    text[0]='S'; text[1]='C'; text[2]='O'; text[3]='R'; text[4]='E'; text[5]=' ';
    do { rev[n++] = (char)('0' + score % 10u); score = (uint16_t)(score / 10u); }
    while (score && n < sizeof(rev));
    while (n) text[p++] = rev[--n];
    text[p] = 0;
    if (s_game.api && s_game.api->ui_draw_string)
        s_game.api->ui_draw_string(tile, 382, 40, text, FONT_16, COLOR_TEXT, COLOR_SKY);
}

static void dino_runner_draw(ui_tile_t *tile) {
    if (!tile || !s_game.api) return;
    fill_rect(tile, 0, SCREEN_TOP, SCREEN_W, 320 - SCREEN_TOP, COLOR_SKY);
    fill_rect(tile, 84, 88, 34, 5, 0xFFFF); fill_rect(tile, 278, 112, 44, 5, 0xFFFF);
    fill_rect(tile, 94, 83, 16, 5, 0xFFFF); fill_rect(tile, 290, 107, 20, 5, 0xFFFF);
    if (s_game.api->ui_draw_fast_hline)
        s_game.api->ui_draw_fast_hline(tile, 16, GROUND_Y, 448, COLOR_GROUND);
    for (int16_t x = 16 - (int16_t)s_game.ground_scroll; x < SCREEN_W; x += 32)
        fill_rect(tile, x, GROUND_Y + 9, 12, 2, COLOR_GROUND);
    if (s_game.state != 0) {
        draw_cactus(tile, s_game.cactus_x, s_game.cactus_h);
        draw_dinosaur(tile, s_game.dino_y, (s_game.score & 1u) != 0);
    } else draw_dinosaur(tile, GROUND_Y - DINO_H, false);
    if (s_game.api->ui_draw_string) {
        const char *status = s_game.state == 1 ? "DINO RUN" :
            (s_game.state == 2 ? "GAME OVER - TAP TO RETRY" : "TAP TO START - JUMP TO AVOID CACTUS");
        s_game.api->ui_draw_string(tile, 22, 40, status, FONT_16, COLOR_TEXT, COLOR_SKY);
        if (s_game.state == 1 || s_game.state == 2)
            s_game.api->ui_draw_string(tile, 22, 282, "TAP SCREEN TO JUMP / RETRY", FONT_16, COLOR_TEXT, COLOR_SKY);
    }
    draw_score(tile, s_game.score);
}

static void dino_runner_exit(void) { s_game.api = 0; }
static const app_plugin_t s_dino_plugin = {
    .magic=APP_MAGIC, .version=0x00010000u, .name="Dino Run",
    .init=dino_runner_init, .update_core1=dino_runner_update_core1,
    .process_touch=dino_runner_process_touch, .draw=dino_runner_draw, .exit=dino_runner_exit,
};
const app_plugin_t *dino_runner_get_descriptor(void) { return &s_dino_plugin; }
