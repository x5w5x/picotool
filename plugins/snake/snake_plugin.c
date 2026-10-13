/* Lightweight Snake PAPP: Core 1 updates game state; Core 0 handles UI. */
#include "app_api.h"

#define GRID_COLS 28
#define GRID_ROWS 16
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_SIZE 12
#define BOARD_X 72
#define BOARD_Y 62
#define STEP_MS 140u
#define CONTROL_CENTER_X 212
#define CONTROL_LEFT_X   156
#define CONTROL_RIGHT_X  268
#define CONTROL_TOP_Y    258
#define CONTROL_ROW_Y    288
#define CONTROL_W        56
#define CONTROL_H        28

#define COLOR_BG       0x0841
#define COLOR_PANEL    0x2104
#define COLOR_GRID     0x3186
#define COLOR_SNAKE    0x07E0
#define COLOR_HEAD     0xBFE0
#define COLOR_FOOD     0xF800
#define COLOR_TEXT     0xFFFF

typedef enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT } direction_t;

typedef struct {
    const pico_host_api_t *api;
    ui_btn_t start;
    ui_btn_t btn_up;
    ui_btn_t btn_left;
    ui_btn_t btn_right;
    ui_btn_t btn_down;
    uint16_t body[GRID_CELLS];
    uint16_t length;
    uint16_t food;
    uint32_t last_step;
    uint32_t rng;

    // ⚡ 当前前进方向与 2 级预输入缓冲队列
    volatile uint8_t direction;
    volatile uint8_t dir_queue[2];
    volatile uint8_t dir_queue_len;

    volatile uint8_t state; /* 0=ready, 1=running, 2=game over */
    volatile uint16_t score;

    bool touch_was_down;
} snake_data_t;

static snake_data_t s_game;

static uint32_t next_random(snake_data_t *g) {
    uint32_t x = g->rng;
    if (!x) x = 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static bool cell_is_snake(const snake_data_t *g, uint16_t cell) {
    for (uint16_t i = 0; i < g->length; ++i)
        if (g->body[i] == cell) return true;
    return false;
}

static void place_food(snake_data_t *g) {
    if (g->length >= GRID_CELLS) { g->state = 2; return; }
    uint16_t cell;
    do { cell = (uint16_t)(next_random(g) % GRID_CELLS); }
    while (cell_is_snake(g, cell));
    g->food = cell;
}

static void start_game(snake_data_t *g) {
    g->length = 4;
    g->body[0] = (GRID_ROWS / 2) * GRID_COLS + GRID_COLS / 2;
    g->body[1] = g->body[0] - 1;
    g->body[2] = g->body[0] - 2;
    g->body[3] = g->body[0] - 3;
    g->direction = DIR_RIGHT;
    g->dir_queue_len = 0;
    g->score = 0;
    g->state = 1;
    place_food(g);
    if (g->api && g->api->get_time_ms) g->last_step = g->api->get_time_ms();
    if (g->api && g->api->request_redraw) g->api->request_redraw();
}

// ⚡ 带有双输入排队保护的转向请求（杜绝快速连拐上->左时误判反向而吞键）
static void request_direction(snake_data_t *g, direction_t dir) {
    if (!g || g->state != 1) return;

    // 比对基准：如果队列有待执行指令，则基于队列末尾的方向判断是否相反
    uint8_t last_dir = (g->dir_queue_len > 0) ? g->dir_queue[g->dir_queue_len - 1] : g->direction;
    if ((uint8_t)dir == last_dir) return; // 方向相同直接忽略
    if ((uint8_t)dir == (uint8_t)((last_dir + 2u) & 3u)) return; // 180度掉头反向忽略

    if (g->dir_queue_len < 2) {
        g->dir_queue[g->dir_queue_len++] = (uint8_t)dir;
    } else {
        // 队列满时覆盖最新拐弯意图
        g->dir_queue[1] = (uint8_t)dir;
    }
}

static void on_start(ui_btn_t *btn, void *ud) { (void)btn; start_game((snake_data_t *)ud); }

static bool point_in_button(const ui_btn_t *btn, int16_t x, int16_t y) {
    return btn && x >= btn->rect.x && x < btn->rect.x + btn->rect.w &&
           y >= btn->rect.y && y < btn->rect.y + btn->rect.h;
}

static bool snake_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_game.api = api;
    s_game.rng = (api->get_time_us ? (uint32_t)api->get_time_us() : 0x19C31u) ^ 0x9E3779B9u;
    s_game.state = 0;
    s_game.length = 0;
    s_game.touch_was_down = false;
    s_game.dir_queue_len = 0;

    if (api->ui_btn_init) {
        /* Keep the start button in the title row and center the D-pad below the board. */
        api->ui_btn_init(&s_game.start, 350, 34, 110, 26, "START / RETRY", on_start);
        api->ui_btn_init(&s_game.btn_up,    CONTROL_CENTER_X, CONTROL_TOP_Y, CONTROL_W, CONTROL_H, "UP",   NULL);
        api->ui_btn_init(&s_game.btn_left,  CONTROL_LEFT_X,   CONTROL_ROW_Y, CONTROL_W, CONTROL_H, "<",    NULL);
        api->ui_btn_init(&s_game.btn_right, CONTROL_RIGHT_X,  CONTROL_ROW_Y, CONTROL_W, CONTROL_H, ">",    NULL);
        api->ui_btn_init(&s_game.btn_down,  CONTROL_CENTER_X, CONTROL_ROW_Y, CONTROL_W, CONTROL_H, "DOWN", NULL);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_game.start, 0x05A0, 0x07E0, COLOR_TEXT);
            api->ui_btn_set_color(&s_game.btn_up,    0x4208, 0x7BEF, COLOR_TEXT);
            api->ui_btn_set_color(&s_game.btn_left,  0x4208, 0x7BEF, COLOR_TEXT);
            api->ui_btn_set_color(&s_game.btn_right, 0x4208, 0x7BEF, COLOR_TEXT);
            api->ui_btn_set_color(&s_game.btn_down,  0x4208, 0x7BEF, COLOR_TEXT);
        }
    }
    s_game.start.user_data = &s_game;
    return true;
}

static void snake_update_core1(void) {
    snake_data_t *g = &s_game;
    if (!g->api || !g->api->get_time_ms) return;
    uint32_t now = g->api->get_time_ms();

    if (g->state == 1 && (uint32_t)(now - g->last_step) >= STEP_MS) {
        g->last_step = now;

        // ⚡ 从预排队缓冲队列中提取下一跳方向
        if (g->dir_queue_len > 0) {
            g->direction = g->dir_queue[0];
            if (g->dir_queue_len == 2) {
                g->dir_queue[0] = g->dir_queue[1];
                g->dir_queue_len = 1;
            } else {
                g->dir_queue_len = 0;
            }
        }

        uint16_t head = g->body[0];
        int16_t x = (int16_t)(head % GRID_COLS);
        int16_t y = (int16_t)(head / GRID_COLS);
        if (g->direction == DIR_UP) --y;
        else if (g->direction == DIR_RIGHT) ++x;
        else if (g->direction == DIR_DOWN) ++y;
        else --x;

        if (x < 0 || x >= GRID_COLS || y < 0 || y >= GRID_ROWS) {
            g->state = 2;
        } else {
            uint16_t next = (uint16_t)(y * GRID_COLS + x);
            bool eating = next == g->food;
            uint16_t check_len = eating ? g->length : (uint16_t)(g->length - 1u);
            for (uint16_t i = 0; i < check_len; ++i)
                if (g->body[i] == next) { g->state = 2; break; }
            if (g->state == 1) {
                if (eating && g->length < GRID_CELLS) {
                    for (uint16_t i = g->length; i > 0; --i) g->body[i] = g->body[i - 1];
                    ++g->length;
                    ++g->score;
                    place_food(g);
                } else {
                    for (uint16_t i = g->length - 1u; i > 0; --i) g->body[i] = g->body[i - 1];
                }
                g->body[0] = next;
            }
        }
        if (g->api->request_redraw) g->api->request_redraw();
    }
    if (g->api->delay_ms) g->api->delay_ms(5);
}

// ⚡ 全屏盲操判定 + 支持贴屏连续画折线拐弯
static void snake_process_touch(bool is_touch, int16_t x, int16_t y) {
    const pico_host_api_t *api = s_game.api;
    if (!api) return;

    // 1. 优先调度右下角开始按钮
    if (api->ui_btn_process_touch) {
        uint32_t now = api->get_time_ms ? api->get_time_ms() : 0;
        api->ui_btn_process_touch(&s_game.start, is_touch, x, y, now);
        api->ui_btn_process_touch(&s_game.btn_up, is_touch, x, y, now);
        api->ui_btn_process_touch(&s_game.btn_left, is_touch, x, y, now);
        api->ui_btn_process_touch(&s_game.btn_right, is_touch, x, y, now);
        api->ui_btn_process_touch(&s_game.btn_down, is_touch, x, y, now);
    }

    // 2. 手指落地：只要没点在右下角按钮上，全屏皆可作为手势起点
    if (is_touch && !s_game.touch_was_down) {
        if (point_in_button(&s_game.btn_up, x, y)) request_direction(&s_game, DIR_UP);
        else if (point_in_button(&s_game.btn_left, x, y)) request_direction(&s_game, DIR_LEFT);
        else if (point_in_button(&s_game.btn_right, x, y)) request_direction(&s_game, DIR_RIGHT);
        else if (point_in_button(&s_game.btn_down, x, y)) request_direction(&s_game, DIR_DOWN);
    }

    s_game.touch_was_down = is_touch;
}

static void draw_cell(ui_tile_t *tile, uint16_t cell, uint16_t color) {
    const pico_host_api_t *api = s_game.api;
    if (!api || !api->ui_draw_fill_rect) return;
    int16_t x = (int16_t)(BOARD_X + (cell % GRID_COLS) * CELL_SIZE + 1);
    int16_t y = (int16_t)(BOARD_Y + (cell / GRID_COLS) * CELL_SIZE + 1);
    ui_rect_t r = ui_rect_make(x, y, CELL_SIZE - 2, CELL_SIZE - 2);
    api->ui_draw_fill_rect(tile, &r, color);
}

static void score_to_text(uint16_t score, char *out) {
    char digits[6];
    uint8_t count = 0;
    do {
        digits[count++] = (char)('0' + (score % 10u));
        score = (uint16_t)(score / 10u);
    } while (score && count < sizeof(digits));
    uint8_t pos = 0;
    out[pos++] = 'S'; out[pos++] = 'C'; out[pos++] = 'O'; out[pos++] = 'R'; out[pos++] = 'E'; out[pos++] = ':'; out[pos++] = ' ';
    while (count) out[pos++] = digits[--count];
    out[pos] = '\0';
}

static void snake_draw(ui_tile_t *tile) {
    const pico_host_api_t *api = s_game.api;
    if (!api || !tile) return;
    if (api->ui_draw_fill_rect) {
        ui_rect_t screen = ui_rect_make(0, 32, 480, 288);
        ui_rect_t board = ui_rect_make(BOARD_X, BOARD_Y, GRID_COLS * CELL_SIZE, GRID_ROWS * CELL_SIZE);
        api->ui_draw_fill_rect(tile, &screen, COLOR_BG);
        api->ui_draw_fill_rect(tile, &board, COLOR_PANEL);
    }
    if (api->ui_draw_fast_hline) {
        for (int16_t row = 0; row <= GRID_ROWS; ++row)
            api->ui_draw_fast_hline(tile, BOARD_X, BOARD_Y + row * CELL_SIZE,
                                    GRID_COLS * CELL_SIZE, COLOR_GRID);
    }
    if (api->ui_draw_fill_rect) {
        for (int16_t col = 0; col <= GRID_COLS; ++col) {
            ui_rect_t line = ui_rect_make(BOARD_X + col * CELL_SIZE, BOARD_Y, 1,
                                          GRID_ROWS * CELL_SIZE);
            api->ui_draw_fill_rect(tile, &line, COLOR_GRID);
        }
    }
    if (s_game.length) {
        draw_cell(tile, s_game.food, COLOR_FOOD);
        for (uint16_t i = s_game.length; i > 1; --i) draw_cell(tile, s_game.body[i - 1], COLOR_SNAKE);
        draw_cell(tile, s_game.body[0], COLOR_HEAD);
    }
    if (api->ui_draw_string) {
        const char *status = s_game.state == 1 ? "SNAKE  RUNNING" :
                             (s_game.state == 2 ? "GAME OVER - TAP DIRECTION" : "SNAKE - TAP DIRECTION");
        char score_text[16];
        score_to_text(s_game.score, score_text);
        api->ui_draw_string(tile, 22, 40, status, FONT_16, COLOR_TEXT, COLOR_BG);
        api->ui_draw_string(tile, 22, 282, score_text, FONT_16, COLOR_TEXT, COLOR_BG);
    }
    if (api->ui_btn_draw) {
        api->ui_btn_draw(tile, &s_game.start);
        api->ui_btn_draw(tile, &s_game.btn_up);
        api->ui_btn_draw(tile, &s_game.btn_left);
        api->ui_btn_draw(tile, &s_game.btn_right);
        api->ui_btn_draw(tile, &s_game.btn_down);
    }
}

static void snake_exit(void) { s_game.api = 0; }

static const app_plugin_t s_snake_plugin = {
    .magic = APP_MAGIC,
    .version = 0x00010000u,
    .name = "Snake",
    .init = snake_init,
    .update_core1 = snake_update_core1,
    .process_touch = snake_process_touch,
    .draw = snake_draw,
    .exit = snake_exit,
};

const app_plugin_t *snake_get_descriptor(void) { return &s_snake_plugin; }

// ⚡ 兼容 PApp 动态加载器统一二进制入口 (0x20038000 偏移 0x00 处)
__attribute__((section(".text.entry"), used))
const app_plugin_t *papp_entry(const pico_host_api_t *api) {
    (void)api;
    return &s_snake_plugin;
}
