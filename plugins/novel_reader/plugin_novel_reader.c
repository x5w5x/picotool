/* =============================================================
 * Novel Reader (TF 卡 .txt 小说阅读器)
 *
 * 两种状态:
 *   SCREEN_BROWSER  列 TF 卡 0:/ 下所有 .txt 文件,tap 选择
 *   SCREEN_READER   打开文件,渲染分页,触摸翻页/拖动进度条
 *
 * 编码:
 *   - 自动检测 BOM (UTF-8 BOM 跳过前 3 字节)
 *   - 文件头 64 字节做 UTF-8 合法性扫描,无效 → 假设 GBK
 *   - UTF-8 文件读出来后必须转成 GBK 才能喂给 ui_draw_string
 *     (W25Q64 字库是 GB2312,只看 GBK 双字节)
 *
 * 字体大小 (3 档,顶栏右侧按钮循环):
 *   0 小  = FONT_16 (16 px CJK, 8 px ASCII, 行高 20)
 *   1 中  = FONT_20 (20 / 10, 24)
 *   2 大  = FONT_24 (24 / 12, 28)
 *
 * 触摸:
 *   浏览器  - tap 列表行 = 打开文件
 *   阅读器  - tap 左半 = 上一页
 *            - tap 右半 = 下一页
 *            - drag 底部进度条 = 跳转到比例位置
 *            - tap 顶栏"返回"= 退出 (保存书签)
 *            - tap 顶栏"Aa"= 循环字体大小
 *
 * 书签:
 *   打开时读 <filepath>.bmk (8 字节 hex byte_pos)
 *   退出时写当前 byte_pos 到 <filepath>.bmk
 *   没有书签从头开始
 *
 * 架构契约 (与 led_controller / wire_quality 一致):
 *   - 二进制 ≤ 32KB,落 0x20038000-0x20040000
 *   - 入口 app_entry() 必须在 offset 0
 *   - 所有 UI 走 host_api 函数指针
 *   - 所有文件访问走 host_api fat_*
 *
 * 源文件编码: UTF-8 → GBK (与 wire_quality 相同处理)
 * ============================================================= */

#include "plugin_novel_reader.h"
#include <stddef.h>

/* ================= 1. 常量 ================= */

#define SCREEN_W       480
#define SCREEN_H       320

/* 屏幕状态 */
#define SCR_BROWSER    0
#define SCR_READER     1

/* 编码 */
#define ENC_GBK        0
#define ENC_UTF8       1
#define ENC_UTF8_BOM   2

/* 字体大小 */
#define FONT_SMALL     0
#define FONT_MEDIUM    1
#define FONT_LARGE     2

/* 布局 */
#define HEADER_H       32       /* 顶栏 */
#define BODY_Y         (HEADER_H + 4)
#define BODY_H         240      /* 文本区 */
#define PROG_Y         280      /* 进度条 Y */
#define PROG_H         24       /* 进度条区高 */
#define MARGIN_X       10       /* 左右边距 */

/* 文件浏览 */
#define FILE_LIST_Y    40
#define FILE_ROW_H     28
#define FILE_LIST_MAX  9        /* 一屏 9 行 */

/* 缓冲区 */
#define PAGE_BUF_SIZE  2400    /* 1 页 GBK 字节数 (大字体也够 6+ 行) */
#define MAX_FILES      20      /* 文件名缓冲 */
#define NAME_LEN       32
#define PATH_LEN       64
#define BMK_PATH_LEN   80
#define MAX_HISTORY    64      /* 历史页码栈深度 */

/* ================= 2. plugin 状态结构 ================= */

typedef struct {
    /* UI 框架 */
    ui_rect_t              dirty_rect;   /* ⚠️ 必须保持在第 1 位 */
    ui_btn_t               btn_back;     /* 顶栏左上"返回" */
    ui_btn_t               btn_font;     /* 顶栏右上"Aa" */

    /* 屏幕状态 */
    uint8_t                screen;       /* SCR_BROWSER / SCR_READER */
    bool                   unsupported_utf8;  /* ⚡ 文件是 UTF-8,字库只认 GBK → 提示屏 */

    /* 字体大小 */
    uint8_t                font_size;
    int16_t                char_w_ascii;
    int16_t                char_w_cjk;
    int16_t                line_h;

    /* 文件浏览器状态 */
    char                   file_names[MAX_FILES][NAME_LEN];
    int                    file_count;
    int                    file_top;     /* 列表滚动到的首行 index */
    int                    file_selected;/* 高亮的行 index (-1 = 无) */

    /* 阅读状态 */
    int                    fd;           /* host_api fat fd, -1 = 没开 */
    char                   filepath[PATH_LEN];
    char                   bookmark_path[BMK_PATH_LEN];
    uint32_t               file_size;
    uint32_t               byte_pos;     /* 当前页首字节 */
    uint32_t               page_end;     /* 当前页实际结束字节 (下一行起点) - 用于精准翻页 */
    uint8_t                encoding;
    uint8_t                skip_bytes;   /* GBK=0, UTF-8-BOM=3 */

    /* 当前页缓冲 (UTF-8 已转 GBK,内含 \0 行分隔) */
    char                   page_buf[PAGE_BUF_SIZE];
    int                    page_bytes;   /* 实际填充字节数 */
    int                    last_br;      /* ⚡ 上次 fat_read 实际读到的字节数(诊断用) */

    /* 翻页历史 (栈) — history_top 是当前页索引,top-1 是上一页首字节 */
    uint32_t               history[MAX_HISTORY];
    int                    history_top;
    int                    history_count;

    /* 触摸拖动进度条 */
    bool                   dragging;

    /* ⚡ 触摸残留防护:从浏览器点文件进入阅读器时,手指还按在屏幕上。
     * 紧接着的 touch UP 会在阅读器被当作翻页手势,让 byte_pos
     * 直接跳到 EOF → page_bytes=0 → 白屏。
     * open_file() 把它置 true,nr_process_touch 看到 true 就
     * 把整段 (down + up) 都吞掉,直到看到下一次"新的 down"(再次按下),
     * 才认为用户开始正式操作阅读器 */
    bool                   skip_carried_touch;

    const pico_host_api_t *api;
} novel_data_t;

static novel_data_t s_nr;

/* ================= 3. 工具: int → str ================= */

static int int_to_str(int32_t v, char *buf, int sz) {
    if (sz < 2) return 0;
    char tmp[12]; int n = 0;
    bool neg = (v < 0);
    if (neg) v = -v;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 12) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    if (neg && i < sz - 1) buf[i++] = '-';
    while (n > 0 && i < sz - 1) buf[i++] = tmp[--n];
    buf[i] = '\0';
    return i;
}

/* ================= 4. 字体度量 ================= */

static void apply_font_metrics(void) {
    switch (s_nr.font_size) {
    case FONT_SMALL:
        s_nr.char_w_ascii = 8;
        s_nr.char_w_cjk   = 16;
        s_nr.line_h       = 20;
        break;
    case FONT_LARGE:
        s_nr.char_w_ascii = 12;
        s_nr.char_w_cjk   = 24;
        s_nr.line_h       = 28;
        break;
    case FONT_MEDIUM:
    default:
        s_nr.char_w_ascii = 10;
        s_nr.char_w_cjk   = 20;
        s_nr.line_h       = 24;
        break;
    }
}

/* ================= 5. 编码检测 =================
 * 读前 64 字节,根据 BOM 和 UTF-8 合法性判别
 * 返 enc 编码
 * 失败 / 文件太小 → 假设 GBK (W25Q64 字库是 GB2312) */
static uint8_t detect_encoding(int fd) {
    uint8_t head[64];
    int br = s_nr.api->fat_read(fd, head, sizeof(head));
    if (br <= 0) return ENC_GBK;
    /* seek 回去 (open_file 紧接着还要读书签 + 渲染) */
    s_nr.api->fat_seek(fd, 0);

    /* BOM 检测:UTF-8 BOM = EF BB BF */
    if (br >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        return ENC_UTF8_BOM;
    }

    /* UTF-8 合法性扫描:只数 3-byte CJK 序列 (E0..EF + 80..BF + 80..BF)
     * 这些几乎不可能出现在 GBK 里 (GBK 双字节首字节 0x81..0xFE,次字节 0x40..0xFE)
     * 看到 ≥1 个合法 3-byte CJK → UTF-8 */
    int cjk = 0, bad = 0;
    for (int i = 0; i < br; i++) {
        uint8_t b = head[i];
        if (b < 0x80) continue;                                  /* ASCII */
        if ((b & 0xF0) == 0xE0 &&
            i + 2 < br &&
            (head[i+1] & 0xC0) == 0x80 &&
            (head[i+2] & 0xC0) == 0x80) {
            cjk++; i += 2;
        } else {
            bad++;
        }
    }
    /* 有合法 3-byte CJK 且无坏字节 → UTF-8;否则按 GBK 处理 (W25Q64 字库认) */
    if (cjk >= 1 && bad == 0) return ENC_UTF8;
    return ENC_GBK;
}

/* ================= 6. UTF-8 → GBK 转码 =================
 * src/src_len: UTF-8 字节
 * dst/dst_max: GBK 字节输出
 * 返写入 dst 的字节数
 *
 * 转换规则 (线性近似,只覆盖 CJK 统一汉字基本平面):
 *   Unicode U+4E00..U+9FA5 (20902 字) → GBK 0xB0A1..(粗略)
 *     offset = cp - 0x4E00
 *     hi     = 0xB0 + offset / 94
 *     lo     = 0xA1 + offset % 94
 *   范围外的字 (符号 / 扩展 / 罕用字) 写 '?'
 *
 * 多数常见中文小说 95%+ 字符都落在 U+4E00..U+9FA5,够用 */
static int utf8_to_gbk(const uint8_t *src, int src_len, uint8_t *dst, int dst_max) {
    int si = 0, di = 0;
    while (si < src_len && di + 2 < dst_max) {
        uint8_t b1 = src[si];
        if (b1 < 0x80) {
            dst[di++] = b1;
            si++;
        } else if ((b1 & 0xE0) == 0xC0 && si + 1 < src_len) {
            /* 2-byte UTF-8 → ASCII 范围,直接当 1 字节 */
            dst[di++] = '?';
            si += 2;
        } else if ((b1 & 0xF0) == 0xE0 && si + 2 < src_len) {
            uint32_t cp = ((uint32_t)(b1 & 0x0F) << 12)
                        | ((uint32_t)(src[si+1] & 0x3F) << 6)
                        |  (uint32_t)(src[si+2] & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FA5) {
                int offset = (int)(cp - 0x4E00);
                int hi = 0xB0 + (offset / 94);
                int lo = 0xA1 + (offset % 94);
                if (hi >= 0xB0 && hi <= 0xF7 && lo >= 0xA1 && lo <= 0xFE) {
                    dst[di++] = (uint8_t)hi;
                    dst[di++] = (uint8_t)lo;
                } else {
                    dst[di++] = '?';
                }
            } else {
                dst[di++] = '?';
            }
            si += 3;
        } else {
            dst[di++] = '?';
            si++;
        }
    }
    return di;
}

/* ================= 7. 书签读写 =================
 * 书签文件存 byte_pos 的十六进制 (8 hex chars) + \n
 * 用 fat_open (mode=1 写 / mode=0 读) */
static uint32_t load_bookmark(const char *path) {
    int fd = s_nr.api->fat_open(path, 0);  /* mode 0 = read */
    if (fd < 0) return 0;
    uint8_t buf[16];
    int br = s_nr.api->fat_read(fd, buf, sizeof(buf));
    s_nr.api->fat_close(fd);
    if (br <= 0) return 0;
    /* 解析 hex */
    uint32_t v = 0;
    bool any = false;
    for (int i = 0; i < br; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') { v = (v << 4) | (c - '0'); any = true; }
        else if (c >= 'a' && c <= 'f') { v = (v << 4) | (c - 'a' + 10); any = true; }
        else if (c >= 'A' && c <= 'F') { v = (v << 4) | (c - 'A' + 10); any = true; }
        else break;
    }
    return any ? v : 0;
}

static void save_bookmark(const char *path, uint32_t pos) {
    /* hex 8 位 + '\n' = 9 字节 */
    char buf[12];
    const char *hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[pos & 0xF];
        pos >>= 4;
    }
    buf[8] = '\n';
    buf[9] = '\0';
    int fd = s_nr.api->fat_open(path, 1);  /* mode 1 = write/create */
    if (fd < 0) return;
    s_nr.api->fat_write(fd, (const uint8_t *)buf, 9);
    s_nr.api->fat_close(fd);
}

/* ================= 8. 当前页渲染 =================
 * 流程:
 *   1. fat_seek 到 byte_pos
 *   2. 读 PAGE_BUF_SIZE 字节到 temp_buf
 *   3. 若是 UTF-8,转码到 page_buf (GBK)
 *   4. 在 page_buf 里按 char width 找换行点
 *   5. 在每个换行点写 \0 (page_buf 必须可写)
 *   6. 更新 byte_pos 到页末
 *
 * 返回:是否成功渲染 */
static bool render_current_page(void) {
    if (s_nr.fd < 0) return false;
    if (s_nr.api->fat_seek(s_nr.fd, (uint32_t)s_nr.byte_pos) < 0) return false;

    uint8_t tmp[PAGE_BUF_SIZE];
    int br = s_nr.api->fat_read(s_nr.fd, tmp, sizeof(tmp));
    s_nr.last_br = br;   /* ⚡ 诊断用:实际 fat_read 返了多少字节 */
    if (br <= 0) {
        s_nr.page_bytes = 0;
        return false;
    }

    /* UTF-8 → GBK 转码 (如果是 UTF-8) */
    int gbk_len;
    if (s_nr.encoding == ENC_UTF8 || s_nr.encoding == ENC_UTF8_BOM) {
        gbk_len = utf8_to_gbk(tmp, br, (uint8_t *)s_nr.page_buf, PAGE_BUF_SIZE - 1);
    } else {
        if (br > PAGE_BUF_SIZE - 1) br = PAGE_BUF_SIZE - 1;
        for (int i = 0; i < br; i++) s_nr.page_buf[i] = (char)tmp[i];
        gbk_len = br;
    }
    s_nr.page_buf[gbk_len] = '\0';
    s_nr.page_bytes = gbk_len;

    /* 按 char width 找换行点,在换行点插 \0
     * line_ends[i] = "第 i 行的最后一个字节的位置(将被置 \0)",
     *               不含首行起点(首行起点固定是 page_buf[0])
     * 扫到行尾时记录,扫完从后往前插 \0,避免影响后面的扫描 */
    int max_x = SCREEN_W - MARGIN_X * 2;
    int cur_x = 0;
    static int line_ends[64];   /* 最多 64 行,够 32KB 的 page_buf */
    int byte_i = 0;
    int lc = 0;                 /* line_ends[0..lc-1] 是已记录的行尾位置数 */

    while (byte_i < gbk_len && lc < 64) {
        uint8_t b = (uint8_t)s_nr.page_buf[byte_i];
        int ch_w, ch_bytes;
        if (b == '\n') {
            /* 显式换行 (LF) → 当前行结束在 byte_i(将置 \0),下一行从 byte_i+1 */
            line_ends[lc++] = byte_i;
            byte_i++;
            cur_x = 0;
            continue;
        }
        if (b == '\r') {
            /* CR (Windows 记事本 GBK 文件用 \r\n 行尾) → 视作换行 */
            line_ends[lc++] = byte_i;
            byte_i++;
            /* 如果下一个是 \n,跳过 */
            if (byte_i < gbk_len && (uint8_t)s_nr.page_buf[byte_i] == '\n') byte_i++;
            cur_x = 0;
            continue;
        }
        if (b < 0x80) {
            ch_w = s_nr.char_w_ascii;
            ch_bytes = 1;
        } else {
            ch_w = s_nr.char_w_cjk;
            ch_bytes = 2;
            if (byte_i + 1 >= gbk_len) {
                /* 半个汉字,丢掉 */
                break;
            }
        }
        if (cur_x + ch_w > max_x && cur_x > 0) {
            /* 行满 → 当前行结束在 byte_i(将置 \0),下一行从这里开始 */
            line_ends[lc++] = byte_i;
            cur_x = 0;
            /* 不动 byte_i,继续处理这个字符(它属于下一行) */
            continue;
        }
        cur_x += ch_w;
        byte_i += ch_bytes;
    }

    /* 从后往前在 line_ends 位置插 \0 (避免影响前面的扫描) */
    for (int i = lc - 1; i >= 0; i--) {
        int pos = line_ends[i];
        if (pos < PAGE_BUF_SIZE) s_nr.page_buf[pos] = '\0';
    }

    /* ⚡ 精准翻页:页面结束位置 = 最后一行末尾字节的下一个字节
     *  (也就是 line_ends[lc-1] 的位置,因为我们刚在它前面插了 \0,
     *   \0 之后的内容是会被丢弃的"半截下一行")
     * 如果 br < PAGE_BUF_SIZE (到文件末尾了),就到 gbk_len (再 + 1 给 page_buf 末尾 \0 让位) */
    if (br < (int)sizeof(tmp)) {
        /* 文件已读完 (FATFS 读不到 PAGE_BUF_SIZE 这么多) */
        s_nr.page_end = (uint32_t)(s_nr.byte_pos + gbk_len);
    } else if (lc > 0) {
        /* 文件未读完,用最后一行末位置 */
        s_nr.page_end = (uint32_t)(s_nr.byte_pos + line_ends[lc - 1]);
    } else {
        /* 一行都没有 (page_buf 全是空白) → 跳过整个 PAGE_BUF_SIZE */
        s_nr.page_end = (uint32_t)(s_nr.byte_pos + br);
    }

    /* 计算下一页 byte_pos = 当前 byte_pos + 已消耗字节数 */
    /* 但 GBK 转码后字节数 ≠ 文件原始字节数!
     * 简化:用 "读出来的字节数" 当作原始消耗
     * 如果 UTF-8: 1 个汉字文件占 3 字节,转 GBK 后 2 字节,3 字节原始 → 下一 byte_pos += 3
     * 直接对原文做一遍扫描更准,但代码会变长 */

    /* ⚡ byte_pos 不在这里改,翻页逻辑由 goto_next_page 用 page_end 推进 */

    return true;
}

/* ================= 9. 翻页 ================= */

static void goto_next_page(void) {
    if (s_nr.history_count >= MAX_HISTORY) {
        /* 栈满,丢最早的 */
        for (int i = 0; i < MAX_HISTORY - 1; i++) s_nr.history[i] = s_nr.history[i + 1];
        s_nr.history_count = MAX_HISTORY - 1;
        s_nr.history_top   = s_nr.history_count - 1;
    }
    /* push 当前页起点到 history (作为上一页锚点) */
    s_nr.history[s_nr.history_count] = s_nr.byte_pos;
    s_nr.history_count++;
    s_nr.history_top = s_nr.history_count - 1;
    /* ⚡ 精准翻页:跳到当前页末 (page_end),不是 +br */
    if (s_nr.page_end > s_nr.byte_pos && s_nr.page_end <= s_nr.file_size) {
        s_nr.byte_pos = s_nr.page_end;
    } else {
        /* fallback: page_end 没算对 (例如 lc==0) → 老逻辑 */
        s_nr.byte_pos += PAGE_BUF_SIZE;
    }
    if (s_nr.byte_pos > s_nr.file_size) s_nr.byte_pos = s_nr.file_size;
    render_current_page();
    s_nr.api->request_redraw();
}

static void goto_prev_page(void) {
    if (s_nr.history_top <= 0) {
        /* 已是首页 */
        return;
    }
    s_nr.history_top--;
    s_nr.history_count = s_nr.history_top + 1;
    s_nr.byte_pos = s_nr.history[s_nr.history_top];
    render_current_page();
    s_nr.api->request_redraw();
}

/* 跳到文件比例 (0..100),清空历史 */
static void goto_progress(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t new_pos = (uint32_t)((uint64_t)s_nr.file_size * pct / 100u);
    /* 夹到 skip_bytes 之后 (UTF-8-BOM 前 3 字节不能读) */
    if (new_pos < s_nr.skip_bytes) new_pos = s_nr.skip_bytes;
    s_nr.byte_pos = new_pos;
    s_nr.history_top = 0;
    s_nr.history_count = 1;
    s_nr.history[0] = s_nr.byte_pos;
    render_current_page();
    s_nr.api->request_redraw();
}

/* ================= 10. 打开文件 ================= */

static void open_file(const char *name) {
    /* 关旧文件 */
    if (s_nr.fd >= 0) {
        s_nr.api->fat_close(s_nr.fd);
        s_nr.fd = -1;
    }
    /* 拼路径 "0:/<name>" */
    int i = 0;
    s_nr.filepath[i++] = '0'; s_nr.filepath[i++] = ':'; s_nr.filepath[i++] = '/';
    int j = 0;
    while (name[j] && j < NAME_LEN - 1 && i < PATH_LEN - 1) {
        s_nr.filepath[i++] = name[j++];
    }
    s_nr.filepath[i] = '\0';

    /* 拼书签路径 "0:/<name>.bmk" */
    int k = 0;
    while (s_nr.filepath[k] && k < BMK_PATH_LEN - 1) {
        s_nr.bookmark_path[k] = s_nr.filepath[k];
        k++;
    }
    /* 替换扩展 .txt → .bmk */
    int dot = -1;
    for (int x = k - 1; x >= 0; x--) {
        if (s_nr.bookmark_path[x] == '.') { dot = x; break; }
    }
    if (dot < 0) dot = k;
    s_nr.bookmark_path[dot] = '\0';
    int b = dot;
    const char *suf = ".bmk";
    while (*suf && b < BMK_PATH_LEN - 1) s_nr.bookmark_path[b++] = *suf++;
    s_nr.bookmark_path[b] = '\0';

    /* 打开文件 (mode=0 = read) */
    s_nr.fd = s_nr.api->fat_open(s_nr.filepath, 0);
    if (s_nr.fd < 0) {
        s_nr.screen = SCR_BROWSER;
        return;
    }

    s_nr.file_size = (uint32_t)s_nr.api->fat_size(s_nr.fd);
    s_nr.encoding = detect_encoding(s_nr.fd);
    s_nr.skip_bytes = (s_nr.encoding == ENC_UTF8_BOM) ? 3 : 0;
    s_nr.unsupported_utf8 = (s_nr.encoding == ENC_UTF8 || s_nr.encoding == ENC_UTF8_BOM);

    /* 诊断:报编码 + 文件前 16 字节 (hex) */
    if (s_nr.api->post_log) {
        char msg[128]; int n = 0;
        const char *p = "[NR-DBG] open enc=";
        while (*p && n < (int)sizeof(msg) - 80) msg[n++] = *p++;
        char num[8];
        int_to_str((int32_t)s_nr.encoding, num, sizeof(num));
        int k = 0; while (num[k] && n < (int)sizeof(msg) - 60) msg[n++] = num[k++];
        const char *p2 = " size=";
        while (*p2 && n < (int)sizeof(msg) - 60) msg[n++] = *p2++;
        int_to_str((int32_t)s_nr.file_size, num, sizeof(num));
        k = 0; while (num[k] && n < (int)sizeof(msg) - 50) msg[n++] = num[k++];
        msg[n++] = ' ';
        msg[n++] = 'h';
        msg[n++] = 'e';
        msg[n++] = 'a';
        msg[n++] = 'd';
        msg[n++] = '=';
        /* 重新读前 16 字节 (detect_encoding 里已经 seek 回 0) */
        uint8_t hb[16];
        int brh = s_nr.api->fat_read(s_nr.fd, hb, 16);
        s_nr.api->fat_seek(s_nr.fd, 0);  /* 再 seek 回 0,后续渲染要从 0 读 */
        const char *hex = "0123456789ABCDEF";
        for (int i = 0; i < brh && n < (int)sizeof(msg) - 4; i++) {
            msg[n++] = hex[(hb[i] >> 4) & 0xF];
            msg[n++] = hex[hb[i] & 0xF];
            msg[n++] = ' ';
        }
        msg[n] = '\0';
        s_nr.api->post_log(msg);
    }

    /* ⚡ UTF-8 文件暂不支持 (W25Q64 字库是 GB2312,UTF-8→GBK 需要查表,32KB 放不下)
     * 显示提示屏,关 fd,让用户从顶栏 << 返回回浏览器 */
    if (s_nr.unsupported_utf8) {
        s_nr.api->fat_close(s_nr.fd);
        s_nr.fd = -1;
        s_nr.screen = SCR_READER;
        s_nr.skip_carried_touch = true;
        s_nr.api->request_redraw();
        return;
    }

    /* 尝试读书签 */
    uint32_t bm = load_bookmark(s_nr.bookmark_path);
    s_nr.byte_pos = (bm > 0 && bm < s_nr.file_size) ? bm : s_nr.skip_bytes;

    s_nr.history_top = 0;
    s_nr.history_count = 1;
    s_nr.history[0] = s_nr.byte_pos;

    s_nr.screen = SCR_READER;
    s_nr.skip_carried_touch = true;   /* ⚡ 吞掉进入阅读器的这次残留触摸 */
    render_current_page();
    s_nr.api->request_redraw();
}

/* ================= 11. 顶栏按钮回调 ================= */

static void on_back(ui_btn_t *b, void *ud) {
    (void)b;
    novel_data_t *d = (novel_data_t *)ud;
    if (!d) return;
    if (d->screen == SCR_BROWSER) {
        d->api->request_exit();
    } else {
        /* ⚡ UTF-8 提示屏下 fd 早已关闭,直接回浏览器 */
        if (!d->unsupported_utf8) {
            /* 退出阅读前保存书签 → 下次自动续读 */
            if (d->fd >= 0 && d->bookmark_path[0]) {
                save_bookmark(d->bookmark_path, d->byte_pos);
            }
            if (d->fd >= 0) {
                d->api->fat_close(d->fd);
                d->fd = -1;
            }
        }
        d->unsupported_utf8 = false;
        d->screen = SCR_BROWSER;
        if (d->api->request_redraw) d->api->request_redraw();
    }
}

static void on_font(ui_btn_t *b, void *ud) {
    (void)b;
    novel_data_t *d = (novel_data_t *)ud;
    if (!d) return;
    d->font_size = (d->font_size + 1) % 3;
    apply_font_metrics();
    /* 切字体后重新渲染当前页 (layout 不同) */
    if (d->screen == SCR_READER) {
        d->byte_pos = d->history[d->history_top];
        render_current_page();
    }
    if (d->api->request_redraw) d->api->request_redraw();
}

/* ================= 12. 生命周期 ================= */

static bool nr_init(const pico_host_api_t *api) {
    if (!api) return false;
    s_nr.dirty_rect = ui_rect_make(0, 0, 0, 0);
    s_nr.api         = api;
    s_nr.screen      = SCR_BROWSER;
    s_nr.font_size   = FONT_MEDIUM;
    apply_font_metrics();
    s_nr.fd          = -1;
    s_nr.file_size = 0;
    s_nr.byte_pos = 0;
    s_nr.history_top = 0;
    s_nr.history_count = 0;
    s_nr.file_count = 0;
    s_nr.file_top = 0;
    s_nr.file_selected = -1;
    s_nr.dragging = false;
    s_nr.last_br = -1;   /* ⚡ 诊断:尚未读过 */

    /* 顶栏按钮 */
    if (api->ui_btn_init) {
        api->ui_btn_init(&s_nr.btn_back,  0, 0, 80, HEADER_H, "<< 返回", on_back);
        api->ui_btn_init(&s_nr.btn_font,  SCREEN_W - 100, 0, 100, HEADER_H, "Aa 字体", on_font);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_nr.btn_back,  0x2945, 0x07FF, 0xFFFF);
            api->ui_btn_set_color(&s_nr.btn_font,  0x2945, 0x07FF, 0xFFFF);
        }
        s_nr.btn_back.user_data = &s_nr;
        s_nr.btn_font.user_data = &s_nr;
    }

    /* 列 0:/ 下 .txt 文件 */
    if (api->fat_list_dir) {
        s_nr.file_count = api->fat_list_dir("0:/",
                                            (char *)s_nr.file_names,
                                            sizeof(s_nr.file_names),
                                            MAX_FILES);
        if (s_nr.file_count < 0) s_nr.file_count = 0;
    }

    if (api->post_log) api->post_log("Novel Reader Ready");
    return true;
}

/* ⚡ 调试钩子已清理 */
static void nr_exit(void) {
    if (s_nr.api && s_nr.fd >= 0) {
        s_nr.api->fat_close(s_nr.fd);
        s_nr.fd = -1;
    }
    if (s_nr.api && s_nr.api->post_log) {
        s_nr.api->post_log("Novel Reader Stopped");
    }
    s_nr.api = NULL;
}

/* ================= 13. 触摸 ================= */

static void nr_process_touch(bool is_touch, int16_t x, int16_t y) {
    if (!s_nr.api || !s_nr.api->ui_btn_process_touch) return;
    uint32_t now = s_nr.api->get_time_ms ? s_nr.api->get_time_ms() : 0u;

    /* ⚡ 残留触摸防护:从浏览器点文件进入阅读器时,手指可能还按在屏幕上。
     * 关键时序:open_file() 置 skip_carried_touch=true。
     *  - 期间所有事件都丢(只同步按钮状态机),让 nr_process_touch 的
     *    翻页逻辑在残留 UP 期间绝不触发 goto_next/prev_page
     *  - 看到第一次 is_touch=false → 残留触摸结束,可以接收了,
     *    但这一帧的 UP 本身还是要吞掉(否则它就是触发翻页的凶手)
     *
     * 注意:不能再用 "is_touch=true 就清标志" 的逻辑 ——
     * 用户进阅读器前手指按着,进来后 is_touch=true 还会持续来几帧
     * (finger still down),那样标志会被立刻清掉,残留 UP 就漏过去了。
     * 必须是 UP 才清。 */
    if (s_nr.skip_carried_touch) {
        if (!is_touch) {
            s_nr.skip_carried_touch = false;   /* UP → 残留序列结束 */
        }
        /* 同步按钮状态机(它们的内部 is_touch_down 也要等这个 UP),
         * 但不进入屏逻辑分支 */
        s_nr.api->ui_btn_process_touch(&s_nr.btn_back, is_touch, x, y, now);
        s_nr.api->ui_btn_process_touch(&s_nr.btn_font, is_touch, x, y, now);
        return;
    }

    s_nr.api->ui_btn_process_touch(&s_nr.btn_back, is_touch, x, y, now);
    s_nr.api->ui_btn_process_touch(&s_nr.btn_font, is_touch, x, y, now);

    if (s_nr.screen == SCR_BROWSER) {
        if (!is_touch) {
            /* tap 释放 → 找 tap 到哪一行 */
            if (x >= 0 && x < SCREEN_W && y >= FILE_LIST_Y && y < FILE_LIST_Y + FILE_LIST_MAX * FILE_ROW_H) {
                int row = (y - FILE_LIST_Y) / FILE_ROW_H;
                int idx = s_nr.file_top + row;
                if (idx < s_nr.file_count) {
                    open_file(s_nr.file_names[idx]);
                }
            }
        }
    } else { /* SCR_READER */
        /* ⚡ 进度条:纯点击跳转 (按下记下 x,抬起时跳一次,不要连续跳) */
        if (is_touch && y >= PROG_Y && y < PROG_Y + PROG_H) {
            s_nr.dragging = true;
            return;
        }
        if (!is_touch) {
            if (s_nr.dragging && y >= PROG_Y && y < PROG_Y + PROG_H) {
                /* 进度条抬起 → 跳一次 */
                s_nr.dragging = false;
                int pct = (x * 100) / SCREEN_W;
                goto_progress(pct);
                return;
            }
            s_nr.dragging = false;
            /* ⚡ 翻页:HEADER_H (顶栏) 之下、PROG_Y (进度条) 之上,整片都是翻页区 */
            if (y >= HEADER_H && y < PROG_Y) {
                if (x < SCREEN_W / 2) goto_prev_page();
                else                  goto_next_page();
            }
        }
    }
}

/* ================= 14. 渲染 ================= */

static void draw_header(ui_tile_t *tile, const char *title) {
    if (!s_nr.api->ui_draw_fill_rect) return;
    ui_rect_t bar = ui_rect_make(0, 0, SCREEN_W, HEADER_H);
    s_nr.api->ui_draw_fill_rect(tile, &bar, 0x1A7B);
    if (s_nr.api->ui_draw_string && title) {
        s_nr.api->ui_draw_string(tile, 90, 8, title,
                                 FONT_20, 0xFFFF, 0x1A7B);
    }
}

static void draw_browser(ui_tile_t *tile) {
    if (!s_nr.api) return;
    /* 底色 */
    if (s_nr.api->ui_draw_fill_rect) {
        ui_rect_t full = ui_rect_make(0, 0, SCREEN_W, SCREEN_H);
        s_nr.api->ui_draw_fill_rect(tile, &full, 0xFFFF);
    }
    draw_header(tile, "选择小说 (TF 卡根目录)");

    /* 文件列表 */
    if (s_nr.api->ui_draw_string) {
        if (s_nr.file_count == 0) {
            s_nr.api->ui_draw_string(tile, MARGIN_X, FILE_LIST_Y + 20,
                                     "TF 卡根目录没有 .txt 文件",
                                     FONT_20, 0xF800, 0xFFFF);
        } else {
            for (int i = 0; i < FILE_LIST_MAX; i++) {
                int idx = s_nr.file_top + i;
                if (idx >= s_nr.file_count) break;
                int y = FILE_LIST_Y + i * FILE_ROW_H;
                uint16_t bg = (idx == s_nr.file_selected) ? 0x07FF : 0xEF7D;
                uint16_t fg = 0x0000;
                if (s_nr.api->ui_draw_fill_rect) {
                    ui_rect_t row = ui_rect_make(MARGIN_X, y, SCREEN_W - MARGIN_X * 2, FILE_ROW_H - 2);
                    s_nr.api->ui_draw_fill_rect(tile, &row, bg);
                }
                s_nr.api->ui_draw_string(tile, MARGIN_X + 4, y + 4,
                                         s_nr.file_names[idx],
                                         FONT_20, fg, bg);
            }
        }
    }

    /* 滚动提示 (右上角) */
    if (s_nr.api->ui_draw_string && s_nr.file_count > FILE_LIST_MAX) {
        char buf[8];
        int_to_str(s_nr.file_top + 1, buf, sizeof(buf));
        int_to_str(s_nr.file_count, buf + 4, sizeof(buf) - 4);  /* 不严谨但够 */
        s_nr.api->ui_draw_string(tile, SCREEN_W - 130, FILE_LIST_Y + FILE_LIST_MAX * FILE_ROW_H + 6,
                                 buf, FONT_16, 0x4208, 0xFFFF);
    }

    /* 按钮 */
    if (s_nr.api->ui_btn_draw) {
        s_nr.api->ui_btn_draw(tile, &s_nr.btn_back);
        s_nr.api->ui_btn_draw(tile, &s_nr.btn_font);
    }
}

static void draw_reader(ui_tile_t *tile) {
    if (!s_nr.api) return;
    /* 底色 */
    if (s_nr.api->ui_draw_fill_rect) {
        ui_rect_t full = ui_rect_make(0, 0, SCREEN_W, SCREEN_H);
        s_nr.api->ui_draw_fill_rect(tile, &full, 0xFFFF);
    }

    /* 顶栏 + 文件名 */
    {
        char title[64];
        int i = 4;  /* 跳过 "0:/" */
        int j = 0;
        while (s_nr.filepath[i] && j < (int)sizeof(title) - 1) {
            title[j++] = s_nr.filepath[i++];
        }
        title[j] = '\0';
        draw_header(tile, title);
    }

    /* ⚡ UTF-8 提示屏 (W25Q64 字库只认 GBK,本版本不支持 UTF-8) */
    if (s_nr.unsupported_utf8) {
        if (s_nr.api->ui_draw_string) {
            int cx = SCREEN_W / 2;
            s_nr.api->ui_draw_string(tile, MARGIN_X, BODY_Y + 30,
                                     "本文件是 UTF-8 编码",
                                     FONT_24, 0xF800, 0xFFFF);
            s_nr.api->ui_draw_string(tile, MARGIN_X, BODY_Y + 70,
                                     "当前字库仅支持 GBK / GB2312",
                                     FONT_20, 0x0000, 0xFFFF);
            s_nr.api->ui_draw_string(tile, MARGIN_X, BODY_Y + 110,
                                     "请用记事本打开此文件",
                                     FONT_20, 0x0000, 0xFFFF);
            s_nr.api->ui_draw_string(tile, MARGIN_X, BODY_Y + 140,
                                     "另存为 ANSI / GBK 后重试",
                                     FONT_20, 0x0000, 0xFFFF);
            s_nr.api->ui_draw_string(tile, MARGIN_X, BODY_Y + 190,
                                     "顶部 << 返回 回到文件列表",
                                     FONT_16, 0x4208, 0xFFFF);
        }
        if (s_nr.api->ui_btn_draw) {
            s_nr.api->ui_btn_draw(tile, &s_nr.btn_back);
            s_nr.api->ui_btn_draw(tile, &s_nr.btn_font);
        }
        return;
    }

    /* 文本行渲染 — page_buf 已经被各 \0 切好 */
    if (s_nr.api->ui_draw_string) {
        FontSize_t fs;
        switch (s_nr.font_size) {
        case FONT_SMALL:  fs = FONT_16; break;
        case FONT_LARGE:  fs = FONT_24; break;
        default:          fs = FONT_20; break;
        }
        int y = BODY_Y;
        const char *p = s_nr.page_buf;
        while (y < BODY_Y + BODY_H && *p) {
            const char *eol = p;
            while (*eol) eol++;
            s_nr.api->ui_draw_string(tile, MARGIN_X, y, p, fs, 0x0000, 0xFFFF);
            y += s_nr.line_h;
            p = eol + 1;
            if (y >= BODY_Y + BODY_H) break;
        }
    }

    /* 进度条 */
    if (s_nr.api->ui_draw_fill_rect) {
        ui_rect_t track = ui_rect_make(MARGIN_X, PROG_Y + 10, SCREEN_W - MARGIN_X * 2, 4);
        s_nr.api->ui_draw_fill_rect(tile, &track, 0xC618);
        /* 计算 thumb */
        if (s_nr.file_size > 0) {
            int tw = (SCREEN_W - MARGIN_X * 2) * (int)s_nr.byte_pos / (int)s_nr.file_size;
            if (tw < 8) tw = 8;
            if (tw > SCREEN_W - MARGIN_X * 2) tw = SCREEN_W - MARGIN_X * 2;
            ui_rect_t thumb = ui_rect_make(MARGIN_X, PROG_Y + 8, tw, 8);
            s_nr.api->ui_draw_fill_rect(tile, &thumb, 0x05E0);
        }
        /* 进度文字 */
        if (s_nr.api->ui_draw_string && s_nr.file_size > 0) {
            char pct[8];
            int p = (int)((uint64_t)s_nr.byte_pos * 100u / s_nr.file_size);
            int_to_str(p, pct, sizeof(pct));
            int len = 0; while (pct[len]) len++;
            if (len < (int)sizeof(pct) - 2) { pct[len++] = '%'; pct[len] = '\0'; }
            s_nr.api->ui_draw_string(tile, SCREEN_W / 2 - 20, PROG_Y + 16,
                                     pct, FONT_16, 0x4208, 0xFFFF);
        }
    }

    if (s_nr.api->ui_btn_draw) {
        s_nr.api->ui_btn_draw(tile, &s_nr.btn_back);
        s_nr.api->ui_btn_draw(tile, &s_nr.btn_font);
    }
}

static void nr_draw(ui_tile_t *tile) {
    if (!tile) return;
    if (s_nr.screen == SCR_BROWSER) draw_browser(tile);
    else                              draw_reader(tile);
}

/* ================= 15. 描述符 ================= */

static const app_plugin_t s_novel_plugin = {
    .magic         = APP_MAGIC,
    .version       = 0x00010000u,
    .name          = "Novel Reader",
    .init          = nr_init,
    .update_core1  = NULL,
    .process_touch = nr_process_touch,
    .draw          = nr_draw,
    .exit          = nr_exit,
};

const app_plugin_t *novel_reader_get_descriptor(void) {
    return &s_novel_plugin;
}