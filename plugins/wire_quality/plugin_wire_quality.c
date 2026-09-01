/* =============================================================
 * 杜邦线质量测量仪 (Wire Quality Tester)
 *
 * 三种测试模式 (右上 Tab 切换):
 *   MODE_LEVEL  电平:  读 TX 引脚实时电平 (HIGH/LOW)
 *   MODE_CONT   通断:  TX 推 HIGH/LOW 各一次,RX 回读,详细分类
 *                   PASS / OPEN / SHORT-GND / SHORT-VCC
 *   MODE_LOOP   丢包:  TX 循环发 0x55 (交替位) pattern,RX 回读,
 *                   统计 bits_sent / bit_errors / BER%
 *
 * 架构契约 (与 led_controller 完全一致):
 *   - 二进制 ≤ 32KB,落 APP_RAM_BASE..APP_RAM_BASE+32K (0x20038000..0x20040000)
 *   - 入口 app_entry() 必须在二进制 offset 0 (见 plugin.ld .entry)
 *   - 所有 UI 调用走 host_api 函数指针 (Core 0 上下文 = draw())
 *   - update_core1 跑 Core 1,严禁碰 UI / LCD
 *   - 引脚驱动 (TX 推 HIGH/LOW) 走 host_api->gpio_set_dir / gpio_put
 *     这两个槽位于 host_api_t 末尾扩展段,不破坏 led_controller 的偏移
 *
 * 源文件编码:
 *   源 .c 文件以 UTF-8 保存,编译选项 -finput-charset=UTF-8 -fexec-charset=GBK
 *   将中文常量字符串从 UTF-8 源字符转换为 GBK 字节塞进 .rodata。
 *   ui_draw_string 内部遇到 ≥0x80 字节会按 GB2312 双字节解释并查 W25Q64 字库。
 *
 * 注意:
 *   - GP23/GP24 严禁选作 TX/RX:Pico 板载 PSK flash 占用,plugin 推挽会破坏 flash
 *   - 不使用 printf/snprintf,不调任何 libc / libgcc,所以无需内联 __aeabi_uidiv
 * ============================================================= */

#include "plugin_wire_quality.h"
#include <stddef.h>

/* ================= 1. 屏幕分辨率与测试模式常量 ================= */

#define SCREEN_W  480
#define SCREEN_H  320

/* 三种测试模式 */
#define MODE_LEVEL   0  // 单引脚电平
#define MODE_CONT    1  // 通断测试
#define MODE_LOOP    2  // 丢包测试

/* 通断测试判决结果 */
#define CONT_PASS        0
#define CONT_OPEN        1
#define CONT_SHORT_GND   2
#define CONT_SHORT_VCC   3
#define CONT_SAME_PIN    4  // TX == RX,无可信结果
#define CONT_AMBIGUOUS   5  // 4-bit 真值表无匹配,接触抖动/干扰

/* 可选引脚表 (排除 GP23 / GP24 = 板载 PSK flash)
 * 按物理顺序:GP0..GP22 + GP25..GP28,共 27 个 GPIO
 * pin_gp_table[idx] 对应 GPIO 号,idx 用于循环选择 */
static const uint8_t pin_gp_table[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 25, 26, 27, 28
};
#define PIN_TABLE_LEN  (sizeof(pin_gp_table) / sizeof(pin_gp_table[0]))

/* plugin 私有状态结构 (落 plugin pool BSS) */
typedef struct {
    ui_rect_t              dirty_rect;   // ⚠️ 必须保持在第 1 位
    ui_btn_t               btn_stop;
    ui_btn_t               btn_tab_level;
    ui_btn_t               btn_tab_cont;
    ui_btn_t               btn_tab_loop;
    ui_btn_t               btn_tx_pin;
    ui_btn_t               btn_rx_pin;
    ui_btn_t               btn_loop_start;
    ui_btn_t               btn_loop_stop;   // ⚡ 仅停,不动计数(用户能看到最终 BER%)
    ui_btn_t               btn_loop_clear;  // ⚡ 仅清零(loop_running=false 时才该按)

    uint8_t                mode;         // 当前模式
    uint8_t                tx_idx;       // TX 在 pin_gp_table 的下标
    uint8_t                rx_idx;       // RX 在 pin_gp_table 的下标
    volatile uint8_t       tx_pin;       // 当前 TX 的 GPIO 号 (Core 0/1 共用)
    volatile uint8_t       rx_pin;       // 当前 RX 的 GPIO 号
    volatile bool          loop_running; // MODE_LOOP 是否正在跑
    volatile uint32_t      bits_sent;
    volatile uint32_t      bit_errors;

    volatile uint8_t       level;        // MODE_LEVEL: 0/1
    volatile uint8_t       cont_result;  // MODE_CONT: 上述 4 类之一

    /* ⚡ 诊断字段:init 时校验 host_api 是否带新 GPIO 槽
     * 老 host 固件(在 wire_quality 出现之前烧的).uf2 会把
     * gpio_set_dir / gpio_put / gpio_set_pulls 留成 NULL,
     * plugin 一调就段错误 → Core 1 worker 卡死 → Sent 不增。 */
    volatile bool          api_ok;       // 3 个 GPIO 函数指针是否齐
    volatile uint8_t       missing_idx;  // 0=齐, 1=set_dir, 2=put, 3=set_pulls

    /* 动态 label 缓冲区 (BSS,生命周期等同 plugin 加载期)
     * ⚠️ 必须常驻内存 — draw() 写完后 ui_btn_draw 会再次读取指针,
     * 用栈上 buffer 会在 draw 返回后留下悬空指针 */
    char                   tx_lbl[12];
    char                   rx_lbl[12];

    const pico_host_api_t *api;
} wire_quality_data_t;

static wire_quality_data_t s_wq;

/* ================= 2. 交互回调 ================= */

static void on_stop_click(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data && data->api && data->api->request_exit) {
        data->api->request_exit();
    }
}

static void on_tab_level(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->mode = MODE_LEVEL;
        if (data->api && data->api->post_log) {
            data->api->post_log("[wire_quality] tab -> MODE_LEVEL");
        }
    }
}

static void on_tab_cont(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->mode = MODE_CONT;
        if (data->api && data->api->post_log) {
            data->api->post_log("[wire_quality] tab -> MODE_CONT");
        }
    }
}

static void on_tab_loop(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->mode = MODE_LOOP;
        if (data->api && data->api->post_log) {
            data->api->post_log("[wire_quality] tab -> MODE_LOOP");
        }
    }
}

/* tap TX pin 按钮 → 循环到下一个 GPIO */
static void on_tx_pin_cycle(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (!data || !data->api) return;
    data->tx_idx = (data->tx_idx + 1) % PIN_TABLE_LEN;
    data->tx_pin = pin_gp_table[data->tx_idx];
    /* 切换后立刻把新 TX 配成输出 (mode CONT/LOOP 才需要,LEVEL 模式下忽略) */
    if (data->api->gpio_set_dir) {
        data->api->gpio_set_dir(data->tx_pin, true);
    }
    if (data->api->request_redraw) {
        data->api->request_redraw();
    }
}

/* tap RX pin 按钮 → 循环到下一个 GPIO */
static void on_rx_pin_cycle(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (!data || !data->api) return;
    data->rx_idx = (data->rx_idx + 1) % PIN_TABLE_LEN;
    data->rx_pin = pin_gp_table[data->rx_idx];
    /* 切换后立刻把新 RX 配成输入 */
    if (data->api->gpio_set_dir) {
        data->api->gpio_set_dir(data->rx_pin, false);
    }
    if (data->api->request_redraw) {
        data->api->request_redraw();
    }
}

static void on_loop_start(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->loop_running = true;
        data->bits_sent    = 0;
        data->bit_errors   = 0;
        /* 立刻请求重画 — 否则 draw() 仍读到旧 bits_sent=0 显示 "BER: -- (点开始)",
         * 看起来好像"点击不动"。 */
        if (data->api) {
            if (data->api->request_redraw) data->api->request_redraw();
            if (data->api->post_log) {
                data->api->post_log(data->api_ok
                    ? "[wire_quality] loop_start pressed (api_ok, expecting bits to count up)"
                    : "[wire_quality] loop_start pressed BUT api missing — bits WILL stay 0, reflash host .uf2!");
            }
        }
    }
}

/* ⚡ 仅停止 — 不动计数,让用户能看最终 BER% / Sent / Err */
static void on_loop_stop(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->loop_running = false;
        if (data->api) {
            if (data->api->request_redraw) data->api->request_redraw();
            if (data->api->post_log) {
                data->api->post_log("[wire_quality] loop_stop pressed (counters preserved)");
            }
        }
    }
}

/* ⚡ 仅清零 — 计数归 0,但 loop_running 不动
 * 用户应该先停再清(运行中清零意义不大,数字还在涨)
 * 但代码层面不强校验,出错也清 — 让用户能"硬切" */
static void on_loop_clear(ui_btn_t *btn, void *ud) {
    (void)btn;
    wire_quality_data_t *data = (wire_quality_data_t *)ud;
    if (data) {
        data->bits_sent    = 0;
        data->bit_errors   = 0;
        if (data->api) {
            if (data->api->request_redraw) data->api->request_redraw();
            if (data->api->post_log) {
                data->api->post_log("[wire_quality] loop_clear pressed (counters -> 0)");
            }
        }
    }
}

/* ================= 3. plugin freestanding:无 snprintf — 写一个 int→str helper ================= */

static inline int int_to_str(int32_t val, char *buf, int buf_size) {
    if (buf_size < 2) return 0;
    int n = 0; char tmp[12];
    bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 12) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int i = 0;
    if (neg && i < buf_size - 1) buf[i++] = '-';
    while (n > 0 && i < buf_size - 1) buf[i++] = tmp[--n];
    buf[i] = '\0';
    return i;
}

/* 把 "GP<n>" 写到 buf (plugin 不能 snprintf 任意字符串拼接) */
static int gp_to_str(uint8_t gp, char *buf, int buf_size) {
    if (buf_size < 5) return 0;
    buf[0] = 'G'; buf[1] = 'P';
    return int_to_str((int32_t)gp, buf + 2, buf_size - 2) + 2;
}

/* ================= 4. 生命周期与事件回调 ================= */

static bool wire_quality_init(const pico_host_api_t *api) {
    if (!api) return false;

    s_wq.dirty_rect = ui_rect_make(0, 0, 0, 0);
    s_wq.api        = api;

    s_wq.mode       = MODE_LEVEL;
    s_wq.tx_idx     = 5;   // 默认 GP5 (Pico Pin 10)
    s_wq.rx_idx     = 6;   // 默认 GP6 (Pico Pin 11)
    s_wq.tx_pin     = pin_gp_table[s_wq.tx_idx];
    s_wq.rx_pin     = pin_gp_table[s_wq.rx_idx];
    s_wq.loop_running = false;
    s_wq.bits_sent  = 0;
    s_wq.bit_errors = 0;
    s_wq.level      = 0;
    s_wq.cont_result = CONT_OPEN;
    s_wq.api_ok     = false;
    s_wq.missing_idx = 0;

    /* ⚡ 校验 host_api 是否带新 GPIO 槽 — 老 .uf2 没有这三个,
     * plugin 一调就段错误 → Core 1 worker 卡死 → Sent 一直 0。 */
    bool ok_dir  = (api->gpio_set_dir    != NULL);
    bool ok_put  = (api->gpio_put        != NULL);
    bool ok_pull = (api->gpio_set_pulls  != NULL);
    s_wq.api_ok = (ok_dir && ok_put && ok_pull);
    if (!ok_dir)       s_wq.missing_idx = 1;
    else if (!ok_put)  s_wq.missing_idx = 2;
    else if (!ok_pull) s_wq.missing_idx = 3;

    /* 启动日志:用 USB CDC 看,一眼能看出 plugin 跑没跑、API 缺不缺 */
    if (api->post_log) {
        api->post_log(s_wq.api_ok
            ? "[wire_quality] init OK (gpio_set_dir/put/set_pulls present)"
            : "[wire_quality] init FAIL: missing gpio API (reflash host .uf2)");
    }

    /* ============ 按钮初始化 ============ */
    if (api->ui_btn_init) {
        /* STOP 按钮 (右下角) */
        api->ui_btn_init(&s_wq.btn_stop, SCREEN_W - 110, SCREEN_H - 50,
                         100, 40, "STOP", on_stop_click);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_wq.btn_stop, 0x9000, 0xF800, 0xFFFF);
        }
        s_wq.btn_stop.user_data = &s_wq;

        /* Tab 按钮 (顶部三选一) */
        api->ui_btn_init(&s_wq.btn_tab_level, 10, 36, 150, 36, "电平", on_tab_level);
        api->ui_btn_init(&s_wq.btn_tab_cont,  165, 36, 150, 36, "通断", on_tab_cont);
        api->ui_btn_init(&s_wq.btn_tab_loop,  320, 36, 150, 36, "丢包", on_tab_loop);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_wq.btn_tab_level, 0x2945, 0x07FF, 0xFFFF);
            api->ui_btn_set_color(&s_wq.btn_tab_cont,  0x2945, 0x07FF, 0xFFFF);
            api->ui_btn_set_color(&s_wq.btn_tab_loop,  0x2945, 0x07FF, 0xFFFF);
        }
        s_wq.btn_tab_level.user_data = &s_wq;
        s_wq.btn_tab_cont.user_data  = &s_wq;
        s_wq.btn_tab_loop.user_data  = &s_wq;

        /* TX / RX 引脚选择按钮 — text 指针指到 state 常驻 buffer,
     * 后续 draw() 里我们改 buffer 内容即可更新 label (不再用栈 buffer) */
        s_wq.tx_lbl[0] = 'T'; s_wq.tx_lbl[1] = 'X'; s_wq.tx_lbl[2] = ':'; s_wq.tx_lbl[3] = ' ';
        gp_to_str(s_wq.tx_pin, s_wq.tx_lbl + 4, sizeof(s_wq.tx_lbl) - 4);
        s_wq.rx_lbl[0] = 'R'; s_wq.rx_lbl[1] = 'X'; s_wq.rx_lbl[2] = ':'; s_wq.rx_lbl[3] = ' ';
        gp_to_str(s_wq.rx_pin, s_wq.rx_lbl + 4, sizeof(s_wq.rx_lbl) - 4);

        api->ui_btn_init(&s_wq.btn_tx_pin, 10, 84, 225, 36, s_wq.tx_lbl, on_tx_pin_cycle);
        api->ui_btn_init(&s_wq.btn_rx_pin, 245, 84, 225, 36, s_wq.rx_lbl, on_rx_pin_cycle);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_wq.btn_tx_pin, 0x4208, 0x07E0, 0xFFFF);
            api->ui_btn_set_color(&s_wq.btn_rx_pin, 0x4208, 0x07E0, 0xFFFF);
        }
        s_wq.btn_tx_pin.user_data = &s_wq;
        s_wq.btn_rx_pin.user_data = &s_wq;

        /* 丢包模式下的 START / STOP / CLEAR 三个独立按钮
         * UX 流:开始 → (采集中) → 停止 → (看最终结果) → 清零 → 开始新一轮
         * 原来只有一个"停止+清零"复合按钮,用户没法看最终 BER% 再决定要不要清。
         *
         * 布局(480px 宽,各 145px):
         *   开始  (10, 130, 145, 36)  — 绿
         *   停止  (160, 130, 145, 36) — 红橙(警告:会中断采集)
         *   清零  (310, 130, 145, 36) — 灰(运行中按意义不大,但代码不强校验) */
        api->ui_btn_init(&s_wq.btn_loop_start, 10,  130, 145, 36, "开始", on_loop_start);
        api->ui_btn_init(&s_wq.btn_loop_stop,  160, 130, 145, 36, "停止", on_loop_stop);
        api->ui_btn_init(&s_wq.btn_loop_clear, 310, 130, 145, 36, "清零", on_loop_clear);
        if (api->ui_btn_set_color) {
            api->ui_btn_set_color(&s_wq.btn_loop_start, 0x03E0, 0x07FF, 0x0000);  // 绿底/青按
            api->ui_btn_set_color(&s_wq.btn_loop_stop,  0xFD20, 0xF800, 0x0000);  // 橙底/红按 (停止警告色)
            api->ui_btn_set_color(&s_wq.btn_loop_clear, 0x2945, 0x07FF, 0xFFFF);  // 灰底/青按
        }
        s_wq.btn_loop_start.user_data = &s_wq;
        s_wq.btn_loop_stop.user_data  = &s_wq;
        s_wq.btn_loop_clear.user_data = &s_wq;
    }

    /* 默认电平模式:把 RX 配成输入 (TX 在 LEVEL 模式不需要驱动) */
    if (api->gpio_set_dir) {
        api->gpio_set_dir(s_wq.rx_pin, false);
    }

    if (api->post_log) {
        api->post_log("Wire Quality Tester Ready");
    }
    return true;
}

/* ================= 5. Core 1 主循环 (按 mode 分支) ================= */

static void wire_quality_update_core1(void) {
    if (!s_wq.api) return;

    /* ⚡ 一次性"心跳日志":Core 1 worker 第一次调到本函数时,post_log 一行。
     * 用户在 USB CDC 监视器里看到这行 = Core 1 活着。
     * 看不到 = Core 1 没启动或 app_entry 之后某处崩了 */
    static uint32_t s_first_call_logged = 0;
    if (s_first_call_logged == 0 && s_wq.api->post_log) {
        s_wq.api->post_log(s_wq.api_ok
            ? "[wire_quality] Core1 alive (api_ok)"
            : "[wire_quality] Core1 alive BUT gpio API missing -> tests will silently no-op");
        s_first_call_logged = 1;
    }

    switch (s_wq.mode) {
    case MODE_LEVEL: {
        /* 单引脚电平读取 */
        if (!s_wq.api->gpio_get) break;
        uint8_t cur = s_wq.api->gpio_get(s_wq.tx_pin) ? 1 : 0;
        if (cur != s_wq.level) {
            s_wq.level = cur;
            if (s_wq.api->request_redraw) s_wq.api->request_redraw();
        }
        if (s_wq.api->delay_ms) s_wq.api->delay_ms(50);
        break;
    }

    case MODE_CONT: {
        /* ⚡ 双重弱上下拉真值表 — 解决 OPEN 与 SHORT-VCC 歧义
         *
         * 单次 PU 测量时:
         *   PASS      → PU: 1,0 (RX 跟随 TX)
         *   OPEN      → PU: 1,1 (上拉锁 HIGH)
         *   SHORT-GND → PU: 0,0 (短路把 RX 拉到 0)
         *   SHORT-VCC → PU: 1,1 (短路把 RX 拉到 1)
         *
         * OPEN 和 SHORT-VCC 在 PU 模式下签名相同,无法区分!
         * 加上 PD (下拉) 二次测量:
         *   PASS      → PD: 1,0 (RX 跟随 TX,下拉被 TX LOW 克服)
         *   OPEN      → PD: 0,0 (浮空被下拉拉到 0)
         *   SHORT-GND → PD: 0,0
         *   SHORT-VCC → PD: 1,1 (短路锁 HIGH,下拉被短路克服)
         *
         * 4-bit 真值表 (bit3=PU_HIGH, bit2=PU_LOW, bit1=PD_HIGH, bit0=PD_LOW):
         *   0b1010 = 10 = PASS
         *   0b1100 = 12 = OPEN       ← PD 把浮空拉低,与 SHORT-VCC 区分开!
         *   0b0000 =  0 = SHORT-GND
         *   0b1111 = 15 = SHORT-VCC
         *   其它    = AMBIGUOUS (接触抖动/外力干扰,需要去抖) */
        if (!s_wq.api->gpio_get || !s_wq.api->gpio_put || !s_wq.api->gpio_set_dir) break;

        /* 同引脚保护:TX == RX 时信号必然相等,测试无意义 */
        if (s_wq.tx_pin == s_wq.rx_pin) {
            if (s_wq.cont_result != CONT_SAME_PIN) {
                s_wq.cont_result = CONT_SAME_PIN;
                if (s_wq.api->request_redraw) s_wq.api->request_redraw();
            }
            if (s_wq.api->delay_ms) s_wq.api->delay_ms(50);
            break;
        }

        s_wq.api->gpio_set_dir(s_wq.tx_pin, true);
        s_wq.api->gpio_set_dir(s_wq.rx_pin, false);
        if (s_wq.api->gpio_set_pulls) {
            s_wq.api->gpio_set_pulls(s_wq.tx_pin, false, false);  // TX 推挽,关掉所有拉
        }

        /* ===== 周期 1:RX 上拉 ===== */
        if (s_wq.api->gpio_set_pulls) {
            s_wq.api->gpio_set_pulls(s_wq.rx_pin, true, false);
        }
        s_wq.api->gpio_put(s_wq.tx_pin, true);
        if (s_wq.api->delay_us) s_wq.api->delay_us(1000);
        bool pu_high = s_wq.api->gpio_get(s_wq.rx_pin);
        s_wq.api->gpio_put(s_wq.tx_pin, false);
        if (s_wq.api->delay_us) s_wq.api->delay_us(1000);
        bool pu_low  = s_wq.api->gpio_get(s_wq.rx_pin);

        /* ===== 周期 2:RX 下拉 ===== */
        if (s_wq.api->gpio_set_pulls) {
            s_wq.api->gpio_set_pulls(s_wq.rx_pin, false, true);
        }
        s_wq.api->gpio_put(s_wq.tx_pin, true);
        if (s_wq.api->delay_us) s_wq.api->delay_us(1000);
        bool pd_high = s_wq.api->gpio_get(s_wq.rx_pin);
        s_wq.api->gpio_put(s_wq.tx_pin, false);
        if (s_wq.api->delay_us) s_wq.api->delay_us(1000);
        bool pd_low  = s_wq.api->gpio_get(s_wq.rx_pin);

        /* 收尾:RX 恢复 Hi-Z,免得残留拉电阻影响 LEVEL 模式 */
        if (s_wq.api->gpio_set_pulls) {
            s_wq.api->gpio_set_pulls(s_wq.rx_pin, false, false);
        }

        uint8_t sig = ((pu_high ? 1u : 0u) << 3)
                    | ((pu_low  ? 1u : 0u) << 2)
                    | ((pd_high ? 1u : 0u) << 1)
                    | ((pd_low  ? 1u : 0u) << 0);

        uint8_t r;
        switch (sig) {
            case 0b1010: r = CONT_PASS;       break;
            case 0b1100: r = CONT_OPEN;       break;
            case 0b0000: r = CONT_SHORT_GND;  break;
            case 0b1111: r = CONT_SHORT_VCC;  break;
            default:     r = CONT_AMBIGUOUS;  break;  // 抖动/外力,需检查接触
        }

        if (r != s_wq.cont_result) {
            s_wq.cont_result = r;
            if (s_wq.api->request_redraw) s_wq.api->request_redraw();
        }
        if (s_wq.api->delay_ms) s_wq.api->delay_ms(50);
        break;
    }

    case MODE_LOOP: {
        /* 丢包率测试: 循环发 0x55 (01010101),每 bit 100µs (~10 kbps)
         * 每帧 8 bit ≈ 800µs,每 64 bit (~6.4ms) 或出错时请求重画 */
        if (!s_wq.api->gpio_get || !s_wq.api->gpio_put || !s_wq.api->gpio_set_dir) break;

        if (!s_wq.loop_running) {
            if (s_wq.api->delay_ms) s_wq.api->delay_ms(50);
            break;
        }

        s_wq.api->gpio_set_dir(s_wq.tx_pin, true);
        s_wq.api->gpio_set_dir(s_wq.rx_pin, false);

        uint32_t prev_sent = s_wq.bits_sent;
        uint32_t prev_err  = s_wq.bit_errors;

        /* 一次跑 64 bit (~6.4ms),保证 UI 响应 */
        for (int b = 0; b < 64; b++) {
            bool bit = ((0x55u >> (b & 7)) & 1u) != 0;
            s_wq.api->gpio_put(s_wq.tx_pin, bit);
            if (s_wq.api->delay_us) s_wq.api->delay_us(100);
            bool rx = s_wq.api->gpio_get(s_wq.rx_pin);
            s_wq.bits_sent++;
            if (rx != bit) s_wq.bit_errors++;
        }

        if (s_wq.bits_sent - prev_sent >= 64 ||
            s_wq.bit_errors - prev_err > 0) {
            if (s_wq.api->request_redraw) s_wq.api->request_redraw();
        }
        break;
    }

    default:
        if (s_wq.api->delay_ms) s_wq.api->delay_ms(50);
        break;
    }
}

/* ================= 6. 触控分发 (Core 0 上下文) ================= */

static void wire_quality_process_touch(bool is_touch, int16_t x, int16_t y) {
    if (!s_wq.api || !s_wq.api->ui_btn_process_touch) return;
    uint32_t now = s_wq.api->get_time_ms ? s_wq.api->get_time_ms() : 0u;
    s_wq.api->ui_btn_process_touch(&s_wq.btn_stop,        is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_tab_level,   is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_tab_cont,    is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_tab_loop,    is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_tx_pin,      is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_rx_pin,      is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_loop_start,  is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_loop_stop,   is_touch, x, y, now);
    s_wq.api->ui_btn_process_touch(&s_wq.btn_loop_clear,  is_touch, x, y, now);
}

/* ================= 7. 渲染 (Core 0 上下文) ================= */

static void wire_quality_draw(ui_tile_t *tile) {
    if (!tile || !s_wq.api) return;

    /* 全屏底色 */
    if (s_wq.api->ui_draw_fill_rect) {
        ui_rect_t full = ui_rect_make(0, 0, SCREEN_W, SCREEN_H);
        s_wq.api->ui_draw_fill_rect(tile, &full, 0xFFFF);
    }

    /* 标题栏 (Y 0-32) */
    if (s_wq.api->ui_draw_fill_rect) {
        ui_rect_t bar = ui_rect_make(0, 0, SCREEN_W, 32);
        s_wq.api->ui_draw_fill_rect(tile, &bar, 0x1A7B);
    }
    if (s_wq.api->ui_draw_string) {
        s_wq.api->ui_draw_string(tile, 12, 8, "杜邦线质量测量仪",
                                 FONT_20, 0xFFFF, 0x1A7B);
    }

    /* Tab 按钮 (Y 36-72) — 当前选中的 tab 用 color_bg (绿色) 持续高亮,
     * 其他 tab 用深灰。color_active 只对按下瞬间生效,所以之前用 color_active
     * 实现不了"持续高亮"。 */
    if (s_wq.api->ui_btn_draw) {
        const uint16_t SEL_COLOR  = 0x07E0;   // 选中:亮绿
        const uint16_t IDLE_COLOR = 0x2945;   // 未选:深灰
        s_wq.btn_tab_level.color_bg = (s_wq.mode == MODE_LEVEL) ? SEL_COLOR : IDLE_COLOR;
        s_wq.btn_tab_cont.color_bg  = (s_wq.mode == MODE_CONT)  ? SEL_COLOR : IDLE_COLOR;
        s_wq.btn_tab_loop.color_bg  = (s_wq.mode == MODE_LOOP)  ? SEL_COLOR : IDLE_COLOR;

        s_wq.api->ui_btn_draw(tile, &s_wq.btn_tab_level);
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_tab_cont);
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_tab_loop);
    }

    /* ⚡ API 状态徽章:右上角的小色块 + 文字
     * api_ok=✓(绿) | api 缺=✗(红)+提示
     * 一眼能判断 host 固件是不是最新的 */
    if (s_wq.api->ui_draw_fill_rect && s_wq.api->ui_draw_string) {
        const char  *badge_txt = s_wq.api_ok ? "API v" : "API: REFHOST!";
        uint16_t     badge_bg  = s_wq.api_ok ? 0x05E0 : 0xF800;
        uint16_t     badge_fg  = s_wq.api_ok ? 0x0000 : 0xFFFF;
        ui_rect_t bg = ui_rect_make(SCREEN_W - 100, 4, 96, 22);
        s_wq.api->ui_draw_fill_rect(tile, &bg, badge_bg);
        s_wq.api->ui_draw_string(tile, SCREEN_W - 92, 8,
                                 badge_txt, FONT_16, badge_fg, badge_bg);
    }

    /* TX / RX 引脚按钮 label 动态更新 — 写到 state.tx_lbl / state.rx_lbl (BSS 常驻),
     * init 时已经把 ui_btn_t.text 指到这两个 buffer,现在直接覆写即可 */
    s_wq.tx_lbl[0] = 'T'; s_wq.tx_lbl[1] = 'X'; s_wq.tx_lbl[2] = ':'; s_wq.tx_lbl[3] = ' ';
    gp_to_str(s_wq.tx_pin, s_wq.tx_lbl + 4, sizeof(s_wq.tx_lbl) - 4);
    s_wq.rx_lbl[0] = 'R'; s_wq.rx_lbl[1] = 'X'; s_wq.rx_lbl[2] = ':'; s_wq.rx_lbl[3] = ' ';
    gp_to_str(s_wq.rx_pin, s_wq.rx_lbl + 4, sizeof(s_wq.rx_lbl) - 4);

    /* TX/RX 按钮 (Y 84-120) */
    if (s_wq.api->ui_btn_draw) {
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_tx_pin);
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_rx_pin);
    }

    /* 丢包模式额外画 START / STOP / CLEAR 三个按钮 */
    if (s_wq.mode == MODE_LOOP && s_wq.api->ui_btn_draw) {
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_loop_start);
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_loop_stop);
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_loop_clear);
    }

    /* ============== 状态显示区 (Y 170-280) ============== */
    if (s_wq.api->ui_draw_fill_rect) {
        ui_rect_t box = ui_rect_make(10, 175, SCREEN_W - 20, 95);
        s_wq.api->ui_draw_fill_rect(tile, &box, 0xEF7D);
        /* 用两条 fast_hline 画上下边框(横线) — ui_draw_rect 不在 host API 里 */
        ui_rect_t top    = ui_rect_make(10, 175, SCREEN_W - 20, 1);
        ui_rect_t bottom = ui_rect_make(10, 269, SCREEN_W - 20, 1);
        s_wq.api->ui_draw_fast_hline(tile, top.x,    top.y,    top.w,    0x4208);
        s_wq.api->ui_draw_fast_hline(tile, bottom.x, bottom.y, bottom.w, 0x4208);
    }

    if (s_wq.api->ui_draw_string) {
        char line[24];
        if (s_wq.mode == MODE_LEVEL) {
            char gp_lbl[8];
            gp_to_str(s_wq.tx_pin, gp_lbl, sizeof(gp_lbl));
            s_wq.api->ui_draw_string(tile, 24, 184, "电平模式  GPx:",
                                     FONT_16, 0x0000, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 200, 184, gp_lbl,
                                     FONT_20, 0x0000, 0xEF7D);

            const char *st_str = s_wq.level ? "HIGH (高电平)" : "LOW  (低电平)";
            uint16_t st_col   = s_wq.level ? 0x05E0 : 0xF800;
            s_wq.api->ui_draw_string(tile, 24, 220, st_str,
                                     FONT_24, st_col, 0xEF7D);
        }
        else if (s_wq.mode == MODE_CONT) {
            /* 第一行: 标签 */
            char gp_tx[8], gp_rx[8];
            gp_to_str(s_wq.tx_pin, gp_tx, sizeof(gp_tx));
            gp_to_str(s_wq.rx_pin, gp_rx, sizeof(gp_rx));
            s_wq.api->ui_draw_string(tile, 24, 184, "通断测试  TX=",
                                     FONT_16, 0x0000, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 130, 184, gp_tx,
                                     FONT_16, 0x001F, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 168, 184, "RX=",
                                     FONT_16, 0x0000, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 198, 184, gp_rx,
                                     FONT_16, 0x001F, 0xEF7D);

            /* 第二行: 判决结果(大字) */
            const char *r_str;
            uint16_t r_col;
            switch (s_wq.cont_result) {
                case CONT_PASS:       r_str = "PASS 通";          r_col = 0x05E0; break;
                case CONT_OPEN:       r_str = "OPEN 断/浮空";     r_col = 0xF800; break;
                case CONT_SHORT_GND:  r_str = "SHORT 对地短路";   r_col = 0xF800; break;
                case CONT_SHORT_VCC:  r_str = "SHORT 对VCC短路";  r_col = 0xF800; break;
                case CONT_SAME_PIN:   r_str = "TX=RX 同引脚";     r_col = 0xFD20; break;
                case CONT_AMBIGUOUS:  r_str = "? 抖动/接触不良";  r_col = 0xFD20; break;
                default:              r_str = "?";                r_col = 0x0000; break;
            }
            s_wq.api->ui_draw_string(tile, 24, 212, r_str,
                                     FONT_24, r_col, 0xEF7D);

            /* 第三行: 提示 (小字,排查用) */
            const char *tip;
            switch (s_wq.cont_result) {
                case CONT_PASS:       tip = "TX=RX 同步 → 杜邦线通 (PU+PD 都同步)";   break;
                case CONT_OPEN:       tip = "PU锁高 PD锁低 → 线悬空/接触不良";          break;
                case CONT_SHORT_GND:  tip = "PU/PD 都低 → 线短到 GND";                  break;
                case CONT_SHORT_VCC:  tip = "PU/PD 都高 → 线短到 3.3V";                 break;
                case CONT_SAME_PIN:   tip = "TX 与 RX 选了同一个 GPIO,无法测试";        break;
                case CONT_AMBIGUOUS:  tip = "PU/PD 签名不一致 → 接触抖动,压紧重测";     break;
                default:              tip = "";                                          break;
            }
            s_wq.api->ui_draw_string(tile, 24, 244, tip, FONT_16, 0x4208, 0xEF7D);
        }
        else { /* MODE_LOOP */
            s_wq.api->ui_draw_string(tile, 24, 184, "丢包测试 (0x55 @ 10kbps)",
                                     FONT_16, 0x0000, 0xEF7D);

            /* 运行状态指示器 — 直观告诉用户 loop_running 是否被设置 */
            const char *run_lbl = s_wq.loop_running ? "● 采集中" : "○ 已停止";
            uint16_t run_col   = s_wq.loop_running ? 0x05E0 : 0x4208;
            s_wq.api->ui_draw_string(tile, 280, 184, run_lbl, FONT_16, run_col, 0xEF7D);

            /* Sent / Err 数字行 */
            char sent_buf[16], err_buf[16];
            int_to_str((int32_t)s_wq.bits_sent, sent_buf, sizeof(sent_buf));
            int_to_str((int32_t)s_wq.bit_errors, err_buf, sizeof(err_buf));

            s_wq.api->ui_draw_string(tile, 24, 210, "Sent:", FONT_16, 0x0000, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 80, 210, sent_buf, FONT_16, 0x001F, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 200, 210, "Err:",  FONT_16, 0x0000, 0xEF7D);
            s_wq.api->ui_draw_string(tile, 256, 210, err_buf, FONT_16, 0xF800, 0xEF7D);

            /* BER% 行 */
            if (s_wq.bits_sent > 0) {
                int ber_x10 = (int)((int64_t)s_wq.bit_errors * 1000 / s_wq.bits_sent);
                int ber_int = ber_x10 / 10;
                int ber_dec = ber_x10 % 10;
                line[0] = 'B'; line[1] = 'E'; line[2] = 'R'; line[3] = ':'; line[4] = ' ';
                int_to_str((int32_t)ber_int, line + 5, sizeof(line) - 5);
                int p = 5;
                while (line[p] != '\0') p++;
                if (p < (int)sizeof(line) - 2) {
                    line[p++] = '.';
                    line[p++] = (char)('0' + ber_dec);
                    line[p]   = '\0';
                }
                if (p < (int)sizeof(line) - 1) {
                    line[p++] = '%';
                    line[p]   = '\0';
                }
                uint16_t col = (s_wq.bit_errors == 0) ? 0x05E0 : 0xF800;
                s_wq.api->ui_draw_string(tile, 24, 240, line, FONT_20, col, 0xEF7D);
            } else {
                s_wq.api->ui_draw_string(tile, 24, 240, "BER: -- (点开始)",
                                         FONT_20, 0x4208, 0xEF7D);
            }
        }
    }

    /* 提示行 (Y 290-315) */
    if (s_wq.api->ui_draw_string) {
        const char *hint;
        switch (s_wq.mode) {
            case MODE_LEVEL:
                hint = "提示: 选择 GPIO,观察电平 (无源读取)";
                break;
            case MODE_CONT:
                hint = "提示: TX 接一端,RX 接另一端,通断自动判";
                break;
            case MODE_LOOP:
                hint = "提示: TX/RX 短接,点开始测丢包率";
                break;
            default:
                hint = "";
                break;
        }
        s_wq.api->ui_draw_string(tile, 10, 286, hint, FONT_16, 0x4208, 0xFFFF);
    }

    /* STOP 按钮 (Y 270-310, 右下角) */
    if (s_wq.api->ui_btn_draw) {
        s_wq.api->ui_btn_draw(tile, &s_wq.btn_stop);
    }
}

/* ================= 8. 退出清理 (把 TX/RX 还原成输入,免得烧目标) ================= */

static void wire_quality_exit(void) {
    if (s_wq.api) {
        if (s_wq.api->gpio_set_dir) {
            s_wq.api->gpio_set_dir(s_wq.tx_pin, false);
            s_wq.api->gpio_set_dir(s_wq.rx_pin, false);
        }
        if (s_wq.api->post_log) {
            s_wq.api->post_log("Wire Quality Tester Stopped.");
        }
        s_wq.api = NULL;
    }
}

/* ================= 9. 描述符单例 ================= */

static const app_plugin_t s_wire_quality_plugin = {
    .magic         = APP_MAGIC,        /* 0x50415050 ("PAPP") */
    .version       = 0x00010000u,
    .name          = "Wire Quality",
    .init          = wire_quality_init,
    .update_core1  = wire_quality_update_core1,
    .process_touch = wire_quality_process_touch,
    .draw          = wire_quality_draw,
    .exit          = wire_quality_exit,
};


const app_plugin_t *wire_quality_get_descriptor(void) {
    return &s_wire_quality_plugin;
}
