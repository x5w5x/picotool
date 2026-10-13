/* Two-player Gomoku PAPP. Core 0 handles input/drawing; state is tiny and synchronous. */
#include "app_api.h"

#define BOARD_N 15
#define BOARD_X 105
#define BOARD_Y 44
#define CELL 18
#define STONE_HALF 5
#define COLOR_BG 0xD69A
#define COLOR_GRID 0x632C
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_LAST 0xFBE0
#define COLOR_TEXT 0x18C3
#define COLOR_ACCENT 0x05A0

typedef struct {
    const pico_host_api_t *api;
    uint8_t board[BOARD_N][BOARD_N]; /* 0 empty, 1 black, 2 white */
    uint8_t turn;
    uint8_t result; /* 0 playing, 1 black wins, 2 white wins, 3 draw */
    int8_t last_x;
    int8_t last_y;
    bool touch_was_down;
} gomoku_t;

static gomoku_t s_game;

static void fill(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_game.api || !s_game.api->ui_draw_fill_rect || w <= 0 || h <= 0) return;
    ui_rect_t r = ui_rect_make(x, y, w, h);
    s_game.api->ui_draw_fill_rect(tile, &r, color);
}

static void reset_game(void) {
    for (uint8_t y = 0; y < BOARD_N; ++y)
        for (uint8_t x = 0; x < BOARD_N; ++x) s_game.board[y][x] = 0;
    s_game.turn = 1;
    s_game.result = 0;
    s_game.last_x = -1;
    s_game.last_y = -1;
    if (s_game.api && s_game.api->request_redraw) s_game.api->request_redraw();
}

static bool check_five(uint8_t x, uint8_t y, uint8_t color) {
    static const int8_t dx[4] = {1, 0, 1, 1};
    static const int8_t dy[4] = {0, 1, 1, -1};
    for (uint8_t d = 0; d < 4; ++d) {
        uint8_t count = 1;
        for (int8_t sign = -1; sign <= 1; sign += 2) {
            int16_t cx = x, cy = y;
            for (;;) {
                cx += dx[d] * sign;
                cy += dy[d] * sign;
                if (cx < 0 || cx >= BOARD_N || cy < 0 || cy >= BOARD_N || s_game.board[cy][cx] != color) break;
                ++count;
            }
        }
        if (count >= 5) return true;
    }
    return false;
}

static bool gomoku_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_game.api = api;
    s_game.touch_was_down = false;
    reset_game();
    return true;
}

static void gomoku_update_core1(void) {
    if (s_game.api && s_game.api->delay_ms) s_game.api->delay_ms(5);
}

static void gomoku_process_touch(bool is_touch, int16_t x, int16_t y) {
    if (is_touch && !s_game.touch_was_down) {
        if (x >= 382 && y >= 32 && y < 72) {
            reset_game();
        } else if (s_game.result == 0) {
            int16_t gx = (int16_t)((x - BOARD_X + CELL / 2) / CELL);
            int16_t gy = (int16_t)((y - BOARD_Y + CELL / 2) / CELL);
            if (gx >= 0 && gx < BOARD_N && gy >= 0 && gy < BOARD_N &&
                x >= BOARD_X - CELL / 2 && x <= BOARD_X + (BOARD_N - 1) * CELL + CELL / 2 &&
                y >= BOARD_Y - CELL / 2 && y <= BOARD_Y + (BOARD_N - 1) * CELL + CELL / 2 &&
                s_game.board[gy][gx] == 0) {
                s_game.board[gy][gx] = s_game.turn;
                s_game.last_x = (int8_t)gx;
                s_game.last_y = (int8_t)gy;
                if (check_five((uint8_t)gx, (uint8_t)gy, s_game.turn)) s_game.result = s_game.turn;
                else {
                    bool full = true;
                    for (uint8_t row = 0; row < BOARD_N && full; ++row)
                        for (uint8_t col = 0; col < BOARD_N; ++col)
                            if (!s_game.board[row][col]) { full = false; break; }
                    if (full) s_game.result = 3;
                    else s_game.turn = (uint8_t)(3u - s_game.turn);
                }
                if (s_game.api && s_game.api->request_redraw) s_game.api->request_redraw();
            }
        }
    }
    s_game.touch_was_down = is_touch;
}

static void draw_board(ui_tile_t *tile) {
    if (s_game.api->ui_draw_fast_hline) {
        for (uint8_t i = 0; i < BOARD_N; ++i)
            s_game.api->ui_draw_fast_hline(tile, BOARD_X, BOARD_Y + i * CELL,
                                           (BOARD_N - 1) * CELL + 1, COLOR_GRID);
    }
    for (uint8_t i = 0; i < BOARD_N; ++i)
        fill(tile, BOARD_X + i * CELL, BOARD_Y, 1, (BOARD_N - 1) * CELL + 1, COLOR_GRID);
    /* Four small star points help orient the 15x15 board. */
    static const uint8_t star[4][2] = {{3,3},{11,3},{3,11},{11,11}};
    for (uint8_t i = 0; i < 4; ++i)
        fill(tile, BOARD_X + star[i][0] * CELL - 2, BOARD_Y + star[i][1] * CELL - 2, 5, 5, COLOR_GRID);
}

static void gomoku_draw(ui_tile_t *tile) {
    if (!tile || !s_game.api) return;
    fill(tile, 0, 32, 480, 288, COLOR_BG);
    draw_board(tile);
    for (uint8_t y = 0; y < BOARD_N; ++y) {
        for (uint8_t x = 0; x < BOARD_N; ++x) {
            uint8_t c = s_game.board[y][x];
            if (!c) continue;
            int16_t px = BOARD_X + x * CELL;
            int16_t py = BOARD_Y + y * CELL;
            uint16_t color = c == 1 ? COLOR_BLACK : COLOR_WHITE;
            fill(tile, px - STONE_HALF, py - STONE_HALF, STONE_HALF * 2 + 1, STONE_HALF * 2 + 1, color);
            if (s_game.last_x == (int8_t)x && s_game.last_y == (int8_t)y)
                fill(tile, px - 1, py - 1, 3, 3, COLOR_LAST);
        }
    }
    if (s_game.api->ui_draw_string) {
        const char *status = s_game.result == 1 ? "BLACK WINS" :
                             (s_game.result == 2 ? "WHITE WINS" :
                             (s_game.result == 3 ? "DRAW" : (s_game.turn == 1 ? "BLACK TO MOVE" : "WHITE TO MOVE")));
        s_game.api->ui_draw_string(tile, 16, 34, "GOMOKU", FONT_16, COLOR_TEXT, COLOR_BG);
        s_game.api->ui_draw_string(tile, 112, 34, status, FONT_16, COLOR_ACCENT, COLOR_BG);
        s_game.api->ui_draw_string(tile, 388, 34, "RESET", FONT_16, COLOR_TEXT, COLOR_BG);
        s_game.api->ui_draw_string(tile, 100, 301, "15x15  -  TAP AN INTERSECTION", FONT_16, COLOR_TEXT, COLOR_BG);
    }
}

static void gomoku_exit(void) { s_game.api = 0; }

static const app_plugin_t s_plugin = {
    .magic = APP_MAGIC, .version = 0x00010000u, .name = "Gomoku",
    .init = gomoku_init, .update_core1 = gomoku_update_core1,
    .process_touch = gomoku_process_touch, .draw = gomoku_draw, .exit = gomoku_exit,
};

const app_plugin_t *gomoku_get_descriptor(void) { return &s_plugin; }
