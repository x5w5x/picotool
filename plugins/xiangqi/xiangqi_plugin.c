/* Local two-player Chinese chess (Xiangqi) PAPP. */
#include "app_api.h"

#define ROWS 10
#define COLS 9
#define BOARD_X 32
#define BOARD_Y 58
#define STEP_X 52
#define STEP_Y 25
#define DISC_R 11

#define COLOR_BG 0xD69A
#define COLOR_GRID 0x632C
#define COLOR_TEXT 0x18C3
#define COLOR_RED 0xD800
#define COLOR_BLACK 0x0000
#define COLOR_SELECTED 0xFFE0
#define COLOR_DISC 0xE71C

enum { K_ROOK = 1, K_HORSE, K_ELEPHANT, K_ADVISOR, K_GENERAL, K_CANNON, K_PAWN };

typedef struct {
    const pico_host_api_t *api;
    uint8_t board[ROWS][COLS]; /* side * 8 + kind; side 1 red, side 2 black */
    uint8_t turn;
    uint8_t result; /* 0 playing, 1 red wins, 2 black wins */
    int8_t selected_x;
    int8_t selected_y;
    int8_t last_x;
    int8_t last_y;
    bool in_check;
    bool touch_was_down;
} xiangqi_t;

static xiangqi_t s_game;

static uint8_t side_of(uint8_t p) { return (uint8_t)(p >> 3); }
static uint8_t kind_of(uint8_t p) { return (uint8_t)(p & 7u); }
static uint8_t piece(uint8_t side, uint8_t kind) { return (uint8_t)((side << 3) | kind); }
static bool inside(int16_t x, int16_t y) { return x >= 0 && x < COLS && y >= 0 && y < ROWS; }

static void fill(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_game.api || !s_game.api->ui_draw_fill_rect || w <= 0 || h <= 0) return;
    ui_rect_t r = ui_rect_make(x, y, w, h);
    s_game.api->ui_draw_fill_rect(tile, &r, color);
}

static void line(ui_tile_t *tile, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    int16_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    for (;;) {
        fill(tile, x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = (int16_t)(2 * err);
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_disc(ui_tile_t *tile, int16_t cx, int16_t cy, uint16_t color, bool inner) {
    static const uint8_t outer_width[11] = {4,7,8,9,10,10,10,9,8,7,4};
    static const uint8_t inner_width[11] = {0,4,6,7,8,8,8,7,6,4,0};
    const uint8_t *half_width = inner ? inner_width : outer_width;
    for (int8_t row = -5; row <= 5; ++row)
        if (half_width[row + 5])
            fill(tile, cx - half_width[row + 5], cy + row, half_width[row + 5] * 2 + 1, 1, color);
}

static bool in_palace(uint8_t side, int16_t x, int16_t y) {
    if (x < 3 || x > 5) return false;
    return side == 1 ? (y >= 7 && y <= 9) : (y >= 0 && y <= 2);
}

static uint8_t count_between(const uint8_t b[ROWS][COLS], int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (x0 != x1 && y0 != y1) return 255;
    int16_t dx = x1 == x0 ? 0 : (x1 > x0 ? 1 : -1);
    int16_t dy = y1 == y0 ? 0 : (y1 > y0 ? 1 : -1);
    uint8_t count = 0;
    int16_t x = x0 + dx, y = y0 + dy;
    while (x != x1 || y != y1) {
        if (b[y][x]) ++count;
        x += dx;
        y += dy;
    }
    return count;
}

static bool pseudo_move(const uint8_t b[ROWS][COLS], int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (!inside(x0, y0) || !inside(x1, y1) || (x0 == x1 && y0 == y1)) return false;
    uint8_t p = b[y0][x0];
    if (!p) return false;
    uint8_t side = side_of(p), kind = kind_of(p), target = b[y1][x1];
    if (target && side_of(target) == side) return false;
    int16_t dx = x1 - x0, dy = y1 - y0;
    int16_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    switch (kind) {
    case K_ROOK:
        return (dx == 0 || dy == 0) && count_between(b, x0, y0, x1, y1) == 0;
    case K_CANNON: {
        if (dx != 0 && dy != 0) return false;
        uint8_t screens = count_between(b, x0, y0, x1, y1);
        return target ? screens == 1 : screens == 0;
    }
    case K_HORSE:
        if (!((ax == 1 && ay == 2) || (ax == 2 && ay == 1))) return false;
        return ax == 2 ? b[y0][x0 + (dx > 0 ? 1 : -1)] == 0 : b[y0 + (dy > 0 ? 1 : -1)][x0] == 0;
    case K_ELEPHANT:
        if (ax != 2 || ay != 2 || b[y0 + dy / 2][x0 + dx / 2]) return false;
        return side == 1 ? y1 >= 5 : y1 <= 4;
    case K_ADVISOR:
        return ax == 1 && ay == 1 && in_palace(side, x1, y1);
    case K_GENERAL:
        if (dx == 0 && (side_of(target) == (uint8_t)(3u - side)) && kind_of(target) == K_GENERAL)
            return count_between(b, x0, y0, x1, y1) == 0;
        return ax + ay == 1 && in_palace(side, x1, y1);
    case K_PAWN:
        if (side == 1) {
            if (dy == -1 && dx == 0) return true;
            return y0 <= 4 && dy == 0 && ax == 1;
        }
        if (dy == 1 && dx == 0) return true;
        return y0 >= 5 && dy == 0 && ax == 1;
    default: return false;
    }
}

static bool find_general(const uint8_t b[ROWS][COLS], uint8_t side, int16_t *gx, int16_t *gy) {
    uint8_t wanted = piece(side, K_GENERAL);
    for (int16_t y = 0; y < ROWS; ++y)
        for (int16_t x = 0; x < COLS; ++x)
            if (b[y][x] == wanted) { *gx = x; *gy = y; return true; }
    return false;
}

static bool is_in_check(const uint8_t b[ROWS][COLS], uint8_t side) {
    int16_t gx, gy;
    if (!find_general(b, side, &gx, &gy)) return true;
    /* The two generals may not face each other on an empty file. */
    for (int16_t y = 0; y < ROWS; ++y)
        if (b[y][gx] == piece((uint8_t)(3u - side), K_GENERAL) && count_between(b, gx, gy, gx, y) == 0)
            return true;
    uint8_t enemy = (uint8_t)(3u - side);
    for (int16_t y = 0; y < ROWS; ++y)
        for (int16_t x = 0; x < COLS; ++x)
            if (b[y][x] && side_of(b[y][x]) == enemy && pseudo_move(b, x, y, gx, gy)) return true;
    return false;
}

static bool legal_move(const uint8_t b[ROWS][COLS], int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t side) {
    if (!pseudo_move(b, x0, y0, x1, y1)) return false;
    uint8_t copy[ROWS][COLS];
    for (uint8_t y = 0; y < ROWS; ++y)
        for (uint8_t x = 0; x < COLS; ++x) copy[y][x] = b[y][x];
    copy[y1][x1] = copy[y0][x0];
    copy[y0][x0] = 0;
    return !is_in_check(copy, side);
}

static bool has_legal_move(const uint8_t b[ROWS][COLS], uint8_t side) {
    for (int16_t y0 = 0; y0 < ROWS; ++y0)
        for (int16_t x0 = 0; x0 < COLS; ++x0)
            if (b[y0][x0] && side_of(b[y0][x0]) == side)
                for (int16_t y1 = 0; y1 < ROWS; ++y1)
                    for (int16_t x1 = 0; x1 < COLS; ++x1)
                        if (legal_move(b, x0, y0, x1, y1, side)) return true;
    return false;
}

static void initial_position(void) {
    static const uint8_t back[COLS] = {K_ROOK,K_HORSE,K_ELEPHANT,K_ADVISOR,K_GENERAL,K_ADVISOR,K_ELEPHANT,K_HORSE,K_ROOK};
    for (uint8_t y = 0; y < ROWS; ++y)
        for (uint8_t x = 0; x < COLS; ++x) s_game.board[y][x] = 0;
    for (uint8_t x = 0; x < COLS; ++x) {
        s_game.board[0][x] = piece(2, back[x]);
        s_game.board[9][x] = piece(1, back[x]);
        if ((x & 1u) == 0) {
            s_game.board[3][x] = piece(2, K_PAWN);
            s_game.board[6][x] = piece(1, K_PAWN);
        }
    }
    s_game.board[2][1] = s_game.board[2][7] = piece(2, K_CANNON);
    s_game.board[7][1] = s_game.board[7][7] = piece(1, K_CANNON);
    s_game.turn = 1;
    s_game.result = 0;
    s_game.selected_x = s_game.selected_y = -1;
    s_game.last_x = s_game.last_y = -1;
    s_game.in_check = false;
}

static bool xiangqi_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_game.api = api;
    s_game.touch_was_down = false;
    initial_position();
    return true;
}

static void xiangqi_update_core1(void) {
    if (s_game.api && s_game.api->delay_ms) s_game.api->delay_ms(5);
}

static bool board_touch_cell(int16_t x, int16_t y, int16_t *cx, int16_t *cy) {
    int16_t dx = x - BOARD_X, dy = y - BOARD_Y;
    if (dx < -STEP_X / 2 || dx > 8 * STEP_X + STEP_X / 2 ||
        dy < -STEP_Y / 2 || dy > 9 * STEP_Y + STEP_Y / 2) return false;
    int16_t col = (int16_t)((dx + STEP_X / 2) / STEP_X);
    int16_t row = (int16_t)((dy + STEP_Y / 2) / STEP_Y);
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return false;
    if (dx - col * STEP_X > STEP_X / 2 || col * STEP_X - dx > STEP_X / 2 ||
        dy - row * STEP_Y > STEP_Y / 2 || row * STEP_Y - dy > STEP_Y / 2) return false;
    *cx = col; *cy = row;
    return true;
}

static void commit_move(int16_t x, int16_t y) {
    uint8_t moving = s_game.board[(int8_t)s_game.selected_y][(int8_t)s_game.selected_x];
    uint8_t captured = s_game.board[y][x];
    s_game.board[y][x] = moving;
    s_game.board[(int8_t)s_game.selected_y][(int8_t)s_game.selected_x] = 0;
    s_game.last_x = (int8_t)x;
    s_game.last_y = (int8_t)y;
    s_game.selected_x = s_game.selected_y = -1;
    if (captured && kind_of(captured) == K_GENERAL) {
        s_game.result = s_game.turn;
        return;
    }
    s_game.turn = (uint8_t)(3u - s_game.turn);
    s_game.in_check = is_in_check(s_game.board, s_game.turn);
    if (!has_legal_move(s_game.board, s_game.turn)) s_game.result = (uint8_t)(3u - s_game.turn);
}

static void xiangqi_process_touch(bool is_touch, int16_t x, int16_t y) {
    if (is_touch && !s_game.touch_was_down) {
        if (x >= 382 && y >= 32 && y < 52) {
            initial_position();
            if (s_game.api && s_game.api->request_redraw) s_game.api->request_redraw();
        } else if (!s_game.result) {
            int16_t cx, cy;
            if (board_touch_cell(x, y, &cx, &cy)) {
                uint8_t tapped = s_game.board[cy][cx];
                if (s_game.selected_x < 0) {
                    if (tapped && side_of(tapped) == s_game.turn) {
                        s_game.selected_x = (int8_t)cx;
                        s_game.selected_y = (int8_t)cy;
                    }
                } else if (cx == s_game.selected_x && cy == s_game.selected_y) {
                    s_game.selected_x = s_game.selected_y = -1;
                } else if (tapped && side_of(tapped) == s_game.turn) {
                    s_game.selected_x = (int8_t)cx;
                    s_game.selected_y = (int8_t)cy;
                } else if (legal_move(s_game.board, s_game.selected_x, s_game.selected_y, cx, cy, s_game.turn)) {
                    commit_move(cx, cy);
                }
                if (s_game.api && s_game.api->request_redraw) s_game.api->request_redraw();
            }
        }
    }
    s_game.touch_was_down = is_touch;
}

static void draw_board(ui_tile_t *tile) {
    const int16_t right = BOARD_X + 8 * STEP_X;
    const int16_t bottom = BOARD_Y + 9 * STEP_Y;
    for (uint8_t row = 0; row < ROWS; ++row)
        if (s_game.api->ui_draw_fast_hline)
            s_game.api->ui_draw_fast_hline(tile, BOARD_X, BOARD_Y + row * STEP_Y, right - BOARD_X + 1, COLOR_GRID);
    for (uint8_t col = 0; col < COLS; ++col) {
        int16_t x = BOARD_X + col * STEP_X;
        if (col == 0 || col == 8) {
            fill(tile, x, BOARD_Y, 1, bottom - BOARD_Y + 1, COLOR_GRID);
        } else {
            fill(tile, x, BOARD_Y, 1, 4 * STEP_Y + 1, COLOR_GRID);
            fill(tile, x, BOARD_Y + 5 * STEP_Y, 1, 4 * STEP_Y + 1, COLOR_GRID);
        }
    }
    line(tile, BOARD_X + 3 * STEP_X, BOARD_Y, BOARD_X + 5 * STEP_X, BOARD_Y + 2 * STEP_Y, COLOR_GRID);
    line(tile, BOARD_X + 5 * STEP_X, BOARD_Y, BOARD_X + 3 * STEP_X, BOARD_Y + 2 * STEP_Y, COLOR_GRID);
    line(tile, BOARD_X + 3 * STEP_X, BOARD_Y + 7 * STEP_Y, BOARD_X + 5 * STEP_X, bottom, COLOR_GRID);
    line(tile, BOARD_X + 5 * STEP_X, BOARD_Y + 7 * STEP_Y, BOARD_X + 3 * STEP_X, bottom, COLOR_GRID);
}

static const char *piece_label(uint8_t p) {
    /* 字库按 GB2312 编码索引；使用简体字，避免传统字在点阵库中无字模。 */
    static const char *red[] = {"", "车", "马", "相", "仕", "帅", "炮", "兵"};
    static const char *black[] = {"", "车", "马", "象", "士", "将", "炮", "卒"};
    return side_of(p) == 1 ? red[kind_of(p)] : black[kind_of(p)];
}

static void xiangqi_draw(ui_tile_t *tile) {
    if (!tile || !s_game.api) return;
    fill(tile, 0, 32, 480, 288, COLOR_BG);
    draw_board(tile);
    if (s_game.api->ui_draw_string)
        s_game.api->ui_draw_string(tile, BOARD_X + 3 * STEP_X + 32, BOARD_Y + 4 * STEP_Y + 3,
                                   "RIVER", FONT_16, COLOR_GRID, COLOR_BG);
    for (uint8_t y = 0; y < ROWS; ++y) {
        for (uint8_t x = 0; x < COLS; ++x) {
            uint8_t p = s_game.board[y][x];
            if (!p) continue;
            int16_t px = BOARD_X + x * STEP_X;
            int16_t py = BOARD_Y + y * STEP_Y;
            draw_disc(tile, px, py, COLOR_GRID, false);
            draw_disc(tile, px, py, COLOR_DISC, true);
            if (s_game.api->ui_draw_string)
                s_game.api->ui_draw_string(tile, px - 8, py - 8, piece_label(p), FONT_16,
                                           side_of(p) == 1 ? COLOR_RED : COLOR_BLACK, COLOR_DISC);
            if ((int8_t)x == s_game.selected_x && (int8_t)y == s_game.selected_y) {
                fill(tile, px - 10, py - 10, 6, 2, COLOR_SELECTED);
                fill(tile, px + 4, py - 10, 6, 2, COLOR_SELECTED);
                fill(tile, px - 10, py + 8, 6, 2, COLOR_SELECTED);
                fill(tile, px + 4, py + 8, 6, 2, COLOR_SELECTED);
                fill(tile, px - 10, py - 4, 2, 8, COLOR_SELECTED);
                fill(tile, px + 8, py - 4, 2, 8, COLOR_SELECTED);
            } else if ((int8_t)x == s_game.last_x && (int8_t)y == s_game.last_y) {
                fill(tile, px - 1, py - 1, 3, 3, COLOR_SELECTED);
            }
        }
    }
    if (s_game.api->ui_draw_string) {
        const char *status = s_game.result == 1 ? "RED WINS" :
                             (s_game.result == 2 ? "BLACK WINS" :
                             (s_game.in_check ? (s_game.turn == 1 ? "RED IN CHECK" : "BLACK IN CHECK") :
                             (s_game.turn == 1 ? "RED TO MOVE" : "BLACK TO MOVE")));
        s_game.api->ui_draw_string(tile, 16, 34, "XIANGQI", FONT_16, COLOR_TEXT, COLOR_BG);
        s_game.api->ui_draw_string(tile, 122, 34, status, FONT_16, s_game.turn == 1 ? COLOR_RED : COLOR_BLACK, COLOR_BG);
        s_game.api->ui_draw_string(tile, 388, 34, "RESET", FONT_16, COLOR_TEXT, COLOR_BG);
        s_game.api->ui_draw_string(tile, 110, 302, "TAP PIECE, THEN DESTINATION", FONT_16, COLOR_TEXT, COLOR_BG);
    }
}

static void xiangqi_exit(void) { s_game.api = 0; }

static const app_plugin_t s_plugin = {
    .magic = APP_MAGIC, .version = 0x00010000u, .name = "Xiangqi",
    .init = xiangqi_init, .update_core1 = xiangqi_update_core1,
    .process_touch = xiangqi_process_touch, .draw = xiangqi_draw, .exit = xiangqi_exit,
};

const app_plugin_t *xiangqi_get_descriptor(void) { return &s_plugin; }
