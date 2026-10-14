/* =============================================================
 * Game Boy 模拟器 (静态 App) —— 基于 Peanut-GB (60FPS 满血加速版)
 * ============================================================= */

#include "app_base.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "lcd.h"
#include "ff.h"
#include "tf.h"
#include "ui_draw.h"
#include "ui_file_browser.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern app_manager_t g_app_mgr;

/* 捕获主屏幕物理分辨率 480x320 */
enum {
    PL_LCD_WIDTH  = LCD_WIDTH,
    PL_LCD_HEIGHT = LCD_HEIGHT
};
#undef LCD_WIDTH
#undef LCD_HEIGHT

#include "peanut_gb.h"

/* ===================== 布局 ===================== */

#define GB_SCALE        2
#define GB_SRC_W        160
#define GB_SRC_H        144

#define GB_DISP_W       (GB_SRC_W * GB_SCALE)         /* 320 */
#define GB_DISP_H       (GB_SRC_H * GB_SCALE)         /* 288 */

#define GB_DISP_X       0
#define GB_DISP_Y       32

#define PANEL_X         320
#define PANEL_W         (PL_LCD_WIDTH - PANEL_X)      /* 160 */

/* 攒 16 个源行 (放大后 32 显示行) 推一条,每帧 9 条。
 * 144 = 16*9 正好整除,不存在不满的最后一块。
 * 单条 320x32x2B = 20,480B,传输约 2.6ms —— 远小于面板 16.6ms 扫描周期,
 * 所以撕裂不可见。 */
#define BAND_SRC_ROWS   16
#define BAND_DISP_ROWS  (BAND_SRC_ROWS * GB_SCALE)    /* 32 */
#define BANDS_PER_FRAME (GB_SRC_H / BAND_SRC_ROWS)    /* 9 */

/* ===================== 跳帧参数 =====================
 * GB 硬件帧率 59.7Hz,一帧 70224 周期 -> 16.74ms。
 * 用 16ms 作为逻辑步进,轻微偏快约 4.4%,肉眼不可辨;
 * 换来的是整数运算、无浮点、无累积漂移。 */
#define GB_FRAME_MS              16u
#define GB_MAX_FRAMES_PER_LOOP    3u   /* 单轮最多追 3 帧,封顶约 50ms 模拟 */

/* ===================== 配色 (修正 DMA 大小端) ===================== */

#define SWAP16(x)       ((uint16_t)(((uint16_t)(x) << 8) | ((uint16_t)(x) >> 8)))

#define C_DMG0          SWAP16(0xE7B4)  /* 0: 最浅底色 */
#define C_DMG1          SWAP16(0x9D2F)  /* 1: 浅灰绿   */
#define C_DMG2          SWAP16(0x538A)  /* 2: 深森林绿 */
#define C_DMG3          SWAP16(0x1A26)  /* 3: 最深墨黑 */

static const uint16_t k_dmg_pal[4] = { C_DMG0, C_DMG1, C_DMG2, C_DMG3 };

#define C_PANEL_BG      0x10A2
#define C_BTN           0x39E7
#define C_BTN_ON        0x07E0
#define C_TEXT          0xFFFF
#define C_WARN          0xF800

/* ===================== 极速 ROM 页缓存架构 =====================
 * ⚡ 彻底解决 Bank 乒乓抖动：
 *   - Bank 0 (0x0000 ~ 0x3FFF, 16KB) 常驻 s_rom_bank0；
 *   - 0x4000 之后采用 8 路 × 2KB 页缓存 (共 16KB)，用页号哈希 + 2路组相联，
 *     即使同一帧跨越多个 Bank 也不会反复重读 TF 卡！
 */

#define ROM_BANK0_SZ    0x4000u         /* 16KB 常驻 Bank 0 */
#define PAGE_SHIFT      11u             /* 2^11 = 2048B (2KB 每页) */
#define PAGE_SIZE       (1u << PAGE_SHIFT)
#define PAGE_MASK       (PAGE_SIZE - 1u)
#define CACHE_PAGES     8u              /* 8 页 × 2KB = 16KB 动态缓存 */

#define CART_RAM_MAX    (8u * 1024u)

static uint8_t  *s_rom_bank0   = NULL;  /* 16KB 常驻 */
static uint8_t  *s_page_pool   = NULL;  /* 16KB 分页池 (8 * 2KB) */
static uint32_t  s_page_tag[CACHE_PAGES];
static uint32_t  s_page_lru[CACHE_PAGES];
static uint32_t  s_access_tick = 0;

static FIL       s_rom_file;
static bool      s_rom_open     = false;
static uint32_t  s_rom_size     = 0;

static uint8_t     *s_cart_ram   = NULL;
static bool         s_cart_dirty = false;
static struct gb_s *s_gb         = NULL;

/* 行缓冲:按最大情况(2x, 320 宽 × 32 显示行)分配 = 20,480 B。
 * 1x 模式复用同一块内存,只是每行只用前 160 个像素。 */
static uint16_t    *s_band       = NULL;

/* 渲染累加器:当前块内已填的源行数,以及本帧已推出去的显示行数。
 * 用"显示行数"判满,1x/2x 共用同一套分块逻辑。 */
static uint32_t  s_band_src_row  = 0;   /* 0..GB_SRC_H */
static uint32_t  s_band_disp_row = 0;   /* 0..GB_DISP_H */

/* ===================== 状态机 ===================== */

typedef enum { ST_BROWSE = 0, ST_RUN, ST_ERROR } gb_state_t;

typedef struct {
    ui_rect_t         dirty_rect;       /* 首位：供 app_manager 读取脏矩形 */

    gb_state_t        state;
    bool              need_menu_redraw;
    bool              need_pad_redraw;
    uint8_t           err_code;
    bool              is_cgb_only;

    ui_file_browser_t fb;
    FATFS             fs;
    bool              fs_mounted;

    bool              skip_carried_touch;
    uint8_t           pad_state;
    uint32_t          frames;           /* 累计显示帧数 */

    /* ---- 跳帧解耦状态 ----
     * GB 硬件 59.7Hz(一帧 70224 周期),但面板只能推约 25 FPS。
     * 若每轮循环只跑一帧模拟,游戏逻辑就会跟着掉到 25Hz —— 变慢放。
     * 解法:按真实时间跑够该跑的模拟帧,但只在"显示帧"推送像素。 */
    bool              draw_this_frame;  /* true = 本帧绘制并推屏 */
    uint32_t          last_tick;        /* 上次推进的墙钟时刻 (ms) */
} app_gb_state_t;

static app_gb_state_t *s_d = NULL;
static app_t           s_app_inst;

/* ===================== O(1) 极速 ROM 页读取回调 ===================== */

static inline __attribute__((always_inline))
uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    /* 缓冲还没分配好时返回 0xFF(未编程的 flash 内容),不要裸解引用 NULL */
    if (!s_rom_bank0 || !s_page_pool) return 0xFF;

    /* 1. 前 16KB 常驻内存，单周期直接返回 */
    if (__builtin_expect(addr < ROM_BANK0_SZ, 1)) {
        return s_rom_bank0[addr];
    }

    /* 2. 2KB 页缓存查找 */
    uint32_t page_id = addr >> PAGE_SHIFT;
    /* 2-way 组相联索引 (set = 0..3, 每组 2 个槽位) */
    uint32_t set_idx = (page_id & 3u) << 1;
    uint32_t slot0   = set_idx;
    uint32_t slot1   = set_idx + 1u;

    if (__builtin_expect(s_page_tag[slot0] == page_id, 1)) {
        s_page_lru[slot0] = ++s_access_tick;
        return s_page_pool[(slot0 << PAGE_SHIFT) + (addr & PAGE_MASK)];
    }
    if (__builtin_expect(s_page_tag[slot1] == page_id, 1)) {
        s_page_lru[slot1] = ++s_access_tick;
        return s_page_pool[(slot1 << PAGE_SHIFT) + (addr & PAGE_MASK)];
    }

    /* 3. 未命中：挑 LRU 较老的槽位，只从 TF 卡读 2KB (仅 4 个扇区，耗时 < 1ms) */
    uint32_t victim = (s_page_lru[slot0] <= s_page_lru[slot1]) ? slot0 : slot1;
    uint8_t *dst    = &s_page_pool[victim << PAGE_SHIFT];
    uint32_t file_off = page_id << PAGE_SHIFT;

    if (file_off < s_rom_size) {
        UINT br = 0;
        if (f_lseek(&s_rom_file, file_off) == FR_OK) {
            f_read(&s_rom_file, dst, PAGE_SIZE, &br);
        }
        if (br < PAGE_SIZE) {
            memset(dst + br, 0xFF, PAGE_SIZE - br);
        }
    } else {
        memset(dst, 0xFF, PAGE_SIZE);
    }

    s_page_tag[victim] = page_id;
    s_page_lru[victim] = ++s_access_tick;
    return dst[addr & PAGE_MASK];
}

static uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr >= CART_RAM_MAX) return 0xFF;
    return s_cart_ram[addr];
}

static void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr,
                                 const uint8_t val)
{
    (void)gb;
    if (addr >= CART_RAM_MAX) return;
    if (s_cart_ram[addr] != val) {
        s_cart_ram[addr] = val;
        s_cart_dirty = true;
    }
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t val)
{
    (void)gb; (void)val;
    s_d->err_code = (uint8_t)err;
    s_d->state    = ST_ERROR;
    s_d->need_menu_redraw = true;
}

/* ===================== 32 位成对写入逐行显示回调 ===================== */

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels,
                             const uint_fast8_t line)
{
    (void)gb;
    (void)line;                 /* 用自己维护的源行序号,不依赖 line */

    if (!s_band || !s_d) return;

    /* ⚡ 跳帧:非显示帧只推进逻辑,不合成也不推屏。
     * 这一句是跳帧真正省下带宽的关键 —— 省掉整条的 SPI 推送。 */
    if (!s_d->draw_this_frame) return;

    const uint32_t gb_row = s_band_src_row;      /* 0..143 */
    if (gb_row >= GB_SRC_H) return;

    /* 横向 ×2 放大等价于写一个 32 位整数 (c<<16|c),内存带宽利用翻倍。
     * 调色板已用 SWAP16 预交换,所以这里直接写即可,无需再 bswap。 */
    uint32_t *d0 = (uint32_t *)&s_band[(gb_row * 2u) * GB_DISP_W];
    uint32_t *d1 = (uint32_t *)&s_band[(gb_row * 2u + 1u) * GB_DISP_W];
    for (uint32_t x = 0; x < GB_SRC_W; ++x) {
        uint16_t c = k_dmg_pal[pixels[x] & 0x03u];
        uint32_t pair = ((uint32_t)c << 16) | c;
        d0[x] = pair;
        d1[x] = pair;
    }

    ++s_band_src_row;

    /* 攒满 16 个源行(放大后 32 显示行)推一条。144 = 16*9,正好整除,
     * 所以不存在不满的最后一块。 */
    if (s_band_src_row >= BAND_SRC_ROWS) {
        uint16_t y = (uint16_t)(GB_DISP_Y + s_band_disp_row);
        LCD_PushSubTile((uint16_t)GB_DISP_X, y, (uint16_t)GB_DISP_W,
                        (uint16_t)BAND_DISP_ROWS, s_band, (uint16_t)GB_DISP_W, 0, 0);

        s_band_disp_row += BAND_DISP_ROWS;
        s_band_src_row   = 0;
    }
}

/* ===================== 右侧 160px 面板虚拟按键 ===================== */

#define BTN_W       46
#define BTN_H       44
#define BTN_STEP_X  50
#define BTN_STEP_Y  52

/* 面板纵向分区(相对 GB_DISP_Y=32):
 *   76..276  方向/动作键 4 行
 *   288..324 退出按钮
 * 原先退出按钮在 y=294~330,超出屏幕 320 会被裁掉,现已上移。 */
#define PANEL_GRID_Y    (GB_DISP_Y + 44)
#define PANEL_EXIT_Y    (GB_DISP_Y + 256)
#define PANEL_EXIT_H    36

static void app_gb_btn_rect(uint8_t col, uint8_t row, ui_rect_t *r)
{
    const int16_t origin_x = PANEL_X + 7;
    *r = ui_rect_make((int16_t)(origin_x + col * BTN_STEP_X),
                      (int16_t)(PANEL_GRID_Y + row * BTN_STEP_Y),
                      BTN_W, BTN_H);
}

typedef struct {
    uint8_t     col;
    uint8_t     row;
    uint8_t     bit;
    const char *label;
} pad_btn_t;

static const pad_btn_t k_pad[8] = {
    { 1, 0, JOYPAD_UP,     "UP" },
    { 0, 1, JOYPAD_LEFT,   "LT" },
    { 1, 1, JOYPAD_DOWN,   "DN" },
    { 2, 1, JOYPAD_RIGHT,  "RT" },
    { 0, 2, JOYPAD_B,      "B"  },
    { 2, 2, JOYPAD_A,      "A"  },
    { 0, 3, JOYPAD_SELECT, "SE" },
    { 2, 3, JOYPAD_START,  "ST" },
};

static void app_gb_draw_pad(ui_tile_t *tile)
{
    ui_rect_t bg = ui_rect_make(PANEL_X, GB_DISP_Y, PANEL_W, PL_LCD_HEIGHT - GB_DISP_Y);
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &bg, &ov)) return;

    ui_draw_fill_rect(tile, &ov, C_PANEL_BG);

    for (uint8_t i = 0; i < 8; ++i) {
        ui_rect_t r;
        app_gb_btn_rect(k_pad[i].col, k_pad[i].row, &r);
        if (!ui_rect_intersect(&tile->rect, &r, &ov)) continue;

        bool on = (s_d->pad_state & k_pad[i].bit) != 0;
        ui_draw_fill_rect(tile, &ov, on ? C_BTN_ON : C_BTN);
        ui_draw_string(tile, (int16_t)(r.x + 14), (int16_t)(r.y + 14),
                       k_pad[i].label, FONT_16,
                       on ? 0x0000 : C_TEXT, on ? C_BTN_ON : C_BTN);
    }

    ui_rect_t exit_r = ui_rect_make(PANEL_X + 16, PANEL_EXIT_Y, 128, PANEL_EXIT_H);
    if (ui_rect_intersect(&tile->rect, &exit_r, &ov)) {
        ui_draw_fill_rect(tile, &ov, 0x8800);
        ui_draw_string(tile, exit_r.x + 32, exit_r.y + 6, "退出游戏", FONT_16, C_TEXT, 0x8800);
    }
}

static uint8_t app_gb_pad_hit(int16_t x, int16_t y, bool *hit_exit)
{
    *hit_exit = false;

    ui_rect_t exit_r = ui_rect_make(PANEL_X + 16, PANEL_EXIT_Y, 128, PANEL_EXIT_H);
    if (x >= exit_r.x && x < exit_r.x + exit_r.w &&
        y >= exit_r.y && y < exit_r.y + exit_r.h) {
        *hit_exit = true;
        return 0;
    }

    for (uint8_t i = 0; i < 8; ++i) {
        ui_rect_t r;
        app_gb_btn_rect(k_pad[i].col, k_pad[i].row, &r);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            return k_pad[i].bit;
        }
    }
    return 0;
}

/* ===================== 错误提示 ===================== */

static const char *const k_err_msg[] = {
    "启动成功",
    "不支持的卡带类型 (MBC/RAM 超限)",
    "头部校验和不匹配 (ROM 损坏)",
    "初始化失败",
};

static const char *app_gb_err_text(uint8_t code)
{
    if (code < sizeof(k_err_msg) / sizeof(k_err_msg[0])) return k_err_msg[code];
    return "运行期指令错误";
}

static void app_gb_draw_error(ui_tile_t *tile)
{
    ui_rect_t bg = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &bg, &ov)) return;

    ui_draw_fill_rect(tile, &ov, 0x0841);

    ui_draw_string(tile, 24, 96, "Game Boy 启动失败", FONT_24, C_WARN, 0x0841);
    ui_draw_string(tile, 24, 136, app_gb_err_text(s_d->err_code), FONT_16, C_TEXT, 0x0841);

    if (s_d->is_cgb_only) {
        ui_draw_string(tile, 24, 172, "这是 GBC 专用彩色游戏 (0xC0)", FONT_16, 0xC618, 0x0841);
        ui_draw_string(tile, 24, 194, "本模拟器仅支持 DMG 黑白/兼容游戏", FONT_16, 0xC618, 0x0841);
    } else {
        ui_draw_string(tile, 24, 172, "请检查 ROM 文件是否完整", FONT_16, 0xC618, 0x0841);
    }

    ui_draw_string(tile, 24, 240, "点击屏幕任意处返回列表", FONT_16, 0xFFFF, 0x0841);
}

/* ===================== 游戏启停 ===================== */

static void app_gb_stop(void)
{
    if (s_rom_open) {
        f_close(&s_rom_file);
        s_rom_open = false;
    }
    if (s_gb) {
        free(s_gb);
        s_gb = NULL;
    }
    if (s_d) {
        s_d->state = ST_BROWSE;
        s_d->need_menu_redraw = true;
        s_d->pad_state = 0;
    }
}

static bool app_gb_start(const char *path)
{
    app_gb_stop();

    if (!s_rom_bank0 || !s_page_pool || !s_band || !s_cart_ram) {
        s_d->err_code = 3;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }

    if (f_open(&s_rom_file, path, FA_READ) != FR_OK) {
        s_d->err_code = 3;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }
    s_rom_open = true;
    s_rom_size = (uint32_t)f_size(&s_rom_file);
    s_d->frames = 0;
    s_d->err_code = 0;

    if (s_rom_size < ROM_BANK0_SZ) {
        app_gb_stop();
        s_d->err_code = 2;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }

    /* 1. 常驻 Bank 0 (16KB) */
    if (f_lseek(&s_rom_file, 0) != FR_OK) {
        app_gb_stop();
        return false;
    }
    UINT br = 0;
    if (f_read(&s_rom_file, s_rom_bank0, ROM_BANK0_SZ, &br) != FR_OK) {
        app_gb_stop();
        return false;
    }
    if (br < ROM_BANK0_SZ) memset(s_rom_bank0 + br, 0xFF, ROM_BANK0_SZ - br);

    s_d->is_cgb_only = (s_rom_bank0[0x143] == 0xC0);

    /* 2. 重置并预热 Bank 1 的 8 个 2KB 页缓存 (0x4000 ~ 0x7FFF) */
    s_access_tick = 0;
    for (uint32_t i = 0; i < CACHE_PAGES; i++) {
        uint32_t page_id = (ROM_BANK0_SZ >> PAGE_SHIFT) + i; /* 页号 8..15 */
        uint32_t set_idx = (page_id & 3u) << 1;
        uint32_t slot    = set_idx + ((i >= 4) ? 1u : 0u);
        UINT pbr = 0;
        if (f_read(&s_rom_file, &s_page_pool[slot << PAGE_SHIFT], PAGE_SIZE, &pbr) == FR_OK && pbr > 0) {
            s_page_tag[slot] = page_id;
            s_page_lru[slot] = ++s_access_tick;
        } else {
            s_page_tag[slot] = 0xFFFFFFFFu;
            s_page_lru[slot] = 0;
        }
    }

    s_cart_dirty = false;
    memset(s_cart_ram, 0, CART_RAM_MAX);

    s_gb = (struct gb_s *)malloc(sizeof(struct gb_s));
    if (!s_gb) {
        app_gb_stop();
        s_d->err_code = 3;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }
    memset(s_gb, 0, sizeof(struct gb_s));

    enum gb_init_error_e err = gb_init(s_gb, gb_rom_read_cb,
                                       gb_cart_ram_read_cb, gb_cart_ram_write_cb,
                                       gb_error_cb, NULL);
    if (err != GB_INIT_NO_ERROR) {
        app_gb_stop();
        s_d->err_code = (uint8_t)err;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }

    gb_init_lcd(s_gb, lcd_draw_line_cb);
    /* ⚡ 开启 Peanut-GB 原生隔帧交错加速：物理逻辑 60FPS，LCD 推屏减半，杜绝 SPI 阻塞 */
    s_gb->direct.frame_skip = 1;

    uint32_t need = (uint32_t)s_gb->num_ram_banks * 8192u;
    if (need > CART_RAM_MAX) {
        app_gb_stop();
        s_d->err_code = 1;
        s_d->state = ST_ERROR;
        s_d->need_menu_redraw = true;
        return false;
    }

    s_d->pad_state        = 0;
    s_d->state            = ST_RUN;
    s_d->need_menu_redraw = false;
    s_d->need_pad_redraw  = true;

    /* ⚡ 重置跳帧计时。若不清零,上一局留下的旧 last_tick 会让第一轮
     * due 算出很大的值(但仍受 GB_MAX_FRAMES_PER_LOOP 限制),
     * 表现为刚进游戏时画面突进几帧。 */
    s_d->last_tick        = 0;
    s_d->draw_this_frame  = true;
    s_band_src_row        = 0;
    s_band_disp_row       = 0;

    /* 清游戏区 */
    LCD_FillRect(GB_DISP_X, GB_DISP_Y, GB_DISP_W, GB_DISP_H, 0x0000);
    return true;
}

/* ===================== 文件列表扫描 ===================== */

static bool app_gb_name_is_gb(const char *n)
{
    size_t l = strlen(n);
    if (l < 3) return false;
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    if (strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0) {
        return true;
    }
    return false;
}

static void app_gb_on_file_select(ui_file_browser_t *fb, int8_t index,
                                  const ui_file_item_t *item, void *ud)
{
    (void)fb; (void)index; (void)ud;
    if (!item) return;

    char path[64];
    if (strncmp(item->name, "dl/", 3) == 0) {
        snprintf(path, sizeof(path), "0:/download/%s", item->name + 3);
    } else {
        snprintf(path, sizeof(path), "0:/%s", item->name);
    }
    app_gb_start(path);
}

static void app_gb_scan_roms(void)
{
    ui_file_browser_clear(&s_d->fb);

    DIR dj;
    FILINFO fno;
    if (f_opendir(&dj, "0:/") == FR_OK) {
        while (f_readdir(&dj, &fno) == FR_OK && fno.fname[0] != 0) {
            if (fno.fattrib & AM_DIR) continue;
            if (!app_gb_name_is_gb(fno.fname)) continue;
            if (!ui_file_browser_add_item(&s_d->fb, fno.fname,
                                          (uint32_t)fno.fsize, false)) break;
        }
        f_closedir(&dj);
    }

    if (f_opendir(&dj, "0:/download") == FR_OK) {
        while (f_readdir(&dj, &fno) == FR_OK && fno.fname[0] != 0) {
            if (fno.fattrib & AM_DIR) continue;
            if (!app_gb_name_is_gb(fno.fname)) continue;
            char subname[32];
            snprintf(subname, sizeof(subname), "dl/%s", fno.fname);
            if (!ui_file_browser_add_item(&s_d->fb, subname,
                                          (uint32_t)fno.fsize, false)) break;
        }
        f_closedir(&dj);
    }
}

/* ===================== App 生命周期 ===================== */

static void app_gb_init(app_t *self)
{
    self->user_data = g_static_app_ram;
}

static void app_gb_enter(app_t *self)
{
    s_d = (app_gb_state_t *)g_static_app_ram;
    memset(s_d, 0, sizeof(*s_d));
    self->user_data = s_d;

    if (!s_rom_bank0) s_rom_bank0 = (uint8_t *)malloc(ROM_BANK0_SZ);
    if (!s_page_pool) s_page_pool = (uint8_t *)malloc(CACHE_PAGES * PAGE_SIZE);
    if (!s_band)      s_band      = (uint16_t *)malloc(BAND_DISP_ROWS * GB_DISP_W * sizeof(uint16_t));
    if (!s_cart_ram)  s_cart_ram  = (uint8_t *)malloc(CART_RAM_MAX);

    /* ⚡ 必须检查 malloc 结果!
     * gb_rom_read_cb 里是 s_rom_bank0[addr] 这种裸解引用,
     * 分配失败就会空指针崩溃。堆实测约 79KB,本 app 要 60KB + gb_s(约 20KB),
     * 余量很紧,必须挡一道,失败就停在错误页而不是崩。 */
    if (!s_rom_bank0 || !s_page_pool || !s_band || !s_cart_ram) {
        s_d->state    = ST_ERROR;
        s_d->err_code = 1;
        if (s_rom_bank0) { free(s_rom_bank0); s_rom_bank0 = NULL; }
        if (s_page_pool) { free(s_page_pool); s_page_pool = NULL; }
        if (s_band)      { free(s_band);      s_band      = NULL; }
        if (s_cart_ram)  { free(s_cart_ram);  s_cart_ram  = NULL; }
        s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
        return;
    }

    s_d->skip_carried_touch = true;

    TF_Init();
    if (f_mount(&s_d->fs, "0:", 1) == FR_OK) {
        s_d->fs_mounted = true;
    }

    ui_file_browser_init(&s_d->fb, 8, 72, PL_LCD_WIDTH - 16, PL_LCD_HEIGHT - 84,
                         app_gb_on_file_select);
    app_gb_scan_roms();

    s_d->state = ST_BROWSE;
    s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
}

static void app_gb_exit(app_t *self)
{
    (void)self;
    app_gb_stop();

    if (s_d && s_d->fs_mounted) {
        f_mount(NULL, "0:", 1);
        s_d->fs_mounted = false;
    }

    if (s_rom_bank0) { free(s_rom_bank0); s_rom_bank0 = NULL; }
    if (s_page_pool) { free(s_page_pool); s_page_pool = NULL; }
    if (s_band)      { free(s_band);      s_band      = NULL; }
    if (s_cart_ram)  { free(s_cart_ram);  s_cart_ram  = NULL; }

    /* ⚠️ 绝对不要把 user_data 设为 NULL!
     * app_manager_process_touch_dirty() 在 App 的 process_touch() 返回 true 时
     * 会无条件执行 *out_dirty = *(ui_rect_t *)cur->user_data;  —— 它不检查 NULL。
     * 而且 app_manager 用的是指针数组, cur 始终是同一个 s_app_inst,
     * 所以这里设 NULL 会永久生效, 下次触摸直接空指针解引用 -> 死机。
     *
     * 正确做法: 保持 user_data 指向静态池, 只把状态复位。 */
    if (s_d) {
        s_d->state            = ST_BROWSE;
        s_d->pad_state        = 0;
        s_d->draw_this_frame  = false;
        s_d->last_tick        = 0;
        s_d->dirty_rect       = ui_rect_make(0, 0, 0, 0);
    }
}

static void app_gb_update(app_t *self, uint32_t now_tick)
{
    (void)self; (void)now_tick;
    if (!s_d) return;

    s_d->dirty_rect = ui_rect_make(0, 0, 0, 0);

    if (s_d->need_menu_redraw) {
        s_d->need_menu_redraw = false;
        s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
        return;
    }

    if (s_d->need_pad_redraw) {
        s_d->need_pad_redraw = false;
        s_d->dirty_rect = ui_rect_make(PANEL_X, GB_DISP_Y, PANEL_W, PL_LCD_HEIGHT - GB_DISP_Y);
    }

    if (s_d->state != ST_RUN || !s_gb) return;

    s_gb->direct.joypad = (uint8_t)~s_d->pad_state;

    /* ================= 跳帧解耦 =================
     * 面板推一帧 320x288 要约 25ms @62.5MHz SPI,而 GB 逻辑要 59.7Hz。
     * 如果每轮循环只跑一帧模拟,游戏就会跟着显示掉到约 25Hz(慢放一半多)。
     *
     * 这里按**真实经过时间**决定该跑几帧模拟,保证游戏速度正确;
     * 但只有最后一帧才真正绘制推屏,中间帧走 draw_this_frame=false,
     * lcd_draw_line_cb 直接返回,省掉全部 SPI 传输。
     *
     * 净效果:游戏以 60Hz 精度运行(音乐/计时/物理正确),
     *         画面以面板能承受的帧率刷新 —— 即"丢帧"而非"慢放"。 */
    if (s_d->last_tick == 0) s_d->last_tick = now_tick;

    uint32_t elapsed = now_tick - s_d->last_tick;

    /* 用除法算上限再取最小值,不要用 if 覆写 elapsed —— 那会把余数抹掉,导致时间漂移 */
    uint32_t due = elapsed / GB_FRAME_MS;
    if (due > GB_MAX_FRAMES_PER_LOOP) due = GB_MAX_FRAMES_PER_LOOP;
    if (due == 0) due = 1;          /* 至少推进一帧,别让游戏停住 */

    /* 先跑掉前面的帧(不绘制),最后一帧才绘制推屏。
     * 渲染累加器在每帧开始前清零(lcd_draw_line_cb 里累加)。 */
    for (uint32_t i = 0; i < due; ++i) {
        s_d->draw_this_frame = (i == (due - 1u));
        s_band_src_row  = 0;
        s_band_disp_row = 0;
        gb_run_frame(s_gb);
    }

    /* ⚡ last_tick 只前进"真正模拟掉"的时间,余数留到下一轮 —— 这样不会累积漂移。
     * 若长期追不上(due 一直被上限截断),last_tick 会自然落后,
     * 但配合下面的"间隔过大就重置"仍能恢复。 */
    s_d->last_tick += due * GB_FRAME_MS;

    /* 长时间停顿(例如回到列表页再进来)会让 last_tick 严重过期,
     * 下一轮会连续补帧造成突进。超过 0.5s 就重新对齐。 */
    if ((now_tick - s_d->last_tick) > 500u) {
        s_d->last_tick = now_tick;
    }

    /* ⚡ 核心提速技巧:若本帧无面板重绘,设置一个 1x1 像素的微脏矩形 (落在右侧面板边角)，
     * 欺骗主循环 pico.cpp 保持 16666us (60 FPS) 周期，绝不掉入 50000us (20 FPS) 待机休眠！ */
    if (s_d->dirty_rect.w == 0) {
        s_d->dirty_rect = ui_rect_make(PANEL_X, GB_DISP_Y, 1, 1);
    }

    ++s_d->frames;
}

static bool app_gb_process_touch(app_t *self, bool is_touching, int16_t tx,
                                 int16_t ty, uint32_t now_tick)
{
    (void)self;
    if (!s_d) return false;

    if (s_d->skip_carried_touch) {
        if (!is_touching) s_d->skip_carried_touch = false;
        return false;
    }

    if (s_d->state == ST_ERROR) {
        if (is_touching) {
            s_d->state = ST_BROWSE;
            s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
            s_d->skip_carried_touch = true;
            return true;
        }
        return false;
    }

    if (s_d->state == ST_BROWSE) {
        bool r = ui_file_browser_process_touch(&s_d->fb, is_touching, tx, ty, now_tick);
        if (s_d->state == ST_RUN) {
            s_d->dirty_rect = ui_rect_make(PANEL_X, GB_DISP_Y, PANEL_W, PL_LCD_HEIGHT - GB_DISP_Y);
            return true;
        } else if (s_d->state == ST_ERROR) {
            s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
            return true;
        } else if (r) {
            s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
            return true;
        }
        return false;
    }

    if (is_touching) {
        bool hit_exit  = false;
        uint8_t next = app_gb_pad_hit(tx, ty, &hit_exit);

        if (hit_exit) {
            app_gb_stop();
            s_d->skip_carried_touch = true;
            s_d->dirty_rect = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
            return true;
        }
        if (next != s_d->pad_state) {
            s_d->pad_state = next;
            s_d->dirty_rect = ui_rect_make(PANEL_X, GB_DISP_Y, PANEL_W, PL_LCD_HEIGHT - GB_DISP_Y);
            return true;
        }
    } else if (s_d->pad_state != 0) {
        s_d->pad_state = 0;
        s_d->dirty_rect = ui_rect_make(PANEL_X, GB_DISP_Y, PANEL_W, PL_LCD_HEIGHT - GB_DISP_Y);
        return true;
    }

    return false;
}

static void app_gb_draw(app_t *self, ui_tile_t *tile)
{
    (void)self;
    if (!tile || !s_d) return;

    if (s_d->state == ST_ERROR) {
        app_gb_draw_error(tile);
        return;
    }
    if (s_d->state == ST_BROWSE) {
        ui_rect_t bg = ui_rect_make(0, 32, PL_LCD_WIDTH, PL_LCD_HEIGHT - 32);
        ui_rect_t ov;
        if (ui_rect_intersect(&tile->rect, &bg, &ov)) {
            ui_draw_fill_rect(tile, &ov, 0x0841);
            ui_draw_string(tile, 8, 40, "GB 游戏列表 (支持 0:/ 与 0:/download)",
                           FONT_16, 0xFFFF, 0x0841);
            ui_file_browser_draw(tile, &s_d->fb);
        }
        return;
    }

    /* ⚡ 如果只是为了保活 60FPS 的 1x1 像素微脏矩形，直接画 1 像素背景即可返回，0 开销！ */
    if (tile->rect.w == 1 && tile->rect.h == 1) {
        ui_draw_fill_rect(tile, &tile->rect, C_PANEL_BG);
        return;
    }

    /* host 主动整屏重画时(状态栏遥测、切页)会把每个切片过一遍 draw()。
     * 游戏区是模拟器直接推 LCD 的,不走切片管线,所以这里要补一次底色,
     * 否则会被切片缓冲里的陈旧内容覆盖。 */
    ui_rect_t game = ui_rect_make(GB_DISP_X, GB_DISP_Y, GB_DISP_W, GB_DISP_H);
    ui_rect_t gov;
    if (ui_rect_intersect(&tile->rect, &game, &gov)) {
        ui_draw_fill_rect(tile, &gov, 0x0000);
    }

    app_gb_draw_pad(tile);
}

app_t *app_gb_get_instance(void)
{
    s_app_inst.id            = APP_ID_CUSTOM_0 + 5;
    s_app_inst.name          = "游戏机";
    s_app_inst.user_data     = g_static_app_ram;
    s_app_inst.init          = app_gb_init;
    s_app_inst.enter         = app_gb_enter;
    s_app_inst.exit          = app_gb_exit;
    s_app_inst.update        = app_gb_update;
    s_app_inst.process_touch = app_gb_process_touch;
    s_app_inst.draw          = app_gb_draw;
    return &s_app_inst;
}