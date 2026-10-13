/* Touch-controlled 2048 PAPP. Swipe in the desired direction to move tiles. */
#include "app_api.h"

#define BOARD_N 4
#define BOARD_X 119
#define BOARD_Y 58
#define CELL 54
#define GAP 5
#define TILE_X (BOARD_X + 5)
#define TILE_Y (BOARD_Y + 5)

#define COLOR_BG       0xD69A
#define COLOR_BOARD    0xAD55
#define COLOR_EMPTY    0xCE79
#define COLOR_TEXT     0x18C3
#define COLOR_DARK     0x6B4D
#define COLOR_LIGHT    0xFFFF
#define COLOR_ACCENT   0x05A0

typedef struct {
    const pico_host_api_t *api;
    uint16_t board[BOARD_N][BOARD_N];
    uint32_t score;
    uint32_t rng;
    int16_t touch_start_x;
    int16_t touch_start_y;
    int16_t touch_last_x;
    int16_t touch_last_y;
    bool touch_was_down;
    bool won;
    bool game_over;
} game2048_t;

static game2048_t s_game;

static void fill(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_game.api || !s_game.api->ui_draw_fill_rect || w <= 0 || h <= 0) return;
    ui_rect_t r = ui_rect_make(x, y, w, h);
    s_game.api->ui_draw_fill_rect(tile, &r, color);
}

static uint32_t random_next(void) {
    uint32_t x = s_game.rng;
    if (!x) x = 0x6D2B79F5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_game.rng = x;
    return x;
}

static void draw_number(ui_tile_t *tile, int16_t cx, int16_t cy, uint32_t value,
                        FontSize_t font, uint16_t color, uint16_t bg) {
    char text[11];
    uint8_t n = 0;
    do {
        text[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(text));
    for (uint8_t i = 0; i < n / 2; ++i) {
        char t = text[i];
        text[i] = text[n - 1 - i];
        text[n - 1 - i] = t;
    }
    text[n] = '\0';
    int16_t char_width = font >= FONT_20 ? 10 : 8;
    int16_t x = (int16_t)(cx - ((int16_t)n * char_width) / 2);
    int16_t y = (int16_t)(cy - (int16_t)font / 2);
    if (s_game.api->ui_draw_string)
        s_game.api->ui_draw_string(tile, x, y, text, font, color, bg);
}

static uint16_t tile_color(uint16_t value) {
    switch (value) {
    case 2:    return 0xFF5A;
    case 4:    return 0xF6F7;
    case 8:    return 0xFD20;
    case 16:   return 0xFC80;
    case 32:   return 0xFBE0;
    case 64:   return 0xF800;
    case 128:  return 0xE6A0;
    case 256:  return 0xCE60;
    case 512:  return 0xB600;
    case 1024: return 0x9D00;
    case 2048: return 0xFFE0;
    default:   return 0x780F;
    }
}

static void spawn_tile(void) {
    uint8_t empty_count = 0;
    for (uint8_t y = 0; y < BOARD_N; ++y)
        for (uint8_t x = 0; x < BOARD_N; ++x)
            if (!s_game.board[y][x]) ++empty_count;
    if (!empty_count) return;

    uint8_t target = (uint8_t)(random_next() % empty_count);
    for (uint8_t y = 0; y < BOARD_N; ++y) {
        for (uint8_t x = 0; x < BOARD_N; ++x) {
            if (!s_game.board[y][x] && target-- == 0) {
                s_game.board[y][x] = (random_next() % 10u == 0u) ? 4u : 2u;
                return;
            }
        }
    }
}

static void reset_game(void) {
    for (uint8_t y = 0; y < BOARD_N; ++y)
        for (uint8_t x = 0; x < BOARD_N; ++x) s_game.board[y][x] = 0;
    s_game.score = 0;
    s_game.won = false;
    s_game.game_over = false;
    s_game.rng ^= (uint32_t)(s_game.api && s_game.api->get_time_ms ? s_game.api->get_time_ms() : 0u);
    spawn_tile();
    spawn_tile();
    if (s_game.api && s_game.api->request_redraw) s_game.api->request_redraw();
}

static bool can_move(void) {
    for (uint8_t y = 0; y < BOARD_N; ++y) {
        for (uint8_t x = 0; x < BOARD_N; ++x) {
            uint16_t v = s_game.board[y][x];
            if (!v) return true;
            if (x + 1 < BOARD_N && s_game.board[y][x + 1] == v) return true;
            if (y + 1 < BOARD_N && s_game.board[y + 1][x] == v) return true;
        }
    }
    return false;
}

/* direction: 0=left, 1=right, 2=up, 3=down */
static bool move_board(uint8_t direction) {
    uint16_t before[BOARD_N][BOARD_N];
    for (uint8_t y = 0; y < BOARD_N; ++y)
        for (uint8_t x = 0; x < BOARD_N; ++x) before[y][x] = s_game.board[y][x];

    for (uint8_t line = 0; line < BOARD_N; ++line) {
        uint16_t packed[BOARD_N];
        uint16_t merged[BOARD_N];
        for (uint8_t i = 0; i < BOARD_N; ++i) {
            packed[i] = 0;
            merged[i] = 0;
        }
        uint8_t count = 0;
        for (uint8_t i = 0; i < BOARD_N; ++i) {
            uint8_t x = direction == 1 ? (uint8_t)(BOARD_N - 1 - i) :
                        (direction < 2 ? i : line);
            uint8_t y = direction == 3 ? (uint8_t)(BOARD_N - 1 - i) :
                        (direction > 1 ? i : line);
            uint16_t value = s_game.board[y][x];
            if (value) packed[count++] = value;
        }

        uint8_t out = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if (i + 1 < count && packed[i] == packed[i + 1] && packed[i] <= 16384u) {
                merged[out] = (uint16_t)(packed[i] * 2u);
                s_game.score += merged[out];
                if (merged[out] == 2048u) s_game.won = true;
                ++out;
                ++i;
            } else {
                merged[out++] = packed[i];
            }
        }

        for (uint8_t i = 0; i < BOARD_N; ++i) {
            uint8_t x = direction == 1 ? (uint8_t)(BOARD_N - 1 - i) :
                        (direction < 2 ? i : line);
            uint8_t y = direction == 3 ? (uint8_t)(BOARD_N - 1 - i) :
                        (direction > 1 ? i : line);
            s_game.board[y][x] = merged[i];
        }
    }

    bool changed = false;
    for (uint8_t y = 0; y < BOARD_N; ++y)
        for (uint8_t x = 0; x < BOARD_N; ++x)
            if (before[y][x] != s_game.board[y][x]) changed = true;
    if (!changed) return false;

    spawn_tile();
    s_game.game_over = !can_move();
    return true;
}

static bool game2048_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_game.api = api;
    s_game.rng = (uint32_t)(api->get_time_ms ? api->get_time_ms() : 1u) ^ 0x20482048u;
    s_game.touch_was_down = false;
    reset_game();
    return true;
}

static void game2048_update_core1(void) {
    if (s_game.api && s_game.api->delay_ms) s_game.api->delay_ms(5);
}

static void game2048_process_touch(bool is_touch, int16_t x, int16_t y) {
    if (is_touch && !s_game.touch_was_down) {
        s_game.touch_start_x = s_game.touch_last_x = x;
        s_game.touch_start_y = s_game.touch_last_y = y;
        if (x >= 382 && y >= 32 && y < 72) reset_game();
    } else if (is_touch && s_game.touch_was_down) {
        s_game.touch_last_x = x;
        s_game.touch_last_y = y;
    } else if (!is_touch && s_game.touch_was_down) {
        int16_t dx = s_game.touch_last_x - s_game.touch_start_x;
        int16_t dy = s_game.touch_last_y - s_game.touch_start_y;
        int16_t ax = dx < 0 ? (int16_t)-dx : dx;
        int16_t ay = dy < 0 ? (int16_t)-dy : dy;
        if ((ax > 18 || ay > 18) && !s_game.game_over) {
            uint8_t direction = ax > ay ? (dx < 0 ? 0u : 1u) : (dy < 0 ? 2u : 3u);
            if (move_board(direction) && s_game.api && s_game.api->request_redraw)
                s_game.api->request_redraw();
        }
    }
    s_game.touch_was_down = is_touch;
}

static void draw_board(ui_tile_t *tile) {
    fill(tile, BOARD_X, BOARD_Y, 4 * CELL + 3 * GAP + 10, 4 * CELL + 3 * GAP + 10, COLOR_BOARD);
    for (uint8_t y = 0; y < BOARD_N; ++y) {
        for (uint8_t x = 0; x < BOARD_N; ++x) {
            int16_t px = TILE_X + x * (CELL + GAP);
            int16_t py = TILE_Y + y * (CELL + GAP);
            uint16_t value = s_game.board[y][x];
            uint16_t bg = value ? tile_color(value) : COLOR_EMPTY;
            fill(tile, px, py, CELL, CELL, bg);
            if (value) {
                uint16_t fg = value <= 4 ? COLOR_DARK : COLOR_LIGHT;
                FontSize_t font = value < 128 ? FONT_24 : (value < 1024 ? FONT_20 : FONT_16);
                draw_number(tile, px + CELL / 2, py + CELL / 2, value, font, fg, bg);
            }
        }
    }
}

static void game2048_draw(ui_tile_t *tile) {
    if (!tile || !s_game.api) return;
    fill(tile, 0, 32, 480, 288, COLOR_BG);
    draw_board(tile);
    if (!s_game.api->ui_draw_string) return;

    char score[11];
    uint32_t value = s_game.score;
    uint8_t n = 0;
    do {
        score[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(score) - 1);
    for (uint8_t i = 0; i < n / 2; ++i) {
        char t = score[i]; score[i] = score[n - 1 - i]; score[n - 1 - i] = t;
    }
    score[n] = '\0';

    const char *status = s_game.game_over ? "GAME OVER" :
                         (s_game.won ? "2048! KEEP GOING" : "SWIPE TO MOVE");
    s_game.api->ui_draw_string(tile, 16, 34, "2048", FONT_20, COLOR_TEXT, COLOR_BG);
    s_game.api->ui_draw_string(tile, 100, 34, status, FONT_16,
                               s_game.game_over ? 0xF800 : COLOR_ACCENT, COLOR_BG);
    s_game.api->ui_draw_string(tile, 300, 34, "SCORE", FONT_16, COLOR_DARK, COLOR_BG);
    s_game.api->ui_draw_string(tile, 350, 34, score, FONT_16, COLOR_TEXT, COLOR_BG);
    s_game.api->ui_draw_string(tile, 404, 34, "RESET", FONT_16, COLOR_TEXT, COLOR_BG);
    s_game.api->ui_draw_string(tile, 128, 300, "SWIPE UP / DOWN / LEFT / RIGHT", FONT_16,
                               COLOR_DARK, COLOR_BG);
}

static void game2048_exit(void) { s_game.api = 0; }

static const app_plugin_t s_plugin = {
    .magic = APP_MAGIC, .version = 0x00010000u, .name = "2048",
    .init = game2048_init, .update_core1 = game2048_update_core1,
    .process_touch = game2048_process_touch, .draw = game2048_draw, .exit = game2048_exit,
};

const app_plugin_t *game2048_get_descriptor(void) { return &s_plugin; }
