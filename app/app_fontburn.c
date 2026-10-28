/* =============================================================
 * 字库烧录 App —— 把 TF 卡上的 GB2312 字库写进 W25Q64
 *
 * 为什么需要它:
 *   项目里 W25_WriteData / W25_EraseBlock64K 一直存在但没人调用,
 *   字库一直是靠外接 STM32 烧录器写的。既然 W25Q64 就挂在 Pico 的
 *   SPI1 上,直接让 Pico 自己烧最省事:TF 卡放 bin → 点一下 → 烧完。
 *
 * 三个字库文件的地址必须与 Drivers/chinese.h 严格一致:
 *   gb2312_16x16.bin -> FONT16_BASE_ADDR = 0x000000  (261,696 B)
 *   gb2312_20x20.bin -> FONT20_BASE_ADDR = 0x040000  (490,680 B)
 *   gb2312_24x24.bin -> FONT24_BASE_ADDR = 0x0C0000  (588,816 B)
 *
 * ⚠️ 烧录过程中 W25Q64 的内容会被破坏,所以本 App 的界面文字
 *    **全部使用 ASCII** —— ASCII 走固件内置的 Font5x7,不读 W25 字库,
 *    因此字库烧坏期间界面依然可读。
 *
 * 流程:擦除(64KB 块) → 写入(2KB 块) → 读回校验(2KB 块)
 *       校验不通过会报出首个不一致的地址。
 * ============================================================= */

#include "app_base.h"
#include "app_shared_ram.h"
#include "app_manager.h"
#include "lcd.h"
#include "ff.h"
#include "w25q64.h"
#include "spi_bus.h"
#include "ui_draw.h"
#include "chinese.h"

#include <string.h>

/* ===================== 布局 ===================== */

#define SCR_W        480
#define SCR_H        320

#define TXT_X        16
#define TITLE_Y      38
#define ROW_Y0       64
#define ROW_DY       24
#define STAT_Y       146
#define HINT_Y       170
#define PCT_Y        210
#define BAR_Y        232

#define BAR_X        16
#define BAR_W        (SCR_W - 32)
#define BAR_H        22

#define BTN_X        16
#define BTN_Y        272
#define BTN_W        150
#define BTN_H        28

/* 每次 update() 处理的块大小与块数。
 *
 * ⚡ 内存很紧张:全片 .bss 已占 153KB,堆只剩 62KB,而 Game Boy 模拟器
 *    需要 60.5KB 缓冲 + 约 20KB 的 struct gb_s。这里如果放 8KB+8KB 的
 *    静态缓冲,会直接把堆压到不够模拟器用。
 *
 *    所以缓冲只开 1KB,但一次 update 里循环处理 8 块 —— 效果等同 8KB,
 *    内存只占 2KB。烧录速度不变。 */
#define CHUNK_SIZE   1024u
#define PER_STEP     8u

#define C_BG         0x0841
#define C_TXT        0xFFFF
#define C_DIM        0x8410
#define C_OK         0x07E0
#define C_ERR        0xF800
#define C_WARN       0xFFE0

/* ===================== 字库表 ===================== */

typedef struct {
    const char *name;
    uint32_t    addr;
    uint32_t    size;
} font_ent_t;

static const font_ent_t k_fonts[3] = {
    { "gb2312_16x16.bin", 0x000000u, 261696u },
    { "gb2312_20x20.bin", 0x040000u, 490680u },
    { "gb2312_24x24.bin", 0x0C0000u, 588816u },
};

#define FONT_N  3u

/* ===================== 状态机 ===================== */

typedef enum {
    PH_IDLE = 0,    /* 等待点击 START */
    PH_ERASE,       /* 擦除 */
    PH_BLANK,       /* 擦除后空白检查:读回应全为 0xFF */
    PH_WRITE,       /* 写入 */
    PH_VERIFY,      /* 读回校验 */
    PH_DONE,
    PH_ERROR,
} fb_phase_t;

typedef struct {
    /* ⚠️ 必须第一位: app_manager 会把 user_data 当 ui_rect_t* 解引用 */
    ui_rect_t dirty_rect;

    fb_phase_t phase;
    uint8_t    file_idx;
    uint32_t   cursor;      /* 当前文件内偏移 */
    uint32_t   grand;       /* 全部文件已处理字节数(用于总进度) */
    uint32_t   grand_total;
    uint32_t   err_addr;    /* 校验失败地址 */
    uint32_t   blk_done;    /* 已擦除块数(当前文件) */
    uint32_t   blk_total;   /* 当前文件需要擦除的块数 */
    bool       files_ok[FONT_N];
    bool       fs_ready;
    bool       need_redraw;
    bool       skip_carried_touch;
    uint32_t   jedec;       /* JEDEC ID:确认到底是哪颗 flash */
    uint8_t    sr1;         /* W25 状态寄存器1:bit2..5 = 块保护 BP2..BP0 */
    uint8_t    sr2;         /* bit7 = CMP:CMP=1 且 BP=0 时整片被保护 */
    uint8_t    sr3;
    bool       sr1_ok;
    bool       protected_st;  /* 当前是否处于写保护 */
    bool       slow_clk;      /* true = 20MHz 烧录(更稳) */
    uint8_t    redraw_div;  /* 烧录期间的重绘分频计数 */
    char       status[64];  /* ASCII 状态文字 */
} fb_state_t;

static fb_state_t s_fb;
static app_t      s_inst;
static FATFS      s_fs;

/* 写/校验缓冲。2KB 够 W25_WriteData 内部按 256B 页拆分。 */
static uint8_t s_buf[CHUNK_SIZE];
static uint8_t s_vbuf[CHUNK_SIZE];

/* ===================== 工具 ===================== */

static void fb_log(const char *s)
{
    size_t i = 0;
    while (s[i] && i < sizeof(s_fb.status) - 1) {
        s_fb.status[i] = s[i];
        ++i;
    }
    s_fb.status[i] = '\0';
    s_fb.need_redraw = true;
}

/* 拼一个 ASCII 十进制到 buf,返回写入长度 */
static int u32_to_dec(uint32_t v, char *buf, int cap)
{
    char t[12];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < 12) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int i = 0;
    while (n > 0 && i < cap - 1) buf[i++] = t[--n];
    buf[i] = '\0';
    return i;
}

/* 生成 "0:/gb2312_16x16.bin" 这样的路径,先试根目录再试 download */
static bool open_font(FIL *f, const char *name)
{
    char path[64];
    /* 0:/xxx */
    path[0] = '0'; path[1] = ':'; path[2] = '/';
    size_t n = 0;
    while (name[n] && n < sizeof(path) - 4) { path[3 + n] = name[n]; ++n; }
    path[3 + n] = '\0';
    if (f_open(f, path, FA_READ) == FR_OK) return true;

    /* 0:/download/xxx */
    const char *pre = "0:/download/";
    size_t p = 0;
    while (pre[p]) { path[p] = pre[p]; ++p; }
    n = 0;
    while (name[n] && p + n < sizeof(path) - 1) { path[p + n] = name[n]; ++n; }
    path[p + n] = '\0';
    return f_open(f, path, FA_READ) == FR_OK;
}

/* ===================== 烧录核心 ===================== */

static void fb_reset_state(void)
{
    s_fb.phase      = PH_IDLE;
    s_fb.file_idx   = 0;
    s_fb.cursor     = 0;
    s_fb.grand      = 0;
    s_fb.err_addr   = 0;
    s_fb.need_redraw = true;
    s_fb.grand_total = 0;
    for (unsigned i = 0; i < FONT_N; ++i) s_fb.grand_total += k_fonts[i].size;
    fb_log("Ready. Tap START to burn.");
}

static void fb_start(void)
{
    s_fb.phase    = PH_ERASE;
    s_fb.file_idx = 0;
    s_fb.cursor   = 0;
    s_fb.grand    = 0;
    s_fb.need_redraw = true;
    fb_log("Erasing...");
}

/* 每个 update 推进一小步,返回后由主循环重绘 */
static void fb_step(void)
{
    if (s_fb.phase != PH_ERASE && s_fb.phase != PH_BLANK &&
        s_fb.phase != PH_WRITE && s_fb.phase != PH_VERIFY) return;
    if (s_fb.file_idx >= FONT_N) { s_fb.phase = PH_DONE; return; }

    const font_ent_t *fe = &k_fonts[s_fb.file_idx];
    FIL f;

    /* ---- 擦除:一次一个 64KB 块 ---- */
    if (s_fb.phase == PH_ERASE) {
        if (s_fb.cursor == 0) {
            /* 只在进入擦除阶段时读一次文件,验证存在性和长度。
             * 不要每个块都 f_open —— SD 卡目录扫描很慢,会让进度看起来卡住。 */
            if (!open_font(&f, fe->name)) {
                s_fb.phase = PH_ERROR;
                fb_log("ERROR: file not found on TF");
                return;
            }
            FSIZE_t sz = f_size(&f);
            f_close(&f);
            if ((uint32_t)sz != fe->size) {
                s_fb.phase = PH_ERROR;
                fb_log("ERROR: bad file size");
                return;
            }
            s_fb.blk_total = (fe->size + 65535u) / 65536u;
            s_fb.blk_done  = 0;
        }

        /* W25 和 LCD 共用 SPI1:推屏 DMA 还没结束时去读状态寄存器,
         * 读回的值会被总线上的 LCD 数据污染 —— busy 位可能一直是 1,
         * 表现就是"擦除永远不结束"。 */
        LCD_WaitDMA();

        /* ⚡ 擦除命令可能被芯片拒绝(WEL 未置位 / 超时),
         * 必须检查返回值,否则会出现"擦除秒完成、内容一点没变"。 */
        if (!W25_EraseBlock64K(fe->addr + s_fb.cursor)) {
            s_fb.err_addr = fe->addr + s_fb.cursor;
            s_fb.phase = PH_ERROR;
            {
                char t[48];
                int k = 0;
                const char *p = "ERASE CMD FAILED @0x";
                while (*p) t[k++] = *p++;
                char hx[9];
                for (int b = 0; b < 8; ++b) {
                    uint8_t n = (uint8_t)((s_fb.err_addr >> ((7 - b) * 4)) & 0xF);
                    hx[b] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
                }
                hx[8] = '\0';
                p = hx; while (*p) t[k++] = *p++;
                t[k] = '\0';
                fb_log(t);
            }
            return;
        }
        s_fb.cursor += 65536u;
        s_fb.blk_done++;

        /* 状态文字带上块计数,让进度可见 */
        {
            char t[48];
            int k = 0;
            const char *p = "ERASE ";
            while (*p) t[k++] = *p++;
            char num[12];
            u32_to_dec(s_fb.file_idx + 1, num, sizeof(num));
            p = num; while (*p) t[k++] = *p++;
            t[k++] = '/';
            u32_to_dec(FONT_N, num, sizeof(num));
            p = num; while (*p) t[k++] = *p++;
            p = " blk "; while (*p) t[k++] = *p++;
            u32_to_dec(s_fb.blk_done, num, sizeof(num));
            p = num; while (*p) t[k++] = *p++;
            t[k++] = '/';
            u32_to_dec(s_fb.blk_total, num, sizeof(num));
            p = num; while (*p) t[k++] = *p++;
            t[k] = '\0';
            fb_log(t);
        }

        /* 擦除进度:按文件大小折算,全部用整数运算(Cortex-M0+ 无 FPU) */
        {
            uint32_t done = (s_fb.cursor > fe->size) ? fe->size : s_fb.cursor;
            s_fb.grand = 0;
            for (unsigned i = 0; i < s_fb.file_idx; ++i) s_fb.grand += k_fonts[i].size;
            s_fb.grand += done;
        }

        if (s_fb.cursor >= fe->size) {
            s_fb.cursor = 0;
            s_fb.phase  = PH_BLANK;
            fb_log("Blank check...");
        }
        s_fb.need_redraw = true;
        return;
    }

    /* ---- 擦除后空白检查:整个文件区域必须是 0xFF ---- */
    if (s_fb.phase == PH_BLANK) {
        for (uint32_t n = 0; n < PER_STEP && s_fb.cursor < fe->size; ++n) {
            uint32_t remain = fe->size - s_fb.cursor;
            uint32_t chunk  = remain < CHUNK_SIZE ? remain : CHUNK_SIZE;

            LCD_WaitDMA();
            W25_FastReadData(fe->addr + s_fb.cursor, s_vbuf, chunk);

            for (uint32_t i = 0; i < chunk; ++i) {
                if (s_vbuf[i] != 0xFFu) {
                    s_fb.err_addr = fe->addr + s_fb.cursor + i;
                    s_fb.phase = PH_ERROR;
                    char t[48];
                    int k = 0;
                    const char *p = "ERASE FAIL @0x";
                    while (*p) t[k++] = *p++;
                    char hx[9];
                    for (int b = 0; b < 8; ++b) {
                        uint8_t d = (uint8_t)((s_fb.err_addr >> ((7 - b) * 4)) & 0xF);
                        hx[b] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
                    }
                    hx[8] = '\0';
                    p = hx; while (*p) t[k++] = *p++;
                    t[k] = '\0';
                    fb_log(t);
                    return;
                }
            }
            s_fb.cursor += chunk;
        }

        if (s_fb.cursor >= fe->size) {
            s_fb.cursor = 0;
            s_fb.phase  = PH_WRITE;
            fb_log("Writing...");
        } else {
            fb_log("Blank check...");
        }
        s_fb.need_redraw = true;
        return;
    }

    /* ---- 写入 / 校验:缓冲 1KB,一次 update 里连做 PER_STEP 块 ---- */
    if (!open_font(&f, fe->name)) {
        s_fb.phase = PH_ERROR;
        fb_log("ERROR: open failed");
        return;
    }
    if (f_lseek(&f, s_fb.cursor) != FR_OK) {
        f_close(&f);
        s_fb.phase = PH_ERROR;
        fb_log("ERROR: seek failed");
        return;
    }

    for (uint32_t n = 0; n < PER_STEP && s_fb.cursor < fe->size; ++n) {
        uint32_t remain = fe->size - s_fb.cursor;
        uint32_t chunk  = remain < CHUNK_SIZE ? remain : CHUNK_SIZE;

        UINT br = 0;
        if (f_read(&f, s_buf, chunk, &br) != FR_OK || br != chunk) {
            f_close(&f);
            s_fb.phase = PH_ERROR;
            fb_log("ERROR: read TF failed");
            return;
        }

        if (s_fb.phase == PH_WRITE) {
            LCD_WaitDMA();   /* 同上:避开与 LCD DMA 的 SPI1 总线竞争 */
            uint32_t written = W25_WriteData(fe->addr + s_fb.cursor, s_buf, chunk);
            if (written != chunk) {
                f_close(&f);
                s_fb.err_addr = fe->addr + s_fb.cursor + written;
                s_fb.phase = PH_ERROR;
                char tmp[48];
                int k = 0;
                const char *p1 = "WRITE FAIL @0x";
                while (*p1) tmp[k++] = *p1++;
                char hx[9];
                for (int i = 0; i < 8; ++i) {
                    uint8_t d = (uint8_t)((s_fb.err_addr >> ((7 - i) * 4)) & 0xF);
                    hx[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
                }
                hx[8] = '\0';
                p1 = hx;
                while (*p1) tmp[k++] = *p1++;
                tmp[k] = '\0';
                fb_log(tmp);
                return;
            }
        } else {
            /* 校验:读回比对 */
            LCD_WaitDMA();
            W25_FastReadData(fe->addr + s_fb.cursor, s_vbuf, chunk);
            if (memcmp(s_buf, s_vbuf, chunk) != 0) {
                f_close(&f);
                uint32_t bad = 0;
                while (bad < chunk && s_buf[bad] == s_vbuf[bad]) ++bad;
                s_fb.err_addr = fe->addr + s_fb.cursor + bad;
                s_fb.phase = PH_ERROR;
                char tmp[48];
                int k = 0;
                const char *p1 = "VERIFY FAIL @0x";
                while (*p1) tmp[k++] = *p1++;
                char hx[9];
                for (int i = 0; i < 8; ++i) {
                    uint8_t d = (uint8_t)((s_fb.err_addr >> ((7 - i) * 4)) & 0xF);
                    hx[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
                }
                hx[8] = '\0';
                p1 = hx;
                while (*p1) tmp[k++] = *p1++;
                tmp[k] = '\0';
                fb_log(tmp);
                return;
            }
        }

        s_fb.cursor += chunk;
        s_fb.grand  += chunk;
    }
    f_close(&f);

    s_fb.need_redraw = true;

    if (s_fb.cursor >= fe->size) {
        s_fb.cursor = 0;
        if (s_fb.phase == PH_WRITE) {
            s_fb.phase = PH_VERIFY;
            fb_log("Verifying...");
        } else {
            /* 这个文件完成了 */
            s_fb.file_idx++;
            if (s_fb.file_idx >= FONT_N) {
                s_fb.phase = PH_DONE;
                /* 字库变了,必须让字模缓存失效,否则仍会显示旧字形 */
                Chinese_Init();
                fb_log("DONE. Font cache reloaded.");
            } else {
                s_fb.phase = PH_ERASE;
                fb_log("Next file: erasing...");
            }
        }
    }
}

/* ===================== 触控 ===================== */

static bool hit(int16_t x, int16_t y, int16_t bx, int16_t by, int16_t bw, int16_t bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static bool fb_process_touch(app_t *self, bool touching, int16_t x, int16_t y, uint32_t now)
{
    (void)self; (void)now;

    if (s_fb.skip_carried_touch) {
        if (!touching) s_fb.skip_carried_touch = false;
        return false;
    }
    if (!touching) return false;
    if (s_fb.phase == PH_ERASE || s_fb.phase == PH_BLANK || s_fb.phase == PH_WRITE || s_fb.phase == PH_VERIFY) return false;

    if (hit(x, y, BTN_X, BTN_Y, BTN_W, BTN_H)) {
        fb_reset_state();
        fb_start();
        s_fb.dirty_rect = ui_rect_make(0, 32, SCR_W, SCR_H - 32);
        g_app_mgr.needs_full_redraw = true;
        return true;
    }

    /* 时钟档位按钮:切 62.5MHz / 20MHz */
    if (hit(x, y, SCR_W - 210, BTN_Y, 194, BTN_H)) {
        s_fb.slow_clk = !s_fb.slow_clk;
        W25_SetBaud(s_fb.slow_clk ? 20000000u : SPI_BUS_BAUD_HIGH);
        fb_log(s_fb.slow_clk ? "Clock: 20MHz (safer for writes)"
                             : "Clock: 62.5MHz (fast)");
        g_app_mgr.needs_full_redraw = true;
        return true;
    }

    /* CLEAR PROTECT 按钮:仅在检测到写保护时出现 */
    if (s_fb.protected_st &&
        hit(x, y, BTN_X + BTN_W + 12, BTN_Y, 190, BTN_H)) {
        bool ok = W25_ClearBlockProtect();
        s_fb.sr1 = W25_ReadStatus1();
        s_fb.sr2 = W25_ReadStatus2();
        s_fb.sr3 = W25_ReadStatus3();
        s_fb.protected_st = W25_IsProtected();
        if (ok && !s_fb.protected_st) {
            fb_log("Protection cleared. Tap START.");
        } else {
            fb_log("Clear failed: chip ignores SR writes");
        }
        g_app_mgr.needs_full_redraw = true;
        return true;
    }
    return false;
}

/* ===================== 绘制 ===================== */

static void txt(ui_tile_t *tile, int16_t x, int16_t y, const char *s, uint16_t col)
{
    ui_draw_string(tile, x, y, s, FONT_16, col, C_BG);
}

static void fb_draw(app_t *self, ui_tile_t *tile)
{
    (void)self;
    if (!tile) return;

    /* 整屏底色(切片安全: ui_draw_fill_rect 内部按 tile/clip 裁剪) */
    ui_rect_t bg = ui_rect_make(0, 32, SCR_W, SCR_H - 32);
    ui_rect_t ov;
    if (!ui_rect_intersect(&tile->rect, &bg, &ov)) return;
    ui_draw_fill_rect(tile, &ov, C_BG);

    /* 标题 + 文件状态 */
    int16_t y = ROW_Y0;
    txt(tile, TXT_X, TITLE_Y, "FONT BURNER  (TF -> W25Q64)", C_TXT);

    for (unsigned i = 0; i < FONT_N; ++i) {
        const font_ent_t *fe = &k_fonts[i];
        uint16_t col;
        const char *mark;
        if (s_fb.phase == PH_DONE) { col = C_OK; mark = "[OK] "; }
        else if (s_fb.phase == PH_ERROR && s_fb.file_idx == i) { col = C_ERR; mark = "[!!] "; }
        else if (s_fb.phase != PH_IDLE && s_fb.file_idx == i) { col = C_WARN; mark = "[..] "; }
        else { col = C_DIM; mark = "[  ] "; }

        char line[48];
        int k = 0;
        while (*mark) line[k++] = *mark++;
        const char *p = fe->name;
        while (*p && k < 40) line[k++] = *p++;
        line[k] = '\0';
        txt(tile, TXT_X, y, line, col);
        y += ROW_DY;
    }

    /* 状态文字 */
    txt(tile, TXT_X, STAT_Y, s_fb.status, C_TXT);

    /* W25 状态寄存器 —— 块保护位若不为 0,擦除/写入会被芯片拒绝。
     * 特别注意 SR2.CMP: CMP=1 且 SR1.BP=0 时 SR1 看起来是 0x00,
     * 但整片 flash 其实是写保护的,极具迷惑性。 */
    if (s_fb.sr1_ok) {
        static const char hexd[] = "0123456789ABCDEF";
        char sb[40];
        int k = 0;
        const char *p = "SR1=";
        while (*p) sb[k++] = *p++;
        sb[k++] = hexd[(s_fb.sr1 >> 4) & 0xF];
        sb[k++] = hexd[s_fb.sr1 & 0xF];
        p = "  SR2="; while (*p) sb[k++] = *p++;
        sb[k++] = hexd[(s_fb.sr2 >> 4) & 0xF];
        sb[k++] = hexd[s_fb.sr2 & 0xF];
        p = "  SR3="; while (*p) sb[k++] = *p++;
        sb[k++] = hexd[(s_fb.sr3 >> 4) & 0xF];
        sb[k++] = hexd[s_fb.sr3 & 0xF];
        sb[k] = '\0';
        txt(tile, TXT_X, STAT_Y + 20, sb, C_DIM);

        if (s_fb.protected_st) {
            txt(tile, TXT_X, STAT_Y + 38,
                "WRITE-PROTECTED! (CMP or BP set)", C_ERR);
            /* 清保护按钮 */
            ui_rect_t cb = ui_rect_make(BTN_X + BTN_W + 12, BTN_Y, 190, BTN_H);
            ui_rect_t cov;
            if (ui_rect_intersect(&tile->rect, &cb, &cov)) {
                ui_draw_fill_rect(tile, &cov, 0xC000);
                txt(tile, cb.x + 12, cb.y + 6, "CLEAR PROTECT", C_TXT);
            }
        } else {
            txt(tile, TXT_X, STAT_Y + 38, "WP ok", C_DIM);
        }
    }

    /* 进度条 */
    ui_rect_t bar_bg = ui_rect_make(BAR_X, BAR_Y, BAR_W, BAR_H);
    ui_rect_t bov;
    if (ui_rect_intersect(&tile->rect, &bar_bg, &bov)) {
        ui_draw_fill_rect(tile, &bov, 0x2104);
        if (s_fb.grand_total > 0) {
            int32_t w = (int32_t)((uint64_t)BAR_W * s_fb.grand / s_fb.grand_total);
            if (w < 0) w = 0;
            if (w > BAR_W) w = BAR_W;
            ui_rect_t fg = ui_rect_make(BAR_X + 2, BAR_Y + 2, w > 4 ? w - 4 : 0, BAR_H - 4);
            ui_rect_t fov;
            if (fg.w > 0 && ui_rect_intersect(&tile->rect, &fg, &fov)) {
                ui_draw_fill_rect(tile, &fov, s_fb.phase == PH_ERROR ? C_ERR : C_OK);
            }
        }
    }

    /* 百分比 */
    char pb[16];
    uint32_t pct = s_fb.grand_total ? (uint32_t)((uint64_t)s_fb.grand * 100u / s_fb.grand_total) : 0;
    int n = u32_to_dec(pct, pb, 8);
    pb[n++] = '%'; pb[n] = '\0';
    txt(tile, BAR_X, PCT_Y, pb, C_TXT);

    /* 按钮 */
    ui_rect_t btn = ui_rect_make(BTN_X, BTN_Y, BTN_W, BTN_H);
    ui_rect_t bov2;
    if (ui_rect_intersect(&tile->rect, &btn, &bov2)) {
        bool busy = (s_fb.phase == PH_ERASE || s_fb.phase == PH_BLANK || s_fb.phase == PH_WRITE || s_fb.phase == PH_VERIFY);
        ui_draw_fill_rect(tile, &bov2, busy ? 0x4208 : 0x05E0);
        txt(tile, BTN_X + 14, BTN_Y + 4, busy ? "BURNING..." : "START", C_TXT);
    }

    /* 时钟档位按钮(右上):62.5MHz <-> 20MHz。
     * 若 62.5MHz 下擦除命令被芯片拒绝,切到 20MHz 通常就能成功 —— 
     * 共用 SPI 总线上跑满速时,写方向的时序余量比读小得多。 */
    {
        ui_rect_t cb = ui_rect_make(SCR_W - 210, BTN_Y, 194, BTN_H);
        ui_rect_t cov;
        if (ui_rect_intersect(&tile->rect, &cb, &cov)) {
            bool busy = (s_fb.phase == PH_ERASE || s_fb.phase == PH_BLANK || s_fb.phase == PH_WRITE || s_fb.phase == PH_VERIFY);
            ui_draw_fill_rect(tile, &cov, busy ? 0x4208 : (s_fb.slow_clk ? 0xFFE0 : 0x001F));
            txt(tile, cb.x + 10, cb.y + 4,
                s_fb.slow_clk ? "CLK 20MHz" : "CLK 62.5MHz", 0x0000);
        }
    }

    /* JEDEC ID:确认芯片型号。W25Q64 应为 EF4017。 */
    if (s_fb.sr1_ok) {
        static const char hexd[] = "0123456789ABCDEF";
        char jb[24];
        int k = 0;
        const char *p = "JEDEC=";
        while (*p) jb[k++] = *p++;
        for (int b = 0; b < 3; ++b) {
            jb[k++] = hexd[(s_fb.jedec >> ((2 - b) * 8 + 4)) & 0xF];
            jb[k++] = hexd[(s_fb.jedec >> ((2 - b) * 8)) & 0xF];
        }
        jb[k] = '\0';
        txt(tile, TXT_X, STAT_Y + 56, jb, C_DIM);
    }

    /* 提示(放在状态文字下方) */
    if (s_fb.phase == PH_IDLE) {
        txt(tile, TXT_X, HINT_Y,
            "Put gb2312_*.bin on TF: 0:/ or 0:/download/", C_DIM);
    }
}

/* ===================== 生命周期 ===================== */

static void fb_init(app_t *self)
{
    /* ⚠️ user_data 必须指向 dirty_rect(dirty_rect 是本结构第一个成员)。
     * app_manager 会无条件执行 *(ui_rect_t *)user_data,
     * 指向别处会读到垃圾甚至崩溃。 */
    self->user_data = &s_fb.dirty_rect;
}

static void fb_enter(app_t *self)
{
    s_fb.phase = PH_IDLE;
    s_fb.skip_carried_touch = true;
    s_fb.grand = 0;
    s_fb.grand_total = 0;
    for (unsigned i = 0; i < FONT_N; ++i) s_fb.grand_total += k_fonts[i].size;

    if (!s_fb.fs_ready) {
        if (f_mount(&s_fs, "0:", 1) == FR_OK) s_fb.fs_ready = true;
    }

    /* 读一下 W25 状态寄存器:如果块保护位(BP2..BP0)或 SR2.CMP 不为 0,
     * 芯片会拒绝擦除/写入 —— 这正是"擦除后数据不变、校验必失败"的典型原因。 */
    s_fb.sr1_ok      = false;
    s_fb.sr1 = s_fb.sr2 = s_fb.sr3 = 0;
    s_fb.protected_st = false;
    {
        s_fb.jedec = W25_ReadJEDECID();
        if (s_fb.jedec != 0 && s_fb.jedec != 0xFFFFFFu) {
            s_fb.sr1 = W25_ReadStatus1();
            s_fb.sr2 = W25_ReadStatus2();
            s_fb.sr3 = W25_ReadStatus3();
            s_fb.sr1_ok = true;
            s_fb.protected_st = W25_IsProtected();
        }
    }

    /* 检查三个文件是否都在 */
    char msg[48];
    int k = 0;
    for (unsigned i = 0; i < FONT_N; ++i) {
        FIL f;
        s_fb.files_ok[i] = open_font(&f, k_fonts[i].name);
        if (s_fb.files_ok[i]) {
            f_close(&f);
            msg[k++] = '1';
        } else {
            msg[k++] = '0';
        }
    }
    msg[k] = '\0';
    (void)msg;

    fb_reset_state();
    if (!s_fb.files_ok[0] || !s_fb.files_ok[1] || !s_fb.files_ok[2]) {
        fb_log("MISSING bin file(s) on TF card");
    }
    self->user_data = &s_fb.dirty_rect;
    s_fb.dirty_rect = ui_rect_make(0, 32, SCR_W, SCR_H - 32);
}

static void fb_exit(app_t *self)
{
    (void)self;
}

static void fb_update(app_t *self, uint32_t now)
{
    (void)self; (void)now;

    bool busy = (s_fb.phase == PH_ERASE || s_fb.phase == PH_BLANK ||
                 s_fb.phase == PH_WRITE || s_fb.phase == PH_VERIFY);

    if (busy) {
        fb_step();
        /* ⚡ 烧录期间降低重绘频率。
         * 每次重绘都会去 W25Q64 读中文字模(状态栏、文件名),而 W25 和 LCD
         * 共用 SPI1。虽然 W25_CS_Select() 里有 LCD_WaitDMA() 串行化,
         * 但擦/写期间频繁插入读操作既拖慢烧录,也增加总线交互风险。
         * 每 8 步刷一次屏,进度仍然可见。 */
        s_fb.redraw_div++;
        if (s_fb.redraw_div >= 8u) {
            s_fb.redraw_div = 0;
            g_app_mgr.needs_full_redraw = true;
        }
        s_fb.dirty_rect = ui_rect_make(0, 32, SCR_W, SCR_H - 32);
        return;
    }

    s_fb.redraw_div = 7;   /* 阶段切换后立刻刷一次 */

    if (s_fb.need_redraw) {
        s_fb.need_redraw = false;
        s_fb.dirty_rect  = ui_rect_make(0, 32, SCR_W, SCR_H - 32);
        /* ⚡ 关键:主循环只在 needs_full_redraw 或触控脏矩形时才重绘。
         * 只设 dirty_rect 是没用的 —— 主循环根本不读它,
         * 屏幕会定格在 "Erasing..." 看起来像卡死(其实是没重绘)。 */
        g_app_mgr.needs_full_redraw = true;
    } else {
        s_fb.dirty_rect = ui_rect_make(0, 0, 0, 0);
    }
}

app_t *app_fontburn_get_instance(void)
{
    s_inst.id            = APP_ID_CUSTOM_0 + 9;   /* +1..+8 已被其它 app 占用 */
    s_inst.name          = "FONT BURN";
    s_inst.user_data     = &s_fb.dirty_rect;
    s_inst.init          = fb_init;
    s_inst.enter         = fb_enter;
    s_inst.exit          = fb_exit;
    s_inst.update        = fb_update;
    s_inst.process_touch = fb_process_touch;
    s_inst.draw          = fb_draw;
    return &s_inst;
}
