#include "app_dap.h"
#include "app_shared_ram.h"
#include "ui_tabview.h"
#include "ui_scrollview.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_pin_config.h"
#include "ui_toast.h"
#include "ui_switch.h"
#include "ui_modal.h"
#include "ff.h"
#include "lcd.h"
#include <stdbool.h>
#include "stdio.h"
#include <stdint.h>
#include <string.h>
#include "cmsis_dap.h"
#include "fun_pio_swd.h"
#include "fun_swd_config.h"
#include "pin_manager.h"
#include "pico/time.h"
#include "sym_loader.h"
#include "thumb_disasm.h"
#include "stdlib.h"
#include "ui_input.h"
#include "ui_keyboard.h"
#include "ui_terminal.h"
#include "pico/multicore.h"
extern bool cmsis_dap_is_host_active(void);
static const char *s_dap_tabs[] = { "状态", "配置", "终端","监控","日志" };
#define DAP_TAB_COUNT          ((uint8_t)(sizeof(s_dap_tabs) / sizeof(s_dap_tabs[0])))

#define DAP_STATUS_CONTENT_H   260
#define DAP_CONFIG_CONTENT_H   360   //260
#define DAP_TERM_CONTENT_H     360
#define DAP_WATCH_CONTENT_H    360
#define MAX_WATCH_ITEMS 6
#define MAX_SYM_ITEMS 16

#define RTT_TERM_X       120
#define RTT_TERM_Y       38
#define RTT_TERM_W       350
#define RTT_TERM_H       226

#define RTT_BTN_CLEAR_X   352
#define RTT_BTN_CLEAR_Y   272
#define RTT_BTN_CLEAR_W   56
#define RTT_BTN_CLEAR_H   36

#define RTT_BTN_SEARCH_X  230
#define RTT_BTN_SEARCH_Y  272
#define RTT_BTN_SEARCH_W  110
#define RTT_BTN_SEARCH_H  36

#define RTT_QUEUE_SIZE    16
#define RTT_LINE_SIZE     64

typedef struct {
    char text[RTT_LINE_SIZE];
    uint16_t color;
} rtt_term_msg_t;
static rtt_term_msg_t s_rtt_queue[RTT_QUEUE_SIZE];
static volatile uint8_t s_rtt_head = 0;
static volatile uint8_t s_rtt_tail = 0;
static volatile bool    s_dap_session_active = false;
static char          s_rtt_tx_buf[64];
static volatile bool s_rtt_tx_pending = false;
typedef struct {
    uint32_t addr;
    char name[32];
    uint8_t size;
    uint32_t val;
    uint32_t last_val;
    uint32_t change_tick;
    bool valid;

}dap_watch_item_t;
typedef enum
{
    DAP_CORE_UNKNOWN = 0,
    DAP_CORE_ARM_CORTEX_M0,
    DAP_CORE_ARM_CORTEX_M0PLUS,
    DAP_CORE_ARM_CORTEX_M3,
    DAP_CORE_ARM_CORTEX_M4,
    DAP_CORE_ARM_CORTEX_M7,
    DAP_CORE_RISCV
} dap_core_type_t;
typedef enum
{
    DAP_PROBE_IDLE = 0,
    DAP_PROBE_OPEN_FAILED,
    DAP_PROBE_DP_FAILED,
    DAP_PROBE_POWER_FAILED,
    DAP_PROBE_SUCCESS
} dap_probe_status_t;
typedef struct {
    char name[48];
    uint32_t size;
    bool is_dir;
}sym_file_item_t;
typedef struct {
    uint16_t sym_idx;   // 对应 sym_loader 的索引 (0xFFFF 表示 "..")
    uint8_t  is_dir;    // 是否为文件夹
    uint8_t  dir_id;    // 如果是文件夹，记录外设编号
} var_view_item_t;
typedef struct {
    /* 必须放在首字段供 app_manager 读取脏矩形 */
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab_idx;

    /* 每个 Tab 独立的滚动视口 */
    ui_scrollview_t status_scroll;
    ui_scrollview_t config_scroll;
    ui_scrollview_t term_scroll;

    /* Tab 0: 状态页组件 */
    ui_rect_t status_card;
    ui_btn_t pio_probe;
    bool target_detected;
    fun_swd_config_t swd;
    fun_pio_swd_t pio_swd;
    uint32_t dp_id;
    uint32_t cpu_id;
    uint32_t dbg_id;
    uint32_t flash_size;
    uint8_t pio_ack;
    uint8_t pio_ctrl_ack;
    uint32_t pio_ctrl_stat;
    dap_probe_status_t probe_status;

    bool  pio_powered;

    /* Tab 1: 配置页组件（预留扩展） */
    ui_pin_config_t pin_swdio_cf;
    ui_pin_config_t pin_swclk_cf;
    ui_btn_t btn_apply;
    ui_toast_t toast;
    ui_switch_t dap_switch;

    ui_modal_t sym_modal;
    ui_btn_t  choose_sym;
    char sym_file_path[128];         // 当前选中的路径 (默认 "0:/app.sym")
    char modal_cur_path[128];        // 弹窗当前浏览的目录 (如 "0:/")
    sym_file_item_t sym_items[MAX_SYM_ITEMS];
    uint8_t sym_item_count;
    FATFS fs;
    bool fs_mounted;
    /* Tab 2: 终端页组件（预留扩展） */
    ui_btn_t run;
    ui_btn_t halt;
    ui_btn_t step;
    ui_btn_t reset;
    ui_btn_t step_over;
    ui_btn_t step_out;

    uint32_t core_regs[17];
    bool regs_valid;
    bool is_halted;
    ui_rect_t reg_card;
    ui_btn_t view_toggle;
    bool show_disasm;

    ui_btn_t auto_step;
    bool auto_step_enablde;
    uint32_t auto_step_last_tick;
    uint32_t auto_step_ms;

    uint32_t bp_addr;
    bool bp_active;
    ui_btn_t bp;
    ui_btn_t clear_bp;
    ui_modal_t bp_modal;
    uint32_t last_halt_check;
    //tab3
    ui_scrollview_t watch_scroll;
    dap_watch_item_t watch_items[MAX_WATCH_ITEMS];
    uint8_t watch_count;
    ui_btn_t add_watch;
    ui_btn_t clear_watch;
    ui_modal_t var_modal;
    ui_modal_t bit_modal;
    uint32_t last_watch_tick;

    ui_keyboard_t kb;
    ui_input_t input;
    int8_t watch_idx;
    var_view_item_t var_view_items[64]; // 40 * 4 = 仅 160 字节！
    uint16_t        var_view_count;
    char            var_cur_dir[24];    // 缩减为 16 字节
    
    ui_terminal_t rtt_terminal;
    ui_btn_t rtt_clear;
    ui_btn_t rtt_search;
    bool rtt_found;
    ui_btn_t      rtt_send;
    uint32_t      rtt_cb_addr;     // SEGGER RTT 控制块物理地址
    uint32_t      rtt_buf_addr; // 上行 Buffer (MCU -> Host) 物理首地址
    int32_t       rtt_buf_size; // Buffer 尺寸
    int32_t       rtt_last_rd_off; // 上次已消费的偏移
    uint32_t rtt_down_buf_addr;
    int32_t rtt_down_buf_size;
    uint32_t rtt_down_cb_addr;
    volatile bool rtt_worker_enable;
    volatile bool rtt_core1_idle;



} dap_data_t;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b)
{
    if (!a.w || !a.h) return b;
    if (!b.w || !b.h) return a;
    int16_t x1 = a.x < b.x ? a.x : b.x;
    int16_t y1 = a.y < b.y ? a.y : b.y;
    int16_t x2 = (a.x + a.w) > (b.x + b.w) ? a.x + a.w : b.x + b.w;
    int16_t y2 = (a.y + a.h) > (b.y + b.h) ? a.y + a.h : b.y + b.h;
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ============================================================================
 * Tab 0: 状态页 (Status)
 * ============================================================================ */
static const char *dap_get_core_name(uint32_t cpuid,dap_core_type_t *type)
{
    uint32_t ip = (cpuid >>24)&0xFF;
    uint32_t port = (cpuid>>4)&0x0FFF;
    if(ip !=0x41){
        if(type) *type=DAP_CORE_UNKNOWN;
        return "CORE UNKNOWN" ;
    }
    switch(port){
        case 0xc20:
        if(type) *type=DAP_CORE_ARM_CORTEX_M0;
        return "Cortex-M0" ;
        case 0xc60:
        if(type) *type=DAP_CORE_ARM_CORTEX_M0PLUS;
        return "Cortex-M0+" ;
        case 0xc23:
        if(type) *type=DAP_CORE_ARM_CORTEX_M3;
        return "Cortex-M3" ;
        case 0xc24:
        if(type) *type=DAP_CORE_ARM_CORTEX_M4;
        return "Cortex-M4" ;
        case 0xc37:
        if(type) *type=DAP_CORE_ARM_CORTEX_M7;
        return "Cortex-M7" ;
        default:
        if(type) *type=DAP_CORE_UNKNOWN;
        return "ARM Cortex-M" ;
    }
}
void app_dap_force_close_swd(void)
{
    dap_data_t *data = (dap_data_t *)g_static_app_ram;
    if (data && data->pio_swd.opened) {
        fun_pio_swd_close(&data->pio_swd);
        data->pio_powered = false;
    }
}
static void dap_read_all_regs(dap_data_t *data)
{
    if(!data|| !data->pio_swd.opened) return;
    for(uint8_t i=0;i<=16;i++){
        if(!fun_pio_swd_read_core_reg(&data->pio_swd,i,&data->core_regs[i]))
            data->core_regs[i] = 0x0000000000;
    }
    data->regs_valid=true;
}
static bool swd_open(dap_data_t *data)
{
    if(!data) return false;
    if (data->pio_swd.opened) return true;
    const fun_swd_config_t *cfg = fun_swd_config_get();
    if(!cfg) return false;
    if(!fun_pio_swd_open_auto(&data->pio_swd,pio_manager_get_default(),cfg->swclk_gpio,cfg->swdio_gpio,cfg->clock_hz))
        return false;
    uint32_t id = 0;
    uint8_t ack=0;
    if(!fun_pio_swd_read_dp_idcode(&data->pio_swd,&id,&ack)||ack!=1){
        fun_pio_swd_close(&data->pio_swd);
        return false;
    } 
    if(!fun_pio_swd_power_up(&data->pio_swd)){
        fun_pio_swd_close(&data->pio_swd);
        return false;
    }
    return true;

}
static void dap_hardware_bp(dap_data_t *data)
{
    if(!data || !swd_open(data)) return;
    fun_pio_swd_fpb_init(&data->pio_swd);
    if(data->bp_active&&data->bp_addr!=0)
    fun_pio_swd_set_breakpoint_ch(&data->pio_swd,1,data->bp_addr);
    else fun_pio_swd_clear_breakpoint_ch(&data->pio_swd,1);

}
static inline void rtt_queue_push(const char *text ,uint16_t color)
{
    uint8_t next = (uint8_t)((s_rtt_head+1)%RTT_QUEUE_SIZE);
    if(next==s_rtt_tail) return;
    strncpy(s_rtt_queue[s_rtt_head].text,text,RTT_LINE_SIZE-1);
    s_rtt_queue[s_rtt_head].text[RTT_LINE_SIZE-1] ='\0';
    s_rtt_queue[s_rtt_head].color=color;
    s_rtt_head=next;

}
static bool rtt_search_cb(void *user_data)
{
    dap_data_t *data=(dap_data_t*) user_data;
    if(!data||!data->pio_swd.opened) return false;
    uint32_t scan_buf[32];
    const char magic[10]="SEGGER RTT";
    for(uint32_t addr = 0x20000000;addr < 0x20004000;addr+=128){
        if(!fun_pio_swd_read_memory_block32(&data->pio_swd,addr,scan_buf,32))
        continue;
        const char *p =(const char*)scan_buf;
        for(int i=0;i<128-16;i+=4){
            if(memcmp(p+i,magic,10)==0){
                data->rtt_cb_addr=addr+i;
                // 1. 读取 Up Buffer 0 参数 (接收日志)
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_cb_addr + 28, &data->rtt_buf_addr);
                uint32_t sz = 0, rd = 0;
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_cb_addr + 32, &sz);
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_cb_addr + 40, &rd);
                data->rtt_buf_size = (int32_t)sz;
                data->rtt_last_rd_off = (int32_t)rd;

                // 2. ⚡ 读取 Down Buffer 0 参数 (下发命令，位于 aUp 数组之后)[cite: 17]
                uint32_t max_up = 0;
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_cb_addr + 16, &max_up);
                if (max_up == 0 || max_up > 4) max_up = 2; // 默认防守值

                data->rtt_down_cb_addr = data->rtt_cb_addr + 24 + max_up * 24; // aDown[0] 首地址[cite: 17]
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_down_cb_addr + 4, &data->rtt_down_buf_addr);
                uint32_t down_sz = 0;
                fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_down_cb_addr + 8, &down_sz);
                data->rtt_down_buf_size = (int32_t)down_sz;

                data->rtt_found = true;
                return true;
            }
        }
    }
    data->rtt_found = false;
    return false;
}
static void utf8_to_gbk_string(const char *utf8, char *gbk_out, size_t max_out_len)
{
    if (!utf8 || !gbk_out || max_out_len == 0) return;

    size_t in_idx = 0;
    size_t out_idx = 0;

    while (utf8[in_idx] != '\0' && out_idx < max_out_len - 1) {
        uint8_t c = (uint8_t)utf8[in_idx];

        // 1. 单字节标准 ASCII (0x00 ~ 0x7F) 直接透传
        if (c < 0x80) {
            gbk_out[out_idx++] = (char)c;
            in_idx++;
        }
        // 2. 3 字节标准汉字 UTF-8 (0xE0 ~ 0xEF)
        else if ((c & 0xF0) == 0xE0 && utf8[in_idx + 1] && utf8[in_idx + 2]) {
            uint8_t c2 = (uint8_t)utf8[in_idx + 1];
            uint8_t c3 = (uint8_t)utf8[in_idx + 2];

            // 拼出 16 位 Unicode (UCS-2)
            WCHAR unicode = (WCHAR)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));

            // 调用 FatFs 的 Flash 对照表查出 GBK (代码页 936)
            DWORD gbk_code = ff_uni2oem(unicode, 936);

            if (gbk_code > 0xFF) {
                // 双字节汉字 (高字节在前，低字节在后)
                if (out_idx + 2 < max_out_len) {
                    gbk_out[out_idx++] = (char)(gbk_code >> 8);
                    gbk_out[out_idx++] = (char)(gbk_code & 0xFF);
                }
            } else if (gbk_code > 0) {
                gbk_out[out_idx++] = (char)gbk_code;
            } else {
                gbk_out[out_idx++] = '?'; // 未识别字符占位
            }
            in_idx += 3;
        }
        // 3. 2 字节特殊拉丁符号等 (0xC0 ~ 0xDF)
        else if ((c & 0xE0) == 0xC0 && utf8[in_idx + 1]) {
            uint8_t c2 = (uint8_t)utf8[in_idx + 1];
            WCHAR unicode = (WCHAR)(((c & 0x1F) << 6) | (c2 & 0x3F));
            DWORD gbk_code = ff_uni2oem(unicode, 936);
            if (gbk_code > 0 && out_idx < max_out_len - 1) {
                gbk_out[out_idx++] = (char)gbk_code;
            }
            in_idx += 2;
        }
        else {
            in_idx++; // 丢弃孤立的非法字节
        }
    }
    gbk_out[out_idx] = '\0';
}
static void rtt_send_cmd(dap_data_t *data, const char *cmd)
{
    if (!data || !cmd || cmd[0] == '\0' || !data->rtt_found || !data->pio_swd.opened) return;
    if (data->rtt_down_buf_size <= 0 || data->rtt_down_buf_addr == 0) return;

    uint32_t wr_off = 0, rd_off = 0;
    if (!fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_down_cb_addr + 12, &wr_off) ||
        !fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_down_cb_addr + 16, &rd_off)) {
        return;
    }

    int32_t sz = data->rtt_down_buf_size;
    char send_buf[64];
    snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd); // 自动追加回车换行
    size_t len = strlen(send_buf);

    for (size_t i = 0; i < len; i++) {
        uint32_t next_wr = (wr_off + 1) % sz;
        if (next_wr == rd_off) break; // 缓冲区满则停止写入

        uint32_t cur_addr = data->rtt_down_buf_addr + wr_off;
        uint32_t aligned_addr = cur_addr & ~3u;
        uint32_t shift = (cur_addr & 3u) * 8u;

        uint32_t word = 0;
        if (fun_pio_swd_read_memory32(&data->pio_swd, aligned_addr, &word)) {
            word = (word & ~(0xFFu << shift)) | (((uint8_t)send_buf[i]) << shift);
            fun_pio_swd_write_memory32(&data->pio_swd, aligned_addr, word);
            wr_off = next_wr;
        }
    }

    // 更新 WrOff 指针通知 STM32[cite: 16]
    fun_pio_swd_write_memory32(&data->pio_swd, data->rtt_down_cb_addr + 12, wr_off);
}
static void rtt_poll_data(void *user_data) {
    dap_data_t *data=(dap_data_t*) user_data;
    if (!data || !data->rtt_found || !data->pio_swd.opened) return;

    uint32_t wr_off = 0;
    // 读取单片机当前的写入指针
    if (!fun_pio_swd_read_memory32(&data->pio_swd, data->rtt_cb_addr + 36, &wr_off)) 
        return;
    if ((int32_t)wr_off == data->rtt_last_rd_off) return; // 暂无新数据

    // 计算待读字节量
    int32_t rd = data->rtt_last_rd_off;
    int32_t wr = (int32_t)wr_off;
    int32_t sz = data->rtt_buf_size;
    int32_t bytes_to_read = (wr >= rd) ? (wr - rd) : (sz - rd + wr);
    if (bytes_to_read > 256) bytes_to_read = 256; // 限制单批吞吐
    static char line_accum[RTT_LINE_SIZE];
    static size_t line_len = 0;
    for (int32_t i = 0; i < bytes_to_read; i++) {
        uint32_t char_word = 0;
        uint32_t cur_addr = data->rtt_buf_addr + rd;
        // SWD 按 4 字节读取并取字节
        if (fun_pio_swd_read_memory32(&data->pio_swd, cur_addr & ~3u, &char_word)) {
            uint8_t ch = (uint8_t)(char_word >> ((cur_addr & 3u) * 8u));
            if (ch == '\r') {
                // 忽略 CR
            } else if (ch == '\n' || line_len >= RTT_LINE_SIZE - 1) {
                line_accum[line_len] = '\0';
                char gbk_converted[RTT_LINE_SIZE];
                utf8_to_gbk_string(line_accum, gbk_converted, sizeof(gbk_converted));
                rtt_queue_push(gbk_converted, 0x5E7F); // 经典终端亮绿字
                line_len = 0;
            } else {
                line_accum[line_len++] = (char)ch;
            }
        }
        rd = (rd + 1) % sz;
    }
    data->rtt_last_rd_off = rd;
    fun_pio_swd_write_memory32(&data->pio_swd, data->rtt_cb_addr + 40, (uint32_t)rd);
}
static void dap_core1_rtt_worker(void) {
    dap_data_t *data = (dap_data_t *)g_static_app_ram;
    uint32_t last_search = 0;

    while (true) {
        // 避让保护：未激活会话、电脑 Keil 占用总线、或者用户不在“日志”页时，Core 1 静默避让
        if (!s_dap_session_active || !data->rtt_worker_enable || cmsis_dap_is_host_active() || data->tabview.active_tab_idx != 4) {
            data->rtt_core1_idle = true;
            sleep_ms(20);
            continue;
        }

        data->rtt_core1_idle = false;
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 未锁定 RTT 控制块时，每 1 秒尝试重搜一次
        if (!data->rtt_found) {
            if (now - last_search >= 1000) {
                last_search = now;
                if (swd_open(data)) {
                    if (rtt_search_cb(data)) {
                        rtt_queue_push("[系统] 已成功锁定 SEGGER RTT 控制块!", 0x07E0);
                    }
                }
            }
            sleep_ms(50);
            continue;
        }
        if (s_rtt_tx_pending) {
            rtt_send_cmd(data, s_rtt_tx_buf);
            s_rtt_tx_pending = false;
        }
        // 已锁定控制块，高频轮询新字符
        rtt_poll_data(data);
        sleep_ms(15);
    }
}
static void on_rtt_input_focus(ui_input_t *input, void *user_data) {
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    data->kb.mode = UI_KB_MODE_LOWER; // 弹出全功能英文软键盘
    ui_keyboard_open(&data->kb, input);
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}

static void on_rtt_send_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || data->input.text[0] == '\0') return;

    if (!s_rtt_tx_pending) {
        strncpy(s_rtt_tx_buf, data->input.text, sizeof(s_rtt_tx_buf) - 1);
        s_rtt_tx_buf[sizeof(s_rtt_tx_buf) - 1] = '\0';
        s_rtt_tx_pending = true;
        ui_input_clear(&data->input);
        data->dirty_rect = rect_union_box(data->dirty_rect, data->input.rect);
    }
}

static void on_rtt_clear_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    ui_terminal_clear(&data->rtt_terminal);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_terminal.rect);
}

static void on_rtt_search_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    data->rtt_found = false;
    ui_toast_show(&data->toast, "正在重新搜索 RTT...", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
}
static void bp_step_out(dap_data_t *data)
{
    if(!data || !data->bp_active||data->bp_addr==0) return ;
    uint32_t pc =data->core_regs[15] & ~1u;
    if(pc==data->bp_addr){
        fun_pio_swd_clear_breakpoint_ch(&data->pio_swd,1);
        fun_pio_swd_step_target(&data->pio_swd,100);
        fun_pio_swd_set_breakpoint_ch(&data->pio_swd,1,data->bp_addr);
    }
}
/* ============================================================================
 * 断点函数选择弹窗回调
 * ============================================================================ */

static void on_bp_modal_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    uint16_t total = sym_loader_get_func_count();
    int16_t item_h = 34;

    for (uint16_t i = 0; i < total; i++) {
        int16_t item_y = viewport->y + i * item_h - scroll_y;
        if (item_y + item_h < tile->rect.y || item_y > tile->rect.y + tile->rect.h) {
            continue; // 粗裁剪
        }

        const sym_func_entry_t *fn = sym_loader_get_func(i);
        if (!fn) continue;

        bool is_current_bp = (data->bp_active && data->bp_addr == fn->addr);

        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, item_y + 2, viewport->w - 8, item_h - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
            // 当前断点项使用淡红底，其余使用纯白底
            ui_draw_fill_rect(tile, &overlap, is_current_bp ? 0xFDEE : 0xFFFF);
            ui_draw_rect(tile, &item_rect, is_current_bp ? 0xF800 : 0xCE59);
        }

        const char *name = sym_loader_get_func_name(fn);

        // 如果是已设断点的函数，左侧绘制醒目的红点标识
        if (is_current_bp) {
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 7, "[BP] ", FONT_16, 0xF800, 0xFDEE);
            ui_draw_string(tile, item_rect.x + 46, item_rect.y + 7, name, FONT_16, 0xF800, 0xFDEE);
        } else {
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 7, name, FONT_16, 0x1A7B, 0xFFFF);
        }

        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "%08lX", (unsigned long)fn->addr);
        uint16_t bg_col = is_current_bp ? 0xFDEE : 0xFFFF;
        ui_draw_string(tile, item_rect.x + item_rect.w - 76, item_rect.y + 7, addr_str, FONT_16, 0x8410, bg_col);
    }
}
static bool on_bp_modal_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || !modal) return false;

    ui_rect_t vr = modal->sv_body.view_rect;
    bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) && ty >= vr.y && ty < (vr.y + vr.h));

    static bool    s_was_touching = false;
    static int16_t s_down_idx = -1;
    static int16_t s_down_y = 0;
    static int16_t s_down_scroll_y = 0;

    int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
    int idx = (in_viewport && rel_y >= 0) ? (rel_y / 34) : -1;

    // 1. 手指刚按下一瞬间：记录初始 Y 坐标和初始滚动位置
    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_down_idx = idx;
        s_down_y = ty;
        s_down_scroll_y = modal->sv_body.scroll_y;
        return false;
    }

    // 2. 手指松开一瞬间
    if (!is_touching && s_was_touching) {
        s_was_touching = false;

        // ⚡ 核心修复：手指移动超过 6 像素，或者视口发生了任何位移，一律判定为滑动拖拽，丢弃点击！
        int16_t dy = ty - s_down_y;
        int16_t dscroll = modal->sv_body.scroll_y - s_down_scroll_y;
        if (dy < -6 || dy > 6 || dscroll != 0 || modal->sv_body.is_dragged) {
            s_down_idx = -1;
            return false;
        }

        uint16_t total = sym_loader_get_func_count();
        if (s_down_idx >= 0 && s_down_idx == idx && s_down_idx < total) {
            const sym_func_entry_t *fn = sym_loader_get_func(s_down_idx);
            s_down_idx = -1;
            if (fn) {
                if (data->bp_active && data->bp_addr == fn->addr) {
                    data->bp_active = false;
                    data->bp_addr = 0;
                    dap_hardware_bp(data);
                    ui_toast_show(&data->toast, "断点已清除", UI_TOAST_INFO, now_tick);
                } else {
                    data->bp_addr = fn->addr;
                    data->bp_active = true;
                    dap_hardware_bp(data);

                    char msg[48];
                    snprintf(msg, sizeof(msg), "断点 -> %s", sym_loader_get_func_name(fn));
                    ui_toast_show(&data->toast, msg, UI_TOAST_SUCCESS, now_tick);
                }

                if (data->bp_active) {
                    ui_btn_set_color(&data->bp, 0xF800, 0x8000, 0xFFFF);
                } else {
                    ui_btn_set_color(&data->bp, 0xEF7D, 0xA514, 0x4208);
                }

                ui_modal_hide(modal);
            }
            return true;
        }
        s_down_idx = -1;
    }
    return false;
}

// 终端页底栏 [断点] 按钮点击回调
static void on_bp_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    uint16_t cnt = sym_loader_get_func_count();
    if (cnt == 0) {
        ui_toast_show(&data->toast, "请先在配置页加载符号表!", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time()));
        return;
    }

    // 动态撑开弹窗内容高度并打开弹窗
    data->bp_modal.sv_body.content_h = cnt * 34 + 8;
    ui_modal_show(&data->bp_modal, "设置函数断点", data);
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
static void  on_clear_bp_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    uint32_t now_tick = to_ms_since_boot(get_absolute_time());

    // 1. 检查当前是否设置了断点，没有就提示一下直接返回
    if (!data->bp_active && data->bp_addr == 0) {
        ui_toast_show(&data->toast, "当前未设置断点", UI_TOAST_WARN, now_tick);
        return;
    }

    // 2. 清除断点标志位与记录的地址
    data->bp_active = false;
    data->bp_addr = 0;

    // 3. ⚡ 核心：下发 SWD 指令，关闭 Cortex-M 的 FPB 通道 1 硬件比较器
    dap_hardware_bp(data);

    // 4. 恢复终端页底部 [断点] 按钮为默认浅灰色
    ui_btn_set_color(&data->bp, 0xEF7D, 0xA514, 0x4208);

    // 5. 提示用户并标记终端视口脏矩形重绘
    ui_toast_show(&data->toast, "断点已清除", UI_TOAST_INFO, now_tick);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);

}
static void on_dap_pin_config_changed(ui_pin_config_t *config,uint8_t gpio,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    uint32_t mask = gpio < 32u ? (UINT32_C(1) << gpio) : 0u;
    if (!data) return;
    if (config == &data->pin_swclk_cf) {
        /* SWDIO 不能选择当前已经占用的 SWCLK。 */
        ui_pin_config_set_excluded_mask(&data->pin_swdio_cf, mask);
    } else if (config == &data->pin_swdio_cf) {
        /* SWCLK 不能选择当前已经占用的 SWDIO。 */
        ui_pin_config_set_excluded_mask(&data->pin_swclk_cf, mask);
    }
    data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
}
static void on_dap_apply_click(ui_btn_t *btn,void *user_data){
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    uint8_t swdio_pin = ui_pin_config_get_selected(&data->pin_swdio_cf);
    uint8_t swclk_pin = ui_pin_config_get_selected(&data->pin_swclk_cf);
    if(swdio_pin==UINT8_MAX||swclk_pin==UINT8_MAX||swdio_pin==swclk_pin){
        data->dirty_rect=ui_rect_make(0,32,LCD_WIDTH,LCD_HEIGHT-32);
        return;
    }
    if (!fun_swd_config_set_pins(swclk_pin, swdio_pin)) {
        data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return;
    }

    const fun_swd_config_t *cfg = fun_swd_config_get();
    if (cfg) {
        data->swd = *cfg;
    }
    if(data->pio_swd.opened){
        fun_pio_swd_close(&data->pio_swd);
        data->pio_powered=false;
        data->target_detected=false;
    }
    ui_toast_show(&data->toast,"配置成功",UI_TOAST_SUCCESS,to_ms_since_boot(get_absolute_time()));
    // data->dirty_rect=ui_rect_make(0,32,LCD_WIDTH,LCD_HEIGHT-32);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
}
static void on_dap_switch_change(ui_switch_t *sw,bool state,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    g_dap_enabled=state;
    data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);

}
static void dap_scan_sym_files(dap_data_t *data, const char *path)
{
    if (!data) return;
    data->sym_item_count = 0;
    if (!data->fs_mounted) {
        if (f_mount(&data->fs, "0:", 1) != FR_OK) return;
        data->fs_mounted = true;
    }
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK) return;
    if (strcmp(path, "0:") != 0 && strcmp(path, "0:/") != 0) {
        snprintf(data->sym_items[0].name, sizeof(data->sym_items[0].name), "..");
        data->sym_items[0].is_dir = true;
        data->sym_items[0].size = 0;
        data->sym_item_count = 1;
    }
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (data->sym_item_count >= MAX_SYM_ITEMS) break;
        bool is_dir = (fno.fattrib & AM_DIR) ? true : false;

        if (is_dir) {
            if (fno.fname[0] == '.') continue;
            sym_file_item_t *it = &data->sym_items[data->sym_item_count++];
            snprintf(it->name, sizeof(it->name), "%s", fno.fname);
            it->is_dir = true;
            it->size = 0;
        } else if (strstr(fno.fname, ".sym") || strstr(fno.fname, ".SYM")) {
            sym_file_item_t *it = &data->sym_items[data->sym_item_count++];
            snprintf(it->name, sizeof(it->name), "%s", fno.fname);
            it->is_dir = false;
            it->size = (uint32_t)fno.fsize;
        }
    }
    f_closedir(&dir);

    data->sym_modal.sv_body.content_h = data->sym_item_count * 34 + 8;
}
static void on_sym_modal_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    int16_t item_h = 34; 
    for (uint8_t i = 0; i < data->sym_item_count; i++) {
        int16_t item_y = viewport->y + i * item_h - scroll_y;
        if (item_y + item_h < tile->rect.y || item_y > tile->rect.y + tile->rect.h) {
            continue; 
        }
        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, item_y + 2, viewport->w - 8, item_h - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xFFFF); 
            ui_draw_rect(tile, &item_rect, 0xCE59);   
        }
        sym_file_item_t *it = &data->sym_items[i];
        if (it->is_dir) {
            char dir_str[64];
            snprintf(dir_str, sizeof(dir_str), "[目录] %s", it->name);
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 7, dir_str, FONT_16, 0x03DF, 0xFFFF);
        } else {
            char file_str[64];
            snprintf(file_str, sizeof(file_str), "  %s", it->name);
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 7, file_str, FONT_16, 0x1A7B, 0xFFFF);
            char size_str[24];
            snprintf(size_str, sizeof(size_str), "%lu B", (unsigned long)it->size);
            ui_draw_string(tile, item_rect.x + item_rect.w - 70, item_rect.y + 7, size_str, FONT_16, 0x8410, 0xFFFF);
        }
    }
}
static bool on_sym_modal_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || !modal) return false;

    ui_rect_t vr = modal->sv_body.view_rect;
    bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) && ty >= vr.y && ty < (vr.y + vr.h));

    static bool    s_was_touching = false;
    static int16_t s_down_idx = -1;
    static int16_t s_down_y = 0;
    static int16_t s_down_scroll_y = 0;

    int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
    int idx = (in_viewport && rel_y >= 0) ? (rel_y / 34) : -1;

    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_down_idx = idx;
        s_down_y = ty;
        s_down_scroll_y = modal->sv_body.scroll_y;
        return false;
    }

    if (!is_touching && s_was_touching) {
        s_was_touching = false;
        int16_t dy = ty - s_down_y;
        int16_t dscroll = modal->sv_body.scroll_y - s_down_scroll_y;
        if (dy < -6 || dy > 6 || dscroll != 0 || modal->sv_body.is_dragged) {
            s_down_idx = -1;
            return false;
        }

        if (s_down_idx >= 0 && s_down_idx == idx && s_down_idx < data->sym_item_count) {
            sym_file_item_t *it = &data->sym_items[s_down_idx];
            s_down_idx = -1;
            if (it->is_dir) {
                if (strcmp(it->name, "..") == 0) {
                    char *last_slash = strrchr(data->modal_cur_path, '/');
                    if (last_slash && last_slash != data->modal_cur_path + 2) {
                        *last_slash = '\0';
                    } else {
                        strcpy(data->modal_cur_path, "0:/");
                    }
                } else {
                    char next_path[sizeof(data->modal_cur_path)];
                    size_t cur_len = strlen(data->modal_cur_path);
                    bool has_slash = (cur_len > 0 && data->modal_cur_path[cur_len - 1] == '/');

                    snprintf(next_path, sizeof(next_path), "%s%s%s",
                             data->modal_cur_path,
                             has_slash ? "" : "/",
                             it->name);
                    snprintf(data->modal_cur_path, sizeof(data->modal_cur_path), "%s", next_path);
                }
                dap_scan_sym_files(data, data->modal_cur_path);
            } else {
                if (strcmp(data->modal_cur_path, "0:/") == 0) {
                    snprintf(data->sym_file_path, sizeof(data->sym_file_path), "0:/%s", it->name);
                } else {
                    snprintf(data->sym_file_path, sizeof(data->sym_file_path), "%s/%s", data->modal_cur_path, it->name);
                }
                bool ok = sym_loader_load_from_sd(data->sym_file_path);
                
                ui_modal_hide(modal);
                if (ok) {
                    ui_toast_show(&data->toast, "符号表加载成功!", UI_TOAST_SUCCESS, now_tick);
                } else {
                    ui_toast_show(&data->toast, "符号文件加载失败", UI_TOAST_ERROR, now_tick);
                }
            }
            return true;
        }
        s_down_idx = -1;
    }
    return false;
}
static void on_choose_sym_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    if (data->modal_cur_path[0] == '\0') {
        strcpy(data->modal_cur_path, "0:/");
    }
    dap_scan_sym_files(data, data->modal_cur_path);
    ui_modal_show(&data->sym_modal, "选择符号文件 (.sym)", data);
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
static void on_probe_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    data->target_detected = false;
    data->pio_powered=false;
    data->dp_id=0;
    data->cpu_id=0;
    data->dbg_id=0;
    data->flash_size=0;
    data->pio_ack=0;
    data->pio_ctrl_ack=0;
    data->pio_ctrl_stat=0;
    data->probe_status = DAP_PROBE_IDLE;
    const fun_swd_config_t *cfg=fun_swd_config_get();
    if(!cfg){
        data->probe_status = DAP_PROBE_OPEN_FAILED;
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        return;
    }
    bool opened;
    if(data->pio_swd.opened){
        opened = fun_pio_swd_recover(&data->pio_swd);
    }else{
        opened= fun_pio_swd_open_auto(&data->pio_swd,pio_manager_get_default(),cfg->swclk_gpio,cfg->swdio_gpio,cfg->clock_hz);
    }
    if(!opened){
        data->probe_status = DAP_PROBE_OPEN_FAILED;
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        return;
    }
    data->target_detected=fun_pio_swd_read_dp_idcode(&data->pio_swd,&data->dp_id,&data->pio_ack);
    if(!data->target_detected){
        data->probe_status = DAP_PROBE_DP_FAILED;
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        return;
    }
    data->pio_powered=fun_pio_swd_power_up(&data->pio_swd);
    fun_pio_swd_read_dp_register(&data->pio_swd,0x04u,&data->pio_ctrl_stat,&data->pio_ctrl_ack);
    if(!data->pio_powered){
        data->probe_status = DAP_PROBE_POWER_FAILED;
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        return;
    }
    fun_pio_swd_read_memory32(&data->pio_swd,0xE000ED00u,&data->cpu_id);
    fun_pio_swd_read_memory32(&data->pio_swd,0xE0042000u,&data->dbg_id);
    data->probe_status = DAP_PROBE_SUCCESS;
    data->swd=*cfg;
    fun_pio_swd_close(&data->pio_swd);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
}

static void auto_default(dap_data_t *data)
{
    data->auto_step_enablde = false;
    data->auto_step.text = "自动";
    ui_btn_set_color(&data->auto_step, 0xEF7D, 0xA514, 0x4208);
}
static void on_run_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (!swd_open(data)) return;
    bp_step_out(data);
    data->is_halted=false;
    auto_default(data);
    fun_pio_swd_run_target(&data->pio_swd);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}

static void on_halt_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (!swd_open(data)) return;
    fun_pio_swd_halt_target(&data->pio_swd,100);
    data->is_halted =true;
    dap_read_all_regs(data);
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_step_click(ui_btn_t *btn,void *user_data)
{
     dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (!swd_open(data)) return;
    bool halted = false;
    fun_pio_swd_is_halted(&data->pio_swd,&halted);
    if(!halted)
        fun_pio_swd_halt_target(&data->pio_swd,100);
    uint32_t pc=data->core_regs[15] &~1u;
    bool is_at_bp =(data->bp_active&&pc==data->bp_addr);
    if(is_at_bp)
    fun_pio_swd_clear_breakpoint_ch(&data->pio_swd,1);
    if(fun_pio_swd_step_target(&data->pio_swd,100))
    {
        data->is_halted=true;
        dap_read_all_regs(data);
    }
    if(is_at_bp)
    fun_pio_swd_set_breakpoint_ch(&data->pio_swd,1,data->bp_addr);
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_reset_click(ui_btn_t *btn,void *user_data)
{
     dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (!swd_open(data)) return;
    uint32_t demcr = 0;
    fun_pio_swd_read_memory32(&data->pio_swd, 0xE000EDFCu, &demcr);
    fun_pio_swd_write_memory32(&data->pio_swd, 0xE000EDFCu, demcr | 1u);
    fun_pio_swd_reset_target(&data->pio_swd);
    fun_pio_swd_halt_target(&data->pio_swd,100); //复位暂停
    // fun_pio_swd_write_memory32(&data->pio_swd, 0xE000EDFCu, demcr & ~1u);
    data->is_halted=true;
    data->pio_swd.mem_select_valid=false;
    data->pio_swd.mem_csw_valid=false;
    data->pio_swd.mem_tar_valid=false;
    dap_hardware_bp(data);
    dap_read_all_regs(data);
    auto_default(data);
    ui_toast_show(&data->toast, "已复位至入口", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_step_over_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data||!swd_open(data)) return;
    bool halted =false;
    fun_pio_swd_is_halted(&data->pio_swd,&halted);
    if(!halted) fun_pio_swd_halt_target(&data->pio_swd,100);
     uint32_t pc=data->core_regs[15] &~1u;
    bool is_at_bp =(data->bp_active&&pc==data->bp_addr);
    if(is_at_bp)
    fun_pio_swd_clear_breakpoint_ch(&data->pio_swd,1);
    if(fun_pio_swd_step_over(&data->pio_swd,3000)){
        data->is_halted=true;
        dap_read_all_regs(data);
    }
    if(is_at_bp)
    fun_pio_swd_set_breakpoint_ch(&data->pio_swd,1,data->bp_addr);
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_step_out_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data||!swd_open(data)) return;
    bool halted =false;
    fun_pio_swd_is_halted(&data->pio_swd,&halted);
    if(!halted) fun_pio_swd_halt_target(&data->pio_swd,100);
     uint32_t pc=data->core_regs[15] &~1u;
    bool is_at_bp =(data->bp_active&&pc==data->bp_addr);
    if(is_at_bp)
    fun_pio_swd_clear_breakpoint_ch(&data->pio_swd,1);
    if(fun_pio_swd_step_out(&data->pio_swd,3000)){
        data->is_halted=true;
        dap_read_all_regs(data);
    }
    if(is_at_bp)
    fun_pio_swd_set_breakpoint_ch(&data->pio_swd,1,data->bp_addr);
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_view_toggle_click(ui_btn_t *btn, void *user_data)
{

    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    data->show_disasm = !data->show_disasm;
    btn->text = data->show_disasm ? "寄存器" : "汇编";
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_auto_step_click(ui_btn_t *btn, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data|| !swd_open(data)) return;
    if(!data->is_halted){
        fun_pio_swd_halt_target(&data->pio_swd,100);
        data->is_halted=true;
        dap_read_all_regs(data);
    }
    data->auto_step_enablde= !data->auto_step_enablde;
    data->auto_step_last_tick=to_ms_since_boot(get_absolute_time());
    if(data->auto_step_enablde){
        btn->text = "暂停";
        ui_btn_set_color(btn, 0xF800, 0x8000, 0xFFFF);
    }else{
        btn->text = "自动";
        ui_btn_set_color(btn, 0xEF7D, 0xA514, 0x4208);
    }
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void dap_status_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                    int16_t scroll_y, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    int16_t x = viewport->x + 16;
    int16_t y = viewport->y + 8 - scroll_y;
    char text[32];
    const fun_swd_config_t *cfg = fun_swd_config_get();
    if (cfg) data->swd = *cfg;
    const uint16_t color_title = 0x3186;
    const uint16_t color_text = 0x632C;
    const uint16_t color_muted = 0x8410;
    const uint16_t color_card = 0xF7BE;
    const uint16_t color_border = 0xCE59;
    const uint16_t color_ok = 0x2549;

    ui_draw_string(tile, x, y, "DAP状态", FONT_20,color_title, 0xFFFF);
    if(g_dap_enabled) 
    ui_draw_string(tile, x+80, y, "开启", FONT_20,0x07e0, 0xFFFF);
    else
    ui_draw_string(tile, x+80, y, "关闭", FONT_20,0x8410, 0xFFFF);
    const char *sub_text = "未探测";
    switch(data->probe_status){
        case DAP_PROBE_OPEN_FAILED:
        sub_text="PIO初始化失败";
            break;
        case DAP_PROBE_DP_FAILED:
        sub_text = "目标无响应";
            break;
        case DAP_PROBE_POWER_FAILED:
        sub_text = "调试电源失败";
            break;
        case DAP_PROBE_SUCCESS:
        sub_text = "探测成功";
            break;
        case DAP_PROBE_IDLE:
        default:
            break;
    }
    ui_draw_string(tile,x+190,y,sub_text,FONT_20,color_title,0xFFFF);


    ui_draw_fill_rect(tile, &data->status_card, color_card);
    ui_draw_rect(tile, &data->status_card, color_border);

    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16,"SWDIO:",FONT_20,color_title,0xFFFF);
    snprintf(text,sizeof(text),"GPIO-%d",data->swd.swdio_gpio);
    ui_draw_string(tile,data->status_card.x+16*5,data->status_card.y+16,text,FONT_20,color_title,0xFFFF);
    ui_draw_string(tile,data->status_card.x+160,data->status_card.y+16,"SWCLK:",FONT_20,color_title,0xFFFF);
    snprintf(text,sizeof(text),"GPIO-%d",data->swd.swclk_gpio);
    ui_draw_string(tile,data->status_card.x+16*4+160,data->status_card.y+16,text,FONT_20,color_title,0xFFFF);
    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*2,"CLOCK:",FONT_20,color_text,0xFFFF);
    if (data->swd.clock_hz / 1000000u) {
        snprintf(text, sizeof(text), "%luMHZ",
                 (unsigned long)(data->swd.clock_hz / 1000000u));
    } else {
        snprintf(text, sizeof(text), "%luKHZ",
                 (unsigned long)(data->swd.clock_hz / 1000u));
    }
    ui_draw_string(tile,data->status_card.x+16*5,data->status_card.y+16*2,text,FONT_24,color_title,0xFFFF);
    if(!data->target_detected)
    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*3+8,"点击探测开始探测芯片信息...",FONT_20,color_text,0xFFFF);
    else{
    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*3+8,"ID:",FONT_20,color_text,0xFFFF);
    snprintf(text, sizeof(text),"0x%08lX",(unsigned long)data->dp_id);
    ui_draw_string(tile,data->status_card.x+16+32,data->status_card.y+16*3+8,text,FONT_20,color_text,0xFFFF);
    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*4+8,"CPU ID:",FONT_20,color_text,0xFFFF);
    snprintf(text, sizeof(text),"0x%08lX",(unsigned long)data->cpu_id);
    ui_draw_string(tile,data->status_card.x+24+16*4,data->status_card.y+16*4+8,text,FONT_20,color_text,0xFFFF);
     ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*5+8,"DBG ID:",FONT_20,color_text,0xFFFF);
    snprintf(text, sizeof(text),"0x%08lX",(unsigned long)data->dbg_id);
    ui_draw_string(tile,data->status_card.x+24+16*4,data->status_card.y+16*5+8,text,FONT_24,color_text,0xFFFF);
    // ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*6+8,"FLASH SIZE:",FONT_20,color_text,0xFFFF);
    sub_text=dap_get_core_name(data->cpu_id,NULL);
    ui_draw_string(tile,data->status_card.x+16,data->status_card.y+16*6+10,sub_text,FONT_24,color_text,0xFFFF);
    }
    /* 探测按钮 */
    ui_btn_draw(tile, &data->pio_probe);
}

static bool dap_status_process_touch(dap_data_t *data, bool touching,
                                     int16_t x, int16_t y, uint32_t tick)
{
    if (ui_btn_process_touch(&data->pio_probe, touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->pio_probe));
        return true;
    }
    if (ui_scrollview_process_touch(&data->status_scroll, touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->status_scroll.view_rect);
        return true;
    }
    return false;
}

/* ============================================================================
 * Tab 1: 配置页 (Config)
 * ============================================================================ */

static void dap_config_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                    int16_t scroll_y, void *user_data)
{
     dap_data_t *data =(dap_data_t *)user_data;
    if (!data) 
        return;
    int16_t x = viewport->x + 16;
    int16_t y = viewport->y + 16 - scroll_y;
    ui_draw_string(tile, x, y, "DAP引脚配置", FONT_24, 0x4208, 0xFFFF);
   

    ui_pin_config_t swdio_view=data->pin_swdio_cf;
    swdio_view.rect=ui_scrollview_content_rect_to_screen(&data->config_scroll,data->pin_swdio_cf.rect);
    ui_pin_config_draw(tile,&swdio_view);
    ui_pin_config_t swclk_view=data->pin_swclk_cf;
    swclk_view.rect=ui_scrollview_content_rect_to_screen(&data->config_scroll,data->pin_swclk_cf.rect);
    ui_pin_config_draw(tile,&swclk_view);
    ui_btn_t apply_btn =data->btn_apply;
    apply_btn.rect=ui_scrollview_content_rect_to_screen(&data->config_scroll,data->btn_apply.rect);
    ui_btn_draw(tile,&apply_btn);
    ui_switch_t dap=data->dap_switch;
    dap.rect=ui_scrollview_content_rect_to_screen(&data->config_scroll,data->dap_switch.rect);
    ui_switch_draw(tile,&dap);
    ui_draw_string(tile,x,y+150,"DAP开关",FONT_24,0x4208,0xFFFF);
    //
    int16_t sym_y = y + 250;
    ui_draw_string(tile, x, sym_y, "符号映射配置", FONT_24, 0x4208, 0xFFFF);

    char path_buf[64];
     snprintf(path_buf, sizeof(path_buf), "当前: %d", sizeof(dap_data_t));
     ui_draw_string(tile,x+24*7,y,path_buf,FONT_16,0x4208, 0xFFFF);
    snprintf(path_buf, sizeof(path_buf), "当前: %s", data->sym_file_path[0] ? data->sym_file_path : "未设置");
    ui_draw_string(tile, x, sym_y + 32, path_buf, FONT_16, 0x1A7B, 0xFFFF);

    ui_btn_t sym_btn = data->choose_sym;
    sym_btn.rect = ui_scrollview_content_rect_to_screen(&data->config_scroll, data->choose_sym.rect);
    ui_btn_draw(tile, &sym_btn);

}
static void dap_scan_var_items(dap_data_t *data)
{
    if (!data) return;
    data->var_view_count = 0;
    uint16_t total = sym_loader_get_var_count();

    // A. 子目录下 (如 "GPIOC")
    if (data->var_cur_dir[0] != '\0') {
        var_view_item_t *back = &data->var_view_items[data->var_view_count++];
        back->sym_idx = 0xFFFF; // 0xFFFF 代表返回上级
        back->is_dir  = 1;

        size_t dir_len = strlen(data->var_cur_dir);
        for (uint16_t i = 0; i < total; i++) {
            if (data->var_view_count >= 64) break;
            const sym_var_entry_t *v = sym_loader_get_var(i);
            if (!v) continue;
            const char *name = sym_loader_get_var_name(v);

            if (strncmp(name, data->var_cur_dir, dir_len) == 0 && name[dir_len] == '/') {
                var_view_item_t *it = &data->var_view_items[data->var_view_count++];
                it->sym_idx = i;
                it->is_dir  = 0;
            }
        }
    }
    // B. 根目录 (普通全局变量 + 外设文件夹)
    else {
        // 1. 普通全局变量 (名字中不含 '/')
        for (uint16_t i = 0; i < total; i++) {
            if (data->var_view_count >= 64) break;
            const sym_var_entry_t *v = sym_loader_get_var(i);
            if (!v) continue;
            const char *name = sym_loader_get_var_name(v);

            if (strchr(name, '/') == NULL) {
                var_view_item_t *it = &data->var_view_items[data->var_view_count++];
                it->sym_idx = i;
                it->is_dir  = 0;
            }
        }

        // 2. 提取外设文件夹 (如 GPIOA, GPIOB, TIM2...)
        for (uint16_t i = 0; i < total; i++) {
            if (data->var_view_count >= 64) break;
            const sym_var_entry_t *v = sym_loader_get_var(i);
            if (!v) continue;
            const char *name = sym_loader_get_var_name(v);
            const char *slash = strchr(name, '/');

            if (slash != NULL) {
                size_t p_len = slash - name;
                bool exists = false;
                for (uint16_t j = 0; j < data->var_view_count; j++) {
                    if (data->var_view_items[j].is_dir && data->var_view_items[j].sym_idx != 0xFFFF) {
                        const sym_var_entry_t *ev = sym_loader_get_var(data->var_view_items[j].sym_idx);
                        if (ev) {
                            const char *ev_name = sym_loader_get_var_name(ev);
                            const char *ev_slash = strchr(ev_name, '/');
                            size_t ev_len = ev_slash ? (size_t)(ev_slash - ev_name) : strlen(ev_name);
                            if (p_len == ev_len && strncmp(ev_name, name, p_len) == 0) {
                                exists = true;
                                break;
                            }
                        }
                    }
                }
                if (!exists) {
                    var_view_item_t *it = &data->var_view_items[data->var_view_count++];
                    it->sym_idx = i;
                    it->is_dir  = 1;
                }
            }
        }
    }

    // 更新内容高度并归位滚动条
    data->var_modal.sv_body.content_h = data->var_view_count * 36 + 8;
    data->var_modal.sv_body.scroll_y = 0;
}
// static void on_var_modal_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
// {
//     dap_data_t *data = (dap_data_t *)user_data;
//     if (!data) return;

//     uint16_t total = sym_loader_get_var_count();
//     int16_t item_h = 36;

//     for (uint16_t i = 0; i < total; i++) {
//         int16_t item_y = viewport->y + i * item_h - scroll_y;
//         if (item_y + item_h < tile->rect.y || item_y > tile->rect.y + tile->rect.h) continue;

//         const sym_var_entry_t *v = sym_loader_get_var(i);
//         if (!v) continue;

//         // 检查该变量是否已在监控列表中
//         bool is_added = false;
//         for (uint8_t w = 0; w < data->watch_count; w++) {
//             if (data->watch_items[w].valid && data->watch_items[w].addr == v->addr) {
//                 is_added = true;
//                 break;
//             }
//         }

//         ui_rect_t item_rect = ui_rect_make(viewport->x + 4, item_y + 2, viewport->w - 8, item_h - 4);
//         ui_rect_t overlap;
//         if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
//             // 已添加的条目使用柔和浅青色背景高亮
//             ui_draw_fill_rect(tile, &overlap, is_added ? 0xE79F : 0xFFFF);
//             ui_draw_rect(tile, &item_rect, is_added ? 0x03DF : 0xCE59);
//         }

//         const char *name = sym_loader_get_var_name(v);
//         ui_draw_string(tile, item_rect.x + 8, item_rect.y + 8, name, FONT_16, is_added ? 0x0275 : 0x1A7B, is_added ? 0xE79F : 0xFFFF);

//         // 显示变量地址与大小
//         char info_str[32];
//         snprintf(info_str, sizeof(info_str), "0x%08lX (%uB)", (unsigned long)v->addr, v->size);
//         ui_draw_string(tile, item_rect.x + item_rect.w - 130, item_rect.y + 8, info_str, FONT_16, 0x8410, is_added ? 0xE79F : 0xFFFF);
//     }
// }
static void on_var_modal_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    int16_t item_h = 36;
    for (uint16_t i = 0; i < data->var_view_count; i++) {
        int16_t item_y = viewport->y + i * item_h - scroll_y;
        if (item_y + item_h < tile->rect.y || item_y > tile->rect.y + tile->rect.h) continue;

        var_view_item_t *item = &data->var_view_items[i];
        const sym_var_entry_t *v = (item->sym_idx != 0xFFFF) ? sym_loader_get_var(item->sym_idx) : NULL;

        bool is_added = false;
        if (!item->is_dir && v) {
            for (uint8_t w = 0; w < data->watch_count; w++) {
                if (data->watch_items[w].valid && data->watch_items[w].addr == v->addr) {
                    is_added = true;
                    break;
                }
            }
        }

        ui_rect_t item_rect = ui_rect_make(viewport->x + 4, item_y + 2, viewport->w - 8, item_h - 4);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &item_rect, &overlap)) {
            uint16_t bg_col = is_added ? 0xE79F : (item->is_dir ? 0xF7BE : 0xFFFF);
            ui_draw_fill_rect(tile, &overlap, bg_col);
            ui_draw_rect(tile, &item_rect, is_added ? 0x03DF : 0xCE59);
        }

        if (item->is_dir) {
            char dir_str[32];
            if (item->sym_idx == 0xFFFF) {
                snprintf(dir_str, sizeof(dir_str), "[返回上级] ..");
            } else if (v) {
                const char *full = sym_loader_get_var_name(v);
                const char *slash = strchr(full, '/');
                size_t p_len = slash ? (size_t)(slash - full) : strlen(full);
                if (p_len > 20) p_len = 20;
                snprintf(dir_str, sizeof(dir_str), "[外设] %.*s", (int)p_len, full);
            }
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 8, dir_str, FONT_16, 0x03DF, 0xF7BE);
        } else if (v) {
            const char *full = sym_loader_get_var_name(v);
            const char *slash = strchr(full, '/');
            const char *disp = slash ? (slash + 1) : full;

            uint16_t text_col = is_added ? 0x0275 : 0x1A7B;
            uint16_t bg_col   = is_added ? 0xE79F : 0xFFFF;
            ui_draw_string(tile, item_rect.x + 8, item_rect.y + 8, disp, FONT_16, text_col, bg_col);

            char info_str[32];
            snprintf(info_str, sizeof(info_str), "0x%08lX (%uB)", (unsigned long)v->addr, v->size ? v->size : 4);
            ui_draw_string(tile, item_rect.x + item_rect.w - 130, item_rect.y + 8, info_str, FONT_16, 0x8410, bg_col);
        }
    }
}

// // 2. 变量选择弹窗触摸判定 (点选即加入/移出)
// static bool on_var_modal_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data)
// {
//     dap_data_t *data = (dap_data_t *)user_data;
//     if (!data || !modal) return false;

//     ui_rect_t vr = modal->sv_body.view_rect;
//     bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) && ty >= vr.y && ty < (vr.y + vr.h));

//     static bool    s_was_touching = false;
//     static int16_t s_down_idx = -1;
//     static int16_t s_down_y = 0;
//     static int16_t s_down_scroll_y = 0;

//     int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
//     int idx = (in_viewport && rel_y >= 0) ? (rel_y / 36) : -1;

//     if (is_touching && !s_was_touching) {
//         s_was_touching = true;
//         s_down_idx = idx;
//         s_down_y = ty;
//         s_down_scroll_y = modal->sv_body.scroll_y;
//         return false;
//     }

//     if (!is_touching && s_was_touching) {
//         s_was_touching = false;
//         int16_t dy = ty - s_down_y;
//         int16_t dscroll = modal->sv_body.scroll_y - s_down_scroll_y;
//         if (dy < -6 || dy > 6 || dscroll != 0 || modal->sv_body.is_dragged) {
//             s_down_idx = -1;
//             return false;
//         }

//         uint16_t total = sym_loader_get_var_count();
//         if (s_down_idx >= 0 && s_down_idx == idx && s_down_idx < total) {
//             const sym_var_entry_t *v = sym_loader_get_var(s_down_idx);
//             s_down_idx = -1;
//             if (v) {
//                 // 判断是否已在列表中，已存在则移除
//                 int found = -1;
//                 for (uint8_t w = 0; w < data->watch_count; w++) {
//                     if (data->watch_items[w].valid && data->watch_items[w].addr == v->addr) {
//                         found = w;
//                         break;
//                     }
//                 }

//                 if (found >= 0) {
//                     for (uint8_t w = found; w < data->watch_count - 1; w++) {
//                         data->watch_items[w] = data->watch_items[w + 1];
//                     }
//                     data->watch_count--;
//                     ui_toast_show(&data->toast, "已移出监控", UI_TOAST_INFO, now_tick);
//                 } else {
//                     if (data->watch_count < MAX_WATCH_ITEMS) {
//                         dap_watch_item_t *it = &data->watch_items[data->watch_count++];
//                         it->addr = v->addr;
//                         it->size = v->size ? v->size : 4;
//                         it->val = 0;
//                         it->last_val = 0;
//                         it->change_tick = 0;
//                         it->valid = true;
//                         snprintf(it->name, sizeof(it->name), "%s", sym_loader_get_var_name(v));
//                         ui_toast_show(&data->toast, "已加入监控", UI_TOAST_SUCCESS, now_tick);
//                     } else {
//                         ui_toast_show(&data->toast, "监控槽位已满(最多6个)", UI_TOAST_WARN, now_tick);
//                     }
//                 }
//                 ui_modal_hide(modal);
//                 data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
//             }
//             return true;
//         }
//         s_down_idx = -1;
//     }
//     return false;
// }
static bool on_var_modal_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || !modal) return false;

    ui_rect_t vr = modal->sv_body.view_rect;
    bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) && ty >= vr.y && ty < (vr.y + vr.h));

    static bool    s_was_touching = false;
    static int16_t s_down_idx = -1;
    static int16_t s_down_y = 0;
    static int16_t s_down_scroll_y = 0;

    int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
    int idx = (in_viewport && rel_y >= 0) ? (rel_y / 36) : -1;

    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_down_idx = idx;
        s_down_y = ty;
        s_down_scroll_y = modal->sv_body.scroll_y;
        return false;
    }

    if (!is_touching && s_was_touching) {
        s_was_touching = false;
        int16_t dy = ty - s_down_y;
        int16_t dscroll = modal->sv_body.scroll_y - s_down_scroll_y;
        if (dy < -6 || dy > 6 || dscroll != 0 || modal->sv_body.is_dragged) {
            s_down_idx = -1;
            return false;
        }

        if (s_down_idx >= 0 && s_down_idx == idx && s_down_idx < data->var_view_count) {
            var_view_item_t *item = &data->var_view_items[s_down_idx];
            s_down_idx = -1;

            // A. 点击文件夹
            if (item->is_dir) {
                if (item->sym_idx == 0xFFFF) {
                    data->var_cur_dir[0] = '\0'; // 返回根目录
                } else {
                    const sym_var_entry_t *v = sym_loader_get_var(item->sym_idx);
                    if (v) {
                        const char *full = sym_loader_get_var_name(v);
                        const char *slash = strchr(full, '/');
                        size_t p_len = slash ? (size_t)(slash - full) : strlen(full);
                        if (p_len >= sizeof(data->var_cur_dir)) p_len = sizeof(data->var_cur_dir) - 1;
                        strncpy(data->var_cur_dir, full, p_len);
                        data->var_cur_dir[p_len] = '\0';
                    }
                }
                dap_scan_var_items(data);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return true;
            }

            // B. 点击叶子变量/寄存器
            const sym_var_entry_t *v = sym_loader_get_var(item->sym_idx);
            if (v) {
                int found = -1;
                for (uint8_t w = 0; w < data->watch_count; w++) {
                    if (data->watch_items[w].valid && data->watch_items[w].addr == v->addr) {
                        found = w;
                        break;
                    }
                }

                if (found >= 0) {
                    for (uint8_t w = found; w < data->watch_count - 1; w++) {
                        data->watch_items[w] = data->watch_items[w + 1];
                    }
                    data->watch_count--;
                    ui_toast_show(&data->toast, "已移出监控", UI_TOAST_INFO, now_tick);
                } else {
                    if (data->watch_count < MAX_WATCH_ITEMS) {
                        dap_watch_item_t *it = &data->watch_items[data->watch_count++];
                        it->addr = v->addr;
                        it->size = v->size ? v->size : 4;
                        it->val = 0;
                        it->last_val = 0;
                        it->change_tick = 0;
                        it->valid = true;

                        const char *full = sym_loader_get_var_name(v);
                        const char *slash = strchr(full, '/');
                        if (slash) {
                            snprintf(it->name, sizeof(it->name), "%.*s_%s", (int)(slash - full), full, slash + 1);
                        } else {
                            snprintf(it->name, sizeof(it->name), "%s", full);
                        }
                        ui_toast_show(&data->toast, "已加入监控", UI_TOAST_SUCCESS, now_tick);
                    } else {
                        ui_toast_show(&data->toast, "监控槽位已满(最多6个)", UI_TOAST_WARN, now_tick);
                    }
                }
                ui_modal_hide(modal);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
            }
            return true;
        }
        s_down_idx = -1;
    }
    return false;
}
// static void on_add_watch_click(ui_btn_t *btn,void *user_data)
// {
//     dap_data_t *data= (dap_data_t *) user_data;
//     if(!data) return;
//     uint16_t cnt = sym_loader_get_var_count();
//     if (cnt == 0) {
//         ui_toast_show(&data->toast, "未发现变量符号，请检查.sym!", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time()));
//         return;
//     }

//     data->var_modal.sv_body.content_h = cnt * 36 + 8;
//     ui_modal_show(&data->var_modal, "添加监控变量", data);
//     data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
// }
static void on_add_watch_click(ui_btn_t *btn, void *user_data)
{
    (void)btn;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    // 1. 检查符号表是否加载
    uint16_t cnt = sym_loader_get_var_count();
    if (cnt == 0) {
        ui_toast_show(&data->toast, "未发现变量符号，请检查.sym!", UI_TOAST_WARN, to_ms_since_boot(get_absolute_time()));
        return;
    }

    // ⚡ 核心修复：打开前重置回根目录并执行扫描，把条目装填进 var_view_items！
    data->var_cur_dir[0] = '\0';
    dap_scan_var_items(data);

    // 2. 打开弹窗
    ui_modal_show(&data->var_modal, "添加监控变量", data);
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
static void on_clear_watch_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data= (dap_data_t *) user_data;
    if(!data) return;
    data->watch_count = 0;
    ui_toast_show(&data->toast, "已清空监控列表", UI_TOAST_INFO, to_ms_since_boot(get_absolute_time()));
    data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
}
// ⚡ 核心：无感安全改写目标单片机变量 (支持 1B / 2B / 4B 自动对齐与读-改-写)
static bool dap_watch_write_var(dap_data_t *data, dap_watch_item_t *item, uint32_t new_val)
{
    if (!data || !item || !swd_open(data)) return false;

    uint32_t aligned_addr = item->addr & ~3u;
    uint32_t shift = (item->addr & 3u) * 8u;

    if (item->size >= 4) {
        // 4 字节直接对齐写入
        if (fun_pio_swd_write_memory32(&data->pio_swd, aligned_addr, new_val)) {
            item->val = new_val;
            item->last_val = new_val;
            return true;
        }
    } else {
        // 1 字节 / 2 字节采用 Read-Modify-Write，严格保护同一 Word 内的相邻变量
        uint32_t raw_word = 0;
        if (!fun_pio_swd_read_memory32(&data->pio_swd, aligned_addr, &raw_word)) {
            return false;
        }

        uint32_t mask = (item->size == 1) ? (0xFFu << shift) : (0xFFFFu << shift);
        uint32_t insert = (new_val & ((item->size == 1) ? 0xFFu : 0xFFFFu)) << shift;

        raw_word = (raw_word & ~mask) | insert;

        if (fun_pio_swd_write_memory32(&data->pio_swd, aligned_addr, raw_word)) {
            item->val = new_val;
            item->last_val = new_val;
            return true;
        }
    }
    return false;
}
static void on_var_kb_submit(ui_keyboard_t *kb,const char *text,void *user_data)
{
    dap_data_t *data =(dap_data_t *)user_data;
    if(!data||data->watch_idx<0||data->watch_idx>=data->watch_count) return;
    dap_watch_item_t *item=&data->watch_items[data->watch_idx];
    if(!item->valid) return;
    uint32_t parsed_val=0;
    if(text[0]=='0'&&(text[1]=='x'||text[1]=='X'))
    parsed_val=(uint32_t)strtoul(text,NULL,16);
    else
    parsed_val = (uint32_t)strtoul(text,NULL,10);
    if(item->size==1) parsed_val &=0xFFu;
    else if(item->size==2) parsed_val &=0xFFFFu;
    dap_watch_write_var(data,item,parsed_val);
    ui_keyboard_close(kb);
    data->watch_idx=-1;
    data->dirty_rect = ui_rect_make(0,0,LCD_WIDTH,LCD_HEIGHT);

}


static void on_dap_kb_submit(ui_keyboard_t *kb, const char *text, void *user_data) {
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    if (data->tabview.active_tab_idx == 4) {
        if (text && text[0] != '\0' && !s_rtt_tx_pending) {
            strncpy(s_rtt_tx_buf, text, sizeof(s_rtt_tx_buf) - 1);
            s_rtt_tx_buf[sizeof(s_rtt_tx_buf) - 1] = '\0';
            s_rtt_tx_pending = true;
        }
        ui_input_clear(&data->input);
        ui_keyboard_close(kb);
        data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    } else {
        on_var_kb_submit(kb, text, user_data);
    }
}

static bool dap_config_process_touch(dap_data_t *data, bool touching, int16_t x, int16_t y, uint32_t tick)
{
    if (!data) return false;

    /* 弹窗打开时优先交给弹窗 */
    if (data->pin_swdio_cf.modal.is_open) {
        bool handled = ui_pin_config_process_modal_touch(&data->pin_swdio_cf, touching, x, y, tick);
        if (handled) data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return handled;
    }

    if (data->pin_swclk_cf.modal.is_open) {
        bool handled = ui_pin_config_process_modal_touch(&data->pin_swclk_cf, touching, x, y, tick);
        if (handled) data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return handled;
    }

    /* ⚡ 1. 优先调用滚动视口处理手势 (解决配置页不滑动的根本原因) */
    bool scroll_handled = ui_scrollview_process_touch(&data->config_scroll, touching, x, y, tick);
    if (scroll_handled) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
    }

    /* ⚡ 2. 核心保护：若用户正在上下滑动视口，绝不触发下方按钮与开关 */
    if (data->config_scroll.is_dragged) {
        return true;
    }

    /* 3. 子控件使用包含滚动偏移的内容坐标 */
    int16_t content_x = (int16_t)(x - data->config_scroll.view_rect.x);
    int16_t content_y = (int16_t)(y - data->config_scroll.view_rect.y + data->config_scroll.scroll_y);

    if (ui_pin_config_process_touch(&data->pin_swdio_cf, touching, content_x, content_y, tick)) {
        data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return true;
    }

    if (ui_pin_config_process_touch(&data->pin_swclk_cf, touching, content_x, content_y, tick)) {
        data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
        return true;
    }

    if (ui_btn_process_touch(&data->btn_apply, touching, content_x, content_y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
        return true;
    }

    if (ui_switch_process_touch(&data->dap_switch, touching, content_x, content_y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
        return true;
    }

    if (ui_btn_process_touch(&data->choose_sym, touching, content_x, content_y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
        return true;
    }

    return scroll_handled;
}
/* ============================================================================
 * Tab 2: 终端页 (Terminal)
 * ============================================================================ */

static void dap_term_draw_content(ui_tile_t *tile, const ui_rect_t *viewport,
                                  int16_t scroll_y, void *user_data)
{
    dap_data_t *data =(dap_data_t *)user_data;
    if (!data) 
        return;
    int16_t x = viewport->x + 16;
    int16_t y = viewport->y + 16 - scroll_y;
    ui_btn_t run =data->run;
    run.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->run.rect);
    ui_btn_draw(tile,&run);
    ui_btn_t halt =data->halt;
    halt.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->halt.rect);
    ui_btn_draw(tile,&halt);
    ui_btn_t step =data->step;
    step.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->step.rect);
    ui_btn_draw(tile,&step);
    ui_btn_t reset =data->reset;
    reset.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->reset.rect);
    ui_btn_draw(tile,&reset);
    ui_rect_t card = ui_scrollview_content_rect_to_screen(&data->term_scroll,data->reg_card);
    ui_rect_t overlap;
    if(ui_rect_intersect(&tile->rect, &card, &overlap))
    {
        ui_draw_fill_rect(tile,&overlap,0xEF7D);
        ui_draw_rect(tile, &card, 0xCE59);
    }
    ui_btn_t step_over =data->step_over;
    step_over.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->step_over.rect);
    ui_btn_draw(tile,&step_over);
    ui_btn_t step_out =data->step_out;
    step_out.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->step_out.rect);
    ui_btn_draw(tile,&step_out);
    ui_btn_t view =data->view_toggle;
    view.rect=ui_scrollview_content_rect_to_screen(&data->term_scroll,data->view_toggle.rect);
    ui_btn_draw(tile,&view);
    ui_btn_t auto_b = data->auto_step;
    auto_b.rect = ui_scrollview_content_rect_to_screen(&data->term_scroll, data->auto_step.rect);
    ui_btn_draw(tile,&auto_b);
    ui_btn_t bp = data->bp;
    bp.rect = ui_scrollview_content_rect_to_screen(&data->term_scroll, data->bp.rect);
    ui_btn_draw(tile, &bp);
    ui_btn_t clear_bp = data->clear_bp;
    clear_bp.rect = ui_scrollview_content_rect_to_screen(&data->term_scroll, data->clear_bp.rect);
    ui_btn_draw(tile, &clear_bp);
    if(!data->show_disasm){
    static const char *reg_names[17] = {
        "R0 ", "R1 ", "R2 ", "R3 ", "R4 ", "R5 ", "R6 ", "R7 ",
        "R8 ", "R9 ", "R10", "R11", "R12", "SP ", "LR ", "PC ", "PSR"
    };
    char str[32];
    int16_t card_x =card.x+8;
    int16_t card_y =card.y+8;
    for(int i=0;i<17;i++){
    int col =i/6;
    int row =i%6;
    int16_t rx=card_x+col*106;
    int16_t ry=card_y+row*30;
    if(data->regs_valid)
        snprintf(str,sizeof(str),"%s:%08lX",reg_names[i],(unsigned long)data->core_regs[i]);
    else
        snprintf(str,sizeof(str),"%s:--------",reg_names[i]);
    uint16_t text_color = 0x1A7B;
    if(!data->regs_valid) text_color =0x8410;
    else if(i==15) text_color = 0xF800;
    else if(i==14) text_color = 0x0275;
    else if(i==13) text_color =0x03E0;
    ui_draw_string(tile,rx,ry,str,FONT_16,text_color,0xEF7D);
    }
    int16_t state_x =card_x+2*106;
    int16_t state_y = card_y+5*30;
    if(data->is_halted)
        ui_draw_string(tile,state_x,state_y,"状态:[HALT]",FONT_16,0xF800, 0xEF7D);
    else
        ui_draw_string(tile,state_x,state_y,"状态:[RUN ]",FONT_16,0x05E0, 0xEF7D);
    }else{
        uint32_t pc = data->core_regs[15] & ~1u;
        uint32_t offset = 0;
        const char *fn_name = sym_loader_find_func(pc, &offset);
        char sym_title[48];
        if (fn_name) {
            snprintf(sym_title, sizeof(sym_title), " <%s + 0x%lX>", fn_name, (unsigned long)offset);
        } else {
            snprintf(sym_title, sizeof(sym_title), " <未知函数 [0x%08lX]>", (unsigned long)pc);
        }
        ui_draw_string(tile, card.x + 6, card.y + 3, sym_title, FONT_16, 0x8410, 0xEF7D);
        uint32_t start_pc = pc;
        if (fn_name && offset > 0 && offset <= 120) {
            uint32_t fn_base = pc - offset;
            uint32_t trace_buf[32];
            uint32_t words_to_read = (offset + 8) / 4;
            if (words_to_read > 32) words_to_read = 32;
            if (fun_pio_swd_read_memory_block32(&data->pio_swd, fn_base & ~3u, trace_buf, words_to_read)) {
                const uint16_t *t_stream = (const uint16_t *)trace_buf;
                uint32_t cur = fn_base;
                uint32_t prev_history[4] = {0};
                int hist_count = 0;
                while (cur < pc) {
                    prev_history[0] = prev_history[1];
                    prev_history[1] = prev_history[2];
                    prev_history[2] = cur;
                    if (hist_count < 3) hist_count++;
                    int s_idx = (cur - (fn_base & ~3u)) / 2;
                    disasm_line_t tmp;
                    uint8_t len = thumb_disasm_single(cur, t_stream[s_idx], t_stream[s_idx + 1], &tmp);
                    cur += len;
                }
                if (cur == pc) {
                    if (hist_count >= 2) {
                        start_pc = prev_history[1]; 
                    } else if (hist_count == 1) {
                        start_pc = prev_history[2]; 
                    }
                }
            }
        }
        uint32_t code_buf[12];
        if (data->pio_swd.opened && fun_pio_swd_read_memory_block32(&data->pio_swd, start_pc & ~3u, code_buf, 12)) {
            const uint16_t *stream = (const uint16_t *)code_buf;
            int stream_idx = (start_pc & 2u) ? 1 : 0;
            uint32_t cur_addr = start_pc;
            for (int line = 0; line < 6; line++) {
                disasm_line_t insn;
                uint16_t hw1 = stream[stream_idx];
                uint16_t hw2 = stream[stream_idx + 1];
                uint8_t len = thumb_disasm_single(cur_addr, hw1, hw2, &insn);
                char dis_str[64];
                int16_t line_y = card.y + 28 + line * 26;
                if (cur_addr == pc) {
                    ui_rect_t hl_bar = ui_rect_make(card.x + 2, line_y - 2, card.w - 4, 22);
                    ui_draw_fill_rect(tile, &hl_bar, 0xD69A);

                    snprintf(dis_str, sizeof(dis_str), "-> %08lX  %-18s", (unsigned long)cur_addr, insn.text);
                    ui_draw_string(tile, card.x + 6, line_y, dis_str, FONT_16, 0xF800, 0xD69A); 
                } else {
                    snprintf(dis_str, sizeof(dis_str), "   %08lX  %-18s", (unsigned long)cur_addr, insn.text);
                    ui_draw_string(tile, card.x + 6, line_y, dis_str, FONT_16, 0x1A7B, 0xEF7D); 
                }
                stream_idx += (len / 2);
                cur_addr += len;
                if (stream_idx >= 22) break;
            }
        }
    }
}

static bool dap_term_process_touch(dap_data_t *data, bool touching,
                                   int16_t x, int16_t y, uint32_t tick)
{
    int16_t content_x = (int16_t)(x - data->term_scroll.view_rect.x);
    int16_t content_y = (int16_t)(y - data->term_scroll.view_rect.y +
                                   data->term_scroll.scroll_y);
    if (ui_scrollview_process_touch(&data->term_scroll, touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
     if (ui_btn_process_touch(&data->run,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
    if (ui_btn_process_touch(&data->halt,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
        if (ui_btn_process_touch(&data->step,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
        if (ui_btn_process_touch(&data->reset,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
        if (ui_btn_process_touch(&data->step_over,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
      if (ui_btn_process_touch(&data->step_out,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
    if (ui_btn_process_touch(&data->view_toggle,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
    if (ui_btn_process_touch(&data->auto_step,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
     if (ui_btn_process_touch(&data->bp,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
         if (ui_btn_process_touch(&data->clear_bp,touching,content_x,content_y,tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        return true;
    }
    return false;
}
static inline ui_rect_t dap_watch_get_card_screen_rect(dap_data_t *data, uint8_t idx)
{
    int16_t x = data->watch_scroll.view_rect.x + 8;
    int16_t y = data->watch_scroll.view_rect.y + 8 + 38 + idx * (50 + 6) - data->watch_scroll.scroll_y;
    int16_t w = data->watch_scroll.view_rect.w - 16;
    return ui_rect_make(x, y, w, 50);
}

// ⚡ 方案 A: 32位可视化点阵弹窗绘制
static void on_bit_modal_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    (void)scroll_y;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || data->watch_idx < 0 || data->watch_idx >= data->watch_count) return;

    dap_watch_item_t *item = &data->watch_items[data->watch_idx];
    if (!item->valid) return;

    int16_t x = viewport->x + 6;
    int16_t y = viewport->y + 2;

    // 1. 顶部信息栏 (显示名称、物理地址与当前十六进制/十进制值)
    char head_str[64];
    snprintf(head_str, sizeof(head_str), "%s [0x%08lX]", item->name, (unsigned long)item->addr);
    ui_draw_string(tile, x, y, head_str, FONT_16, 0x03DF, 0xCE79);

    char val_str[48];
    snprintf(val_str, sizeof(val_str), "HEX: 0x%08lX  DEC: %lu", (unsigned long)item->val, (unsigned long)item->val);
    ui_draw_string(tile, x, y + 20, val_str, FONT_16, 0x03E0, 0xCE79);

    // 2. 绘制点阵格子 (8位/行，每格 42x26 像素)
    uint8_t total_bits = (item->size >= 4) ? 32 : (item->size * 8);
    uint8_t rows = total_bits / 8;
    int16_t grid_start_y = y + 42;

    for (uint8_t r = 0; r < rows; r++) {
        uint8_t base_bit = (rows - 1 - r) * 8; // 从高字节向低字节排布 (31..24, 23..16, 15..8, 7..0)
        for (int b = 7; b >= 0; b--) {
            uint8_t bit_idx = base_bit + b;
            int16_t bx = x + (7 - b) * 47;
            int16_t by = grid_start_y + r * 30;

            ui_rect_t b_rect = ui_rect_make(bx, by, 44, 26);
            ui_rect_t overlap;
            if (!ui_rect_intersect(&tile->rect, &b_rect, &overlap)) continue;

            bool is_1 = (item->val >> bit_idx) & 1u;
            uint16_t bg_col = is_1 ? 0x05E0 : 0xF7BE;     // 置1亮绿底，置0浅灰底
            uint16_t bd_col = is_1 ? 0x03E0 : 0xCE59;     // 边框色
            uint16_t tx_col = is_1 ? 0xFFFF : 0x4208;     // 文字色

            ui_draw_fill_rect(tile, &overlap, bg_col);
            ui_draw_rect(tile, &b_rect, bd_col);

            char bit_txt[12];
            snprintf(bit_txt, sizeof(bit_txt), "%u:%u", bit_idx, is_1 ? 1 : 0);
            ui_draw_string(tile, b_rect.x + (bit_idx >= 10 ? 4 : 8), b_rect.y + 5, bit_txt, FONT_16, tx_col, bg_col);
        }
    }

    // 3. 底部快捷控制按钮 (全置0、全置1、软键盘、关闭)
    int16_t btn_y = y + 162;
    ui_rect_t btns[4] = {
        ui_rect_make(x,       btn_y, 82, 30),
        ui_rect_make(x + 88,  btn_y, 82, 30),
        ui_rect_make(x + 176, btn_y, 82, 30),
        ui_rect_make(x + 288, btn_y, 82, 30)
    };
    const char *btn_labels[4] = { "全部清0", "全部置1", "键盘输入", "关闭" };
    uint16_t btn_colors[4] = { 0xCE79, 0xCE79, 0x03DF, 0xF800 };

    for (int k = 0; k < 4; k++) {
        ui_rect_t ov;
        if (ui_rect_intersect(&tile->rect, &btns[k], &ov)) {
            ui_draw_fill_rect(tile, &ov, btn_colors[k]);
            ui_draw_rect(tile, &btns[k], 0xCE59);
            ui_draw_string(tile, btns[k].x + 8, btns[k].y + 7, btn_labels[k], FONT_16, 0xFFFF, btn_colors[k]);
        }
    }
}

// ⚡ 方案 A: 32位可视化点阵触摸交互 (点格即改，单片机立刻响应)
static bool on_bit_modal_touch_content(ui_modal_t *modal, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick, void *user_data)
{
    (void)now_tick;
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data || data->watch_idx < 0 || data->watch_idx >= data->watch_count) return false;

    dap_watch_item_t *item = &data->watch_items[data->watch_idx];
    if (!item->valid) return false;

    static bool s_was_touching = false;
    static int16_t s_dx = 0, s_dy = 0;

    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_dx = tx; s_dy = ty;
        return false;
    }

    if (!is_touching && s_was_touching) {
        s_was_touching = false;
        if (abs(tx - s_dx) > 6 || abs(ty - s_dy) > 6) return false;

        ui_rect_t vr = modal->sv_body.view_rect;
        int16_t rel_x = tx - vr.x;
        int16_t rel_y = ty - vr.y;

        int16_t start_x = 6;
        int16_t grid_y  = 44;
        uint8_t total_bits = (item->size >= 4) ? 32 : (item->size * 8);
        uint8_t rows = total_bits / 8;

        // 1. 点选翻转具体某个 Bit
        for (uint8_t r = 0; r < rows; r++) {
            uint8_t base_bit = (rows - 1 - r) * 8;
            for (int b = 7; b >= 0; b--) {
                int16_t bx = start_x + (7 - b) * 47;
                int16_t by = grid_y + r * 30;

                if (rel_x >= bx && rel_x <= bx + 44 && rel_y >= by && rel_y <= by + 26) {
                    uint8_t bit_idx = base_bit + b;
                    uint32_t new_val = item->val ^ (1u << bit_idx); // 翻转选中的这一位
                    dap_watch_write_var(data, item, new_val);        // 立即通过 SWD 写入单片机
                    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                    return true;
                }
            }
        }

        // 2. 底部快捷按钮判定
        int16_t btn_y = 170;
        if (rel_y >= btn_y && rel_y <= btn_y + 30) {
            // [全部清0]
            if (rel_x >= start_x && rel_x <= start_x + 82) {
                dap_watch_write_var(data, item, 0);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return true;
            }
            // [全部置1]
            if (rel_x >= start_x + 88 && rel_x <= start_x + 170) {
                uint32_t mask = (item->size == 1) ? 0xFFu : ((item->size == 2) ? 0xFFFFu : 0xFFFFFFFFu);
                dap_watch_write_var(data, item, mask);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return true;
            }
            // [键盘输入] (切回全尺寸软键盘直接打字)
            if (rel_x >= start_x + 176 && rel_x <= start_x + 258) {
                ui_modal_hide(modal);
                char init_val_str[32];
                snprintf(init_val_str, sizeof(init_val_str), "%lu", (unsigned long)item->val);
                ui_input_set_text(&data->input, init_val_str);
                data->kb.mode = UI_KB_MODE_NUM_SYM;
                ui_keyboard_open(&data->kb, &data->input);
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return true;
            }
            // [关闭]
            if (rel_x >= start_x + 288 && rel_x <= start_x + 370) {
                ui_modal_hide(modal);
                data->watch_idx = -1;
                data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                return true;
            }
        }
    }
    return false;
}
static void dap_watch_draw_content(ui_tile_t *tile, const ui_rect_t *viewport, int16_t scroll_y, void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;

    int16_t x = viewport->x + 8;
    int16_t y = viewport->y + 8 - scroll_y;
    int16_t w = viewport->w - 16;

    ui_draw_string(tile, x + 4, y + 4, "变量监控", FONT_20, 0x1A7B, 0xFFFF);

    ui_btn_t add = data->add_watch;
    add.rect = ui_scrollview_content_rect_to_screen(&data->watch_scroll, data->add_watch.rect);
    ui_btn_draw(tile, &add);

    ui_btn_t clr = data->clear_watch;
    clr.rect = ui_scrollview_content_rect_to_screen(&data->watch_scroll, data->clear_watch.rect);
    ui_btn_draw(tile, &clr);

    int16_t card_start_y = y + 38;
    int16_t card_h = 50;

    if (data->watch_count == 0) {
        ui_rect_t empty_box = ui_rect_make(x, card_start_y, w, 100);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &empty_box, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0xF7BE);
            ui_draw_rect(tile, &empty_box, 0xCE59);
            ui_draw_string(tile, empty_box.x + (w - 120) / 2, empty_box.y + 30, "暂无监控变量", FONT_20, 0x8410, 0xF7BE);
            ui_draw_string(tile, empty_box.x + (w - 200) / 2, empty_box.y + 60, "点击 [添加变量] 开启观测", FONT_16, 0x8410, 0xF7BE);
        }
        return;
    }
    for (uint8_t i = 0; i < data->watch_count; i++) {
        dap_watch_item_t *item = &data->watch_items[i];
        if (!item->valid) continue;

        int16_t cy = card_start_y + i * (card_h + 6);
        ui_rect_t c_rect = ui_rect_make(x, cy, w, card_h);
        ui_rect_t overlap;

        if (!ui_rect_intersect(&tile->rect, &c_rect, &overlap)) continue;

        // bool changed = (item->val != item->last_val);
        // ⚡ 数值跳变后保持 500ms 醒目红色警示，之后自动平滑恢复为科技蓝
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool changed = (item->change_tick > 0 && (now - item->change_tick < 500));

        uint16_t bg_col     = 0xFFFF;
        uint16_t border_col = changed ? 0xF800 : 0xCE59;

        ui_draw_fill_rect(tile, &overlap, bg_col);
        ui_draw_rect(tile, &c_rect, border_col);
        ui_rect_t bar = ui_rect_make(c_rect.x, c_rect.y, 4, c_rect.h);
        ui_rect_t bar_ov;
        if (ui_rect_intersect(&tile->rect, &bar, &bar_ov)) {
            ui_draw_fill_rect(tile, &bar_ov, changed ? 0xF800 : 0x03DF);
        }
        ui_draw_string(tile, c_rect.x + 12, c_rect.y + 5, item->name, FONT_16, 0x1A7B, bg_col);
        char sub_info[48];
        snprintf(sub_info, sizeof(sub_info), "0x%08lX (%uB)", (unsigned long)item->addr, item->size);
        ui_draw_string(tile, c_rect.x + 12, c_rect.y + 26, sub_info, FONT_16, 0x8410, bg_col);
        char hex_str[24];
        if (item->size == 1) snprintf(hex_str, sizeof(hex_str), "0x%02X", (uint8_t)item->val);
        else if (item->size == 2) snprintf(hex_str, sizeof(hex_str), "0x%04X", (uint16_t)item->val);
        else snprintf(hex_str, sizeof(hex_str), "0x%08lX", (unsigned long)item->val);

        int16_t badge_w = (int16_t)strlen(hex_str) * 8 + 12;
        ui_rect_t badge = ui_rect_make(c_rect.x + c_rect.w - badge_w - 8, c_rect.y + 4, badge_w, 20);
        ui_rect_t badge_ov;
        ui_draw_string(tile, badge.x + 6, badge.y + 2, hex_str, FONT_16, 0x2104, 0xF7BE);
        char dec_str[32];
        if (item->size == 1) snprintf(dec_str, sizeof(dec_str), "DEC: %u", (uint8_t)item->val);
        else if (item->size == 2) snprintf(dec_str, sizeof(dec_str), "DEC: %u", (uint16_t)item->val);
        else snprintf(dec_str, sizeof(dec_str), "DEC: %ld", (long)item->val);

        int16_t dec_w = (int16_t)strlen(dec_str) * 8;
        uint16_t dec_col = changed ? 0xF800 : 0x03E0;
        ui_draw_string(tile, c_rect.x + c_rect.w - dec_w - 8, c_rect.y + 26, dec_str, FONT_16, dec_col, bg_col);
    }
}

// static bool dap_watch_process_touch(dap_data_t *data, bool touching, int16_t x, int16_t y, uint32_t tick)
// {
//     if (!data) return false;

//     int16_t content_x = (int16_t)(x - data->watch_scroll.view_rect.x);
//     int16_t content_y = (int16_t)(y - data->watch_scroll.view_rect.y + data->watch_scroll.scroll_y);

//     if (ui_scrollview_process_touch(&data->watch_scroll, touching, x, y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }

//     // 拖拽滑动过程中，绝不误触卡片和按钮
//     if (data->watch_scroll.is_dragged) {
//         return true;
//     }

//     if (ui_btn_process_touch(&data->add_watch, touching, content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }
//     if (ui_btn_process_touch(&data->clear_watch, touching, content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }

//     // ⚡ 点击变量卡片唤出全尺寸软键盘
//     static bool    s_touch_down = false;
//     static int16_t s_down_x = 0, s_down_y = 0;
//     static int16_t s_down_scroll_y = 0;

//     if (touching && !s_touch_down) {
//         s_touch_down = true;
//         s_down_x = x;
//         s_down_y = y;
//         s_down_scroll_y = data->watch_scroll.scroll_y;
//     }

//     if (!touching && s_touch_down) {
//         s_touch_down = false;
//         int16_t dx = x - s_down_x;
//         int16_t dy = y - s_down_y;
//         int16_t dscroll = data->watch_scroll.scroll_y - s_down_scroll_y;

//         // 仅在手指无位移且视口未移动时判定为纯点击
//         if (dx >= -6 && dx <= 6 && dy >= -6 && dy <= 6 && dscroll == 0) {
//             int16_t cy = (int16_t)(s_down_y - data->watch_scroll.view_rect.y + s_down_scroll_y);
//             int16_t cx = (int16_t)(s_down_x - data->watch_scroll.view_rect.x);

//             if (cx >= 8 && cx <= data->watch_scroll.view_rect.w - 8) {
//                 int16_t rel_card_y = cy - 38; // 卡片从 y=38 开始
//                 if (rel_card_y >= 0) {
//                     int idx = rel_card_y / 56; // 每张卡片高 50px + 6px 间距 = 56px
//                     if (idx < data->watch_count && (rel_card_y % 56) <= 50) {
//                         data->watch_idx = (int8_t)idx;
//                         dap_watch_item_t *it = &data->watch_items[idx];

//                         // 预填当前十进制纯数值
//                         char init_val_str[32];
//                         snprintf(init_val_str, sizeof(init_val_str), "%lu", (unsigned long)it->val);
//                         ui_input_set_text(&data->input, init_val_str);

//                         // 默认切到数字键盘模式并打开
//                         data->kb.mode = UI_KB_MODE_NUM_SYM;
//                         ui_keyboard_open(&data->kb, &data->input);

//                         data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
//                         return true;
//                     }
//                 }
//             }
//         }
//     }

//     return false;
// }
static bool dap_watch_process_touch(dap_data_t *data, bool touching, int16_t x, int16_t y, uint32_t tick)
{
    if (!data) return false;

    int16_t content_x = (int16_t)(x - data->watch_scroll.view_rect.x);
    int16_t content_y = (int16_t)(y - data->watch_scroll.view_rect.y + data->watch_scroll.scroll_y);

    if (ui_scrollview_process_touch(&data->watch_scroll, touching, x, y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
        return true;
    }

    // 拖拽滑动过程中，绝不误触卡片和按钮
    if (data->watch_scroll.is_dragged) {
        return true;
    }

    if (ui_btn_process_touch(&data->add_watch, touching, content_x, content_y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
        return true;
    }
    if (ui_btn_process_touch(&data->clear_watch, touching, content_x, content_y, tick)) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
        return true;
    }

    // ⚡ 点击变量卡片识别
    static bool    s_touch_down = false;
    static int16_t s_down_x = 0, s_down_y = 0;
    static int16_t s_down_scroll_y = 0;

    if (touching && !s_touch_down) {
        s_touch_down = true;
        s_down_x = x;
        s_down_y = y;
        s_down_scroll_y = data->watch_scroll.scroll_y;
    }

    if (!touching && s_touch_down) {
        s_touch_down = false;
        int16_t dx = x - s_down_x;
        int16_t dy = y - s_down_y;
        int16_t dscroll = data->watch_scroll.scroll_y - s_down_scroll_y;

        // 仅在手指无位移且视口未移动时判定为纯点击
        if (dx >= -6 && dx <= 6 && dy >= -6 && dy <= 6 && dscroll == 0) {
            int16_t cy = (int16_t)(s_down_y - data->watch_scroll.view_rect.y + s_down_scroll_y);
            int16_t cx = (int16_t)(s_down_x - data->watch_scroll.view_rect.x);

            if (cx >= 8 && cx <= data->watch_scroll.view_rect.w - 8) {
                int16_t rel_card_y = cy - 38; // 卡片从 y=38 开始
                if (rel_card_y >= 0) {
                    int idx = rel_card_y / 56; // 每张卡片高 50px + 6px 间距 = 56px
                    if (idx < data->watch_count && (rel_card_y % 56) <= 50) {
                        data->watch_idx = (int8_t)idx;

                        // ⚡ 方案 A 核心改动：不再打开打字键盘，直接唤出 32 位点阵控制弹窗！
                        ui_modal_show(&data->bit_modal, "位状态控制", data);

                        data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}
// static bool dap_watch_process_touch(dap_data_t *data, bool touching, int16_t x, int16_t y, uint32_t tick)
// {
//     if (!data) return false;

//     int16_t content_x = (int16_t)(x - data->watch_scroll.view_rect.x);
//     int16_t content_y = (int16_t)(y - data->watch_scroll.view_rect.y + data->watch_scroll.scroll_y);

//     if (ui_scrollview_process_touch(&data->watch_scroll, touching, x, y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }

//     // 拖拽滑动过程中，绝不误触卡片和按钮
//     if (data->watch_scroll.is_dragged) {
//         return true;
//     }

//     if (ui_btn_process_touch(&data->add_watch, touching, content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }
//     if (ui_btn_process_touch(&data->clear_watch, touching, content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->watch_scroll.view_rect);
//         return true;
//     }
//     static bool    s_touch_down = false;
//     static int16_t s_down_x = 0, s_down_y = 0;
//     static int16_t s_down_scroll_y = 0;

//     if (touching && !s_touch_down) {
//         s_touch_down = true;
//         s_down_x = x;
//         s_down_y = y;
//         s_down_scroll_y = data->watch_scroll.scroll_y;
//     }
//     if (!touching && s_touch_down) {
//         s_touch_down = false;
//         int16_t dx = x - s_down_x;
//         int16_t dy = y - s_down_y;
//         int16_t dscroll = data->watch_scroll.scroll_y - s_down_scroll_y;
//         if (dx >= -6 && dx <= 6 && dy >= -6 && dy <= 6 && dscroll == 0) {
//             int16_t cy = (int16_t)(s_down_y - data->watch_scroll.view_rect.y + s_down_scroll_y);
//             int16_t cx = (int16_t)(s_down_x - data->watch_scroll.view_rect.x);

//             if (cx >= 8 && cx <= data->watch_scroll.view_rect.w - 8) {
//                 int16_t rel_card_y = cy - 38; 
//                 if (rel_card_y >= 0) {
//                     int idx = rel_card_y / 56; 
//                     if (idx < data->watch_count && (rel_card_y % 56) <= 50) {
//                         data->watch_idx = (int8_t)idx;
//                         dap_watch_item_t *it = &data->watch_items[idx];
//                         char init_val_str[32];
//                         snprintf(init_val_str, sizeof(init_val_str), "%lu", (unsigned long)it->val);
//                         ui_input_set_text(&data->input, init_val_str);
//                         data->kb.mode = UI_KB_MODE_NUM_SYM;
//                         ui_keyboard_open(&data->kb, &data->input);

//                         data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
//                         return true;
//                     }
//                 }
//             }
//         }
//     }

//     return false;
// }
/* ============================================================================
 * 全局生命周期与分发调度
 * ============================================================================ */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab,
                           uint8_t new_tab, void *user_data)
{
    (void)old_tab; (void)new_tab;
    dap_data_t *data = (dap_data_t *)user_data;
    if (data) {
        auto_default(data);
        data->dirty_rect = tv->content_rect;
    }
}

static app_t s_dap;

static void dap_init(app_t *self)
{
    // _Static_assert(sizeof(dap_data_t) <= 10240, "dap_data_t 超出 10KB 静态内存限制！");
    self->user_data = g_static_app_ram;
}

static void dap_enter(app_t *self)
{
    dap_data_t *data = (dap_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(dap_data_t));
    self->user_data = data;
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    const fun_swd_config_t *swd_cfg =fun_swd_config_get();
    if (swd_cfg) data->swd = *swd_cfg;
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_dap_tabs, DAP_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;
    ui_scrollview_init(&data->status_scroll, 120, 40, 350, 260,
                       DAP_STATUS_CONTENT_H, dap_status_draw_content);
    data->status_scroll.user_data = data;
    data->status_scroll.color_bg = 0xFFFF;
    data->status_scroll.color_scrollbar = 0xA514;
    data->status_scroll.show_scrollbar = false;

    ui_scrollview_init(&data->config_scroll, 120, 40, 350, 260,
                       DAP_CONFIG_CONTENT_H, dap_config_draw_content);
    data->config_scroll.user_data = data;
    data->config_scroll.color_bg = 0xFFFF;
    data->config_scroll.color_scrollbar = 0xA514;
    data->config_scroll.show_scrollbar = true;

    ui_scrollview_init(&data->term_scroll, 120, 40, 350, 260,
                       DAP_TERM_CONTENT_H, dap_term_draw_content);
    data->term_scroll.user_data = data;
    data->term_scroll.color_bg = 0xFFFF;
    data->term_scroll.color_scrollbar = 0xA514;
    data->term_scroll.show_scrollbar = true;

    /* 3. 初始化 Tab 0 组件 */
    data->status_card = ui_rect_make(134, 78, 322, 140);
    ui_btn_init(&data->pio_probe, 144, 226, 302, 44, "开始探测", on_probe_click);
    data->pio_probe.user_data = data;
    data->pio_probe.style = UI_BTN_STYLE_ROUND;
    data->pio_probe.font_size = FONT_20;
    ui_btn_set_color(&data->pio_probe, 0xE71C, 0xCE79, 0x3186);
    data->target_detected = false;
    fun_pio_swd_init(&data->pio_swd);
    data->pio_powered=false;
    data->probe_status = DAP_PROBE_IDLE;
    data->dp_id=0;
    data->cpu_id=0;
    data->dbg_id=0;
    data->flash_size=0;

    data->pio_ack=0;
    data->pio_ctrl_ack=0;
    data->pio_ctrl_stat=0;
    //tab1配置界面
    pin_manager_t *pin_manager=pin_manager_get_default();
    ui_pin_config_init(&data->pin_swdio_cf,16,52,230,"SWDIO",pin_manager,PIN_OWNER_SWD,PIN_SOURCE_PIO,PIN_FUNC_PIO,0,data->swd.swdio_gpio,on_dap_pin_config_changed);
    data->pin_swdio_cf.user_data=data;
    ui_pin_config_init(&data->pin_swclk_cf,16,104,230,"SWCLK",pin_manager,PIN_OWNER_SWD,PIN_SOURCE_PIO,PIN_FUNC_PIO,0,data->swd.swclk_gpio,on_dap_pin_config_changed);
    data->pin_swclk_cf.user_data=data;
    ui_pin_config_set_allowed_mask(&data->pin_swdio_cf,UINT32_MAX);
    ui_pin_config_set_allowed_mask(&data->pin_swclk_cf,UINT32_MAX);
    uint8_t swdio_gpio =ui_pin_config_get_selected(&data->pin_swdio_cf);
    uint8_t swclk_gpio = ui_pin_config_get_selected(&data->pin_swclk_cf);
    ui_pin_config_set_excluded_mask(&data->pin_swdio_cf,swclk_gpio < 32u ?(UINT32_C(1) << swclk_gpio) : 0u);
    ui_pin_config_set_excluded_mask(&data->pin_swclk_cf,swdio_gpio < 32u ?(UINT32_C(1) << swdio_gpio) : 0u);
    ui_btn_init(&data->btn_apply,160,168+32,180,44,"应用配置",on_dap_apply_click);
    data->btn_apply.user_data=data;
    data->btn_apply.style=UI_BTN_STYLE_ROUND;
    data->btn_apply.font_size=FONT_20;
    ui_btn_set_color(&data->btn_apply,0xD69A,0xA514,0x4208);
    ui_toast_init(&data->toast);
    ui_switch_init(&data->dap_switch,120,160,64,32,g_dap_enabled,on_dap_switch_change);
    data->dap_switch.user_data = data;
    ui_switch_set_color(&data->dap_switch,0x7BEF,0x5DCC,0xFFFF); 
    //tab3
    // ui_btn_init(&data->run,8,16,80,44,"运行",on_run_click);
    // data->run.user_data=data;
    // ui_btn_set_color(&data->run,0xEF7D,0xA514,0x4208);
    // ui_btn_init(&data->halt,8+82,16,80,44,"停止",on_halt_click);
    // data->halt.user_data=data;
    // ui_btn_set_color(&data->halt,0xEF7D,0xA514,0x4208);
    // ui_btn_init(&data->step,8+82*2,16,80,44,"单步",on_step_click);
    // data->step.user_data=data;
    // ui_btn_set_color(&data->step,0xEF7D,0xA514,0x4208);
    // ui_btn_init(&data->reset,8+82*3,16,80,44,"复位",on_reset_click);
    // data->reset.user_data=data;
    // ui_btn_set_color(&data->reset,0xEF7D,0xA514,0x4208);
    // // ui_btn_set_color(&data->run,0xD69A,0xBDF,0x4208);
    // data->reg_card=ui_rect_make(8,64,322,194);
    // data->regs_valid=false;
    // data->is_halted=false;
    // ui_btn_init(&data->step_over,8,264,80,44,"跳过",on_step_over_click);
    // data->step_over.user_data=data;
    // ui_btn_set_color(&data->step_over,0xEF7D,0xA514,0x4208);
    //  ui_btn_init(&data->step_out,90,264,80,44,"步出",on_step_out_click);
    // data->step_out.user_data=data;
    // ui_btn_set_color(&data->step_out,0xEF7D,0xA514,0x4208);
    int16_t bw = 50, bh = 32, gap = 6, bx = 8, by = 10;
    ui_btn_init(&data->run,       bx + 0 * (bw + gap), by, bw, bh, "运行", on_run_click);
    data->run.user_data = data; data->run.font_size = FONT_16;
    ui_btn_set_color(&data->run, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->halt,      bx + 1 * (bw + gap), by, bw, bh, "停止", on_halt_click);
    data->halt.user_data = data; data->halt.font_size = FONT_16;
    ui_btn_set_color(&data->halt, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->step,      bx + 2 * (bw + gap), by, bw, bh, "步入", on_step_click);
    data->step.user_data = data; data->step.font_size = FONT_16;
    ui_btn_set_color(&data->step, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->step_over, bx + 3 * (bw + gap), by, bw, bh, "跳过", on_step_over_click);
    data->step_over.user_data = data; data->step_over.font_size = FONT_16;
    ui_btn_set_color(&data->step_over, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->step_out,  bx + 4 * (bw + gap), by, bw, bh, "步出", on_step_out_click);
    data->step_out.user_data = data; data->step_out.font_size = FONT_16;
    ui_btn_set_color(&data->step_out, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->reset,     bx + 5 * (bw + gap), by, bw, bh, "复位", on_reset_click);
    data->reset.user_data = data; data->reset.font_size = FONT_16;
    ui_btn_set_color(&data->reset, 0xEF7D, 0xA514, 0x4208);

    // 寄存器面板：刚好紧随其后落入 [48 ~ 242]，一屏完整呈现
    data->reg_card = ui_rect_make(8, 48, 322, 194);
    data->regs_valid = false;
    data->is_halted = false;
    data->show_disasm =false;
    ui_btn_init(&data->view_toggle,8,250,50,32,"汇编",on_view_toggle_click);
    data->view_toggle.user_data=data;
    ui_btn_set_color(&data->view_toggle, 0xEF7D, 0xA514, 0x4208);
    data->auto_step_enablde=false;
    data->auto_step_ms=1000;
    data->auto_step_last_tick=0;
    ui_btn_init(&data->auto_step,8+54,250,50,32,"自动",on_auto_step_click);
    data->auto_step.user_data=data;
    ui_btn_set_color(&data->auto_step, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->bp, 8 + 54 * 2, 250, 50, 32, "断点", on_bp_click);
    data->bp.user_data = data;
    data->bp.font_size = FONT_16;
    ui_btn_set_color(&data->bp, 0xEF7D, 0xA514, 0x4208);
    ui_btn_init(&data->clear_bp, 8 + 54 * 3, 250, 100, 32, "清空断点", on_clear_bp_click);
    data->clear_bp.user_data = data;
    data->clear_bp.font_size = FONT_16;
    ui_btn_set_color(&data->clear_bp, 0xEF7D, 0xA514, 0x4208);
    data->bp_addr = 0;
    data->bp_active = false;
    data->last_halt_check = 0;
    ui_modal_init(&data->bp_modal, LCD_WIDTH, LCD_HEIGHT, 380, 240, 200,
                  "设置函数断点", UI_MODAL_BTN_NONE,
                  on_bp_modal_draw_content, on_bp_modal_touch_content, NULL);
    data->bp_modal.user_data = data;


    strcpy(data->sym_file_path, "0:/app.sym");
    strcpy(data->modal_cur_path, "0:/");
    data->fs_mounted = false;
    sym_loader_init();
    if (f_mount(&data->fs, "0:", 1) == FR_OK) {
        data->fs_mounted = true;
        sym_loader_load_from_sd(data->sym_file_path);
    }

    ui_btn_init(&data->choose_sym, 36+24*6, 268, 120, 38, "选择文件", on_choose_sym_click);
    data->choose_sym.user_data = data;
    data->choose_sym.font_size = FONT_16;
    data->choose_sym.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->choose_sym, 0x03DF, 0x0275, 0xFFFF);

    // 模态弹窗窗口大小 380x240，内容高度初始预设 200，无确定/取消按键 (点击文件即生效)
    ui_modal_init(&data->sym_modal, LCD_WIDTH, LCD_HEIGHT, 380, 240, 200,
                  "选择符号映射文件 (.sym)", UI_MODAL_BTN_NONE,
                  on_sym_modal_draw_content, on_sym_modal_touch_content, NULL);
    data->sym_modal.user_data = data;
    //tab3
    ui_scrollview_init(&data->watch_scroll, 120, 40, 350, 260,DAP_WATCH_CONTENT_H, dap_watch_draw_content);
    data->watch_scroll.user_data = data;
    data->watch_scroll.color_bg = 0xFFFF;
    data->watch_scroll.color_scrollbar = 0xA514;
    data->watch_scroll.show_scrollbar = true;

    ui_btn_init(&data->add_watch, 10+120, 8, 96, 30, "添加变量", on_add_watch_click);
    data->add_watch.user_data = data;
    data->add_watch.font_size = FONT_16;
    data->add_watch.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->add_watch, 0xEF7D, 0xA514, 0x4208);

    ui_btn_init(&data->clear_watch, 114+120, 8, 60, 30, "清空", on_clear_watch_click);
    data->clear_watch.user_data = data;
    data->clear_watch.font_size = FONT_16;
    data->clear_watch.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->clear_watch, 0xEF7D, 0xA514, 0x4208);
    ui_modal_init(&data->var_modal, LCD_WIDTH, LCD_HEIGHT, 380, 240, 200,"添加监控变量", UI_MODAL_BTN_NONE,on_var_modal_draw_content, on_var_modal_touch_content, NULL);
    data->var_modal.user_data = data;
    data->watch_count = 0;
    data->last_watch_tick=0;
    ui_input_init(&data->input,0,0,0,0,"",NULL);
    ui_keyboard_init(&data->kb,on_var_kb_submit);
    data->kb.user_data=data;
    data->watch_idx=-1;
    ui_modal_init(&data->bit_modal, LCD_WIDTH, LCD_HEIGHT, 400, 240, 200, "位状态控制", UI_MODAL_BTN_NONE, on_bit_modal_draw_content, on_bit_modal_touch_content, NULL);
    data->bit_modal.user_data = data;

    //tab4
ui_terminal_init(&data->rtt_terminal, RTT_TERM_X, RTT_TERM_Y, RTT_TERM_W, RTT_TERM_H);
    data->rtt_terminal.color_bg        = 0x10A2;
    data->rtt_terminal.color_border    = 0x31A6;
    data->rtt_terminal.color_text_def  = 0xDEFB;
    data->rtt_terminal.color_scrollbar = 0x05DF;
    ui_terminal_write_line(&data->rtt_terminal, "RTT 交互终端就绪", 0x5E7F);

    // ⚡ 底部输入控制行 (Y=272, H=36)
    // 1. 命令输入框 (宽 150)
    ui_input_init(&data->input, 120, 272, 150, 36, "输入指令", on_rtt_input_focus);
    data->input.user_data = data;
    data->input.color_bg = 0xFFFF;
    data->input.color_border = 0xC618;
    data->input.color_focus_border = 0x03DF;
    data->input.color_text = 0x4208;
    data->input.color_placeholder = 0x8410;

    // 2. 发送按钮
    ui_btn_init(&data->rtt_send, 276, 272, 54, 36, "发送", on_rtt_send_click);
    data->rtt_send.user_data = data;
    data->rtt_send.style = UI_BTN_STYLE_ROUND;
    data->rtt_send.font_size = FONT_16;
    ui_btn_set_color(&data->rtt_send, 0x03DF, 0x0275, 0xFFFF);

    // 3. 重新搜索按钮
    ui_btn_init(&data->rtt_search, 336, 272, 64, 36, "重搜", on_rtt_search_click);
    data->rtt_search.user_data = data;
    data->rtt_search.style = UI_BTN_STYLE_ROUND;
    data->rtt_search.font_size = FONT_16;
    ui_btn_set_color(&data->rtt_search, 0x05E0, 0x03E0, 0xFFFF);

    // 4. 清屏按钮
    ui_btn_init(&data->rtt_clear, 406, 272, 58, 36, "清屏", on_rtt_clear_click);
    data->rtt_clear.user_data = data;
    data->rtt_clear.style = UI_BTN_STYLE_ROUND;
    data->rtt_clear.font_size = FONT_16;
    ui_btn_set_color(&data->rtt_clear, 0x8410, 0x630C, 0xFFFF);

    // 键盘初始化挂接统一回调
    ui_keyboard_init(&data->kb, on_dap_kb_submit);
    data->kb.user_data = data;

    data->rtt_found = false;
    data->rtt_worker_enable = true;
    s_rtt_head = 0;
    s_rtt_tail = 0;
    s_dap_session_active = true;

    // 启动第二核专用轮询线程
    multicore_reset_core1();
    multicore_launch_core1(dap_core1_rtt_worker);
}
static void dap_exit(app_t *self)
{
    dap_data_t *data = (dap_data_t *)self->user_data;

    if (data && data->pio_swd.opened) {
        
        uint32_t demcr = 0;
        if (fun_pio_swd_read_memory32(&data->pio_swd, 0xE000EDFCu, &demcr)) {
            fun_pio_swd_write_memory32(&data->pio_swd, 0xE000EDFCu, demcr & ~1u);
        }
        fun_pio_swd_run_target(&data->pio_swd);
        fun_pio_swd_close(&data->pio_swd);
    }
    s_dap_session_active = false;
    data->rtt_worker_enable = false;
    multicore_reset_core1(); // 彻底停止并复位 Core 1
    self->user_data = NULL;
}
static void dap_update(app_t *self, uint32_t now_tick)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return;

    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
    if (ui_toast_update(&data->toast, now_tick)) {
        data->dirty_rect = rect_union_box(
            data->dirty_rect,
            ui_toast_get_dirty_union(&data->toast));
    }
    if (cmsis_dap_is_host_active()) {
        
        if (data->pio_swd.opened) {
            fun_pio_swd_close(&data->pio_swd);
            data->pio_powered = false;
        }
        return; 
    }
    const fun_swd_config_t *cfg = fun_swd_config_get();
    if (cfg && cfg->clock_hz != data->swd.clock_hz) {
        data->swd = *cfg;
        if (data->tabview.active_tab_idx == 0) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->status_card);
        }
    }

    if (data->tabview.active_tab_idx == 2 && data->auto_step_enablde && data->is_halted && data->pio_swd.opened) {
        if (now_tick - data->auto_step_last_tick >= data->auto_step_ms) {
            data->auto_step_last_tick = now_tick;
            if (fun_pio_swd_step_over(&data->pio_swd, 1000)) {
                data->is_halted = true;
                dap_read_all_regs(data);
            } else {
                auto_default(data);
            }
            data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
        }
    }
    if (data->tabview.active_tab_idx == 2&&!data->is_halted && data->pio_swd.opened) {
        if (now_tick - data->last_halt_check >= 50) {
            data->last_halt_check = now_tick;
            bool is_halted = false;
            if (fun_pio_swd_is_halted(&data->pio_swd, &is_halted) && is_halted) {
                data->is_halted = true;
                dap_read_all_regs(data);
                    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
            }
        }
    }
    if (data->tabview.active_tab_idx == 3 && data->watch_count > 0) {
        if (!data->pio_swd.opened) {
            swd_open(data);
        }
        if (data->pio_swd.opened && (now_tick - data->last_watch_tick >= 50)) {
            data->last_watch_tick = now_tick;
            for (uint8_t i = 0; i < data->watch_count; i++) {
                dap_watch_item_t *it = &data->watch_items[i];
                if (!it->valid) continue;
                uint32_t al_addr = it->addr & ~3u;
                uint32_t shift = (it->addr & 3u) * 8u;
                uint32_t raw_word = 0;
                if (fun_pio_swd_read_memory32(&data->pio_swd, al_addr, &raw_word)) {
                    uint32_t new_val = 0;
                    if (it->size == 1)      new_val = (raw_word >> shift) & 0xFFu;
                    else if (it->size == 2) new_val = (raw_word >> shift) & 0xFFFFu;
                    else                    new_val = raw_word;
                    if (new_val != it->val) {
                        it->last_val = it->val;
                        it->val = new_val;
                        it->change_tick = now_tick;
                        ui_rect_t card_rect = dap_watch_get_card_screen_rect(data, i);
                        data->dirty_rect = rect_union_box(data->dirty_rect, card_rect);
                        if (data->bit_modal.is_open && data->watch_idx == i) {
                            data->dirty_rect = rect_union_box(data->dirty_rect, data->bit_modal.rect);
                        }
                    }
                }
            }
        }
        for (uint8_t i = 0; i < data->watch_count; i++) {
            dap_watch_item_t *it = &data->watch_items[i];
            if (it->valid && it->change_tick > 0) {
                if (now_tick - it->change_tick >= 500) {
                    it->change_tick = 0;
                    ui_rect_t card_rect = dap_watch_get_card_screen_rect(data, i);
                    data->dirty_rect = rect_union_box(data->dirty_rect, card_rect);
                    if (data->bit_modal.is_open && data->watch_idx == i) {
                            data->dirty_rect = rect_union_box(data->dirty_rect, data->bit_modal.rect);
                        }
                }
            }
        }
    }
    // 将 Core 1 收集到的 RTT 日志行刷入 UI 终端
    bool term_has_new = false;
    while (s_rtt_tail != s_rtt_head) {
        rtt_term_msg_t *msg = &s_rtt_queue[s_rtt_tail];
        ui_terminal_write_line(&data->rtt_terminal, msg->text, msg->color);
        s_rtt_tail = (uint8_t)((s_rtt_tail + 1) % RTT_QUEUE_SIZE);
        term_has_new = true;
    }

    if (term_has_new && data->tabview.active_tab_idx == 4) {
        data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_terminal.rect);
    }
}
static bool dap_process_touch(app_t *self, bool touching, int16_t x,
                              int16_t y, uint32_t tick)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return false;

  if (data->sym_modal.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->sym_modal, touching, x, y, tick);
        if (need_refresh) 
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        return true; 
    }
    if (data->bp_modal.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->bp_modal, touching, x, y, tick);
        if (need_refresh) data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        return true;
    }
    if (data->var_modal.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->var_modal, touching, x, y, tick);
        if (need_refresh) data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        return true;
    }
    if (data->kb.is_open) {
        bool handled = ui_keyboard_process_touch(&data->kb, touching, x, y, tick);
        if (handled || !data->kb.is_open) {
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        return true;
    }
    if (data->bit_modal.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->bit_modal, touching, x, y, tick);
        if (need_refresh) data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        return true;
    }
    if (ui_tabview_process_touch(&data->tabview, touching, x, y, tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect = data->tabview.rect;
        } else {
            data->dirty_rect = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    bool handled = false;
    switch (data->tabview.active_tab_idx) {
    case 0:
        handled = dap_status_process_touch(data, touching, x, y, tick);
        break;
    case 1:
        handled = dap_config_process_touch(data, touching, x, y, tick);
        break;
    case 2:
        handled = dap_term_process_touch(data, touching, x, y, tick);
        break;
    case 3:
        handled =dap_watch_process_touch(data,touching,x,y,tick);
        break;
    case 4:
        if (ui_terminal_process_touch(&data->rtt_terminal, touching, x, y, tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_terminal.rect);
            handled = true;
        }
        if (ui_input_process_touch(&data->input, touching, x, y, tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->input.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->rtt_send, touching, x, y, tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_send.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->rtt_clear, touching, x, y, tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_clear.rect);
            handled = true;
        }
        if (ui_btn_process_touch(&data->rtt_search, touching, x, y, tick)) {
            data->dirty_rect = rect_union_box(data->dirty_rect, data->rtt_search.rect);
            handled = true;
        }
        break;
    default:
        break;
    }

    return handled || (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}
static void dap_draw(app_t *self, ui_tile_t *tile)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return;

    /* 绘制通用背景 */
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t overlap;
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    /* 绘制 Tab 容器边框及标签栏 */
    ui_tabview_draw(tile, &data->tabview);
    if (ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        /* 路由分发到激活 Tab 的绘制 */
        switch (data->tabview.active_tab_idx) {
        case 0:
            ui_scrollview_draw(tile, &data->status_scroll);
            break;
        case 1:
            ui_scrollview_draw(tile, &data->config_scroll);
            break;
        case 2:
            ui_scrollview_draw(tile, &data->term_scroll);
            break;
        case 3:
            ui_scrollview_draw(tile,&data->watch_scroll);
            break;
            case 4:
            ui_terminal_draw(tile, &data->rtt_terminal);
            ui_input_draw(tile, &data->input);
            ui_btn_draw(tile, &data->rtt_send);
            ui_btn_draw(tile, &data->rtt_search);
            ui_btn_draw(tile, &data->rtt_clear);
            break;
        default:
            break;
        }

        /* 引脚配置弹窗是覆盖层，必须在页面内容之后绘制。 */
        if (data->tabview.active_tab_idx == 1) {
            ui_pin_config_draw_modal(tile, &data->pin_swdio_cf);
            ui_pin_config_draw_modal(tile, &data->pin_swclk_cf);
        }
    }
    if (data->kb.is_open && data->watch_idx >= 0 && data->watch_idx < data->watch_count) {
        dap_watch_item_t *item = &data->watch_items[data->watch_idx];
        ui_rect_t banner = ui_rect_make(0, 88, LCD_WIDTH, 32);
        ui_rect_t overlap;
        if (ui_rect_intersect(&tile->rect, &banner, &overlap)) {
            ui_draw_fill_rect(tile, &overlap, 0x1A7B); // 深藏青底色
            char tip[64];
            snprintf(tip, sizeof(tip), "正在修改: %s [0x%08lX]", item->name, (unsigned long)item->addr);
            ui_draw_string(tile, 16, banner.y + 8, tip, FONT_16, 0xFFFF, 0x1A7B);
        }
    }

    /* Toast 是全局覆盖层，即使切片不在 Tab 内容区域也必须绘制。 */
    ui_toast_draw(tile, &data->toast);
    ui_modal_draw(tile, &data->sym_modal);
    ui_modal_draw(tile, &data->bp_modal);
    ui_modal_draw(tile, &data->var_modal);
    ui_modal_draw(tile, &data->bit_modal);
    ui_keyboard_draw(tile, &data->kb);
}

app_t *app_dap_get_instance(void)
{
    s_dap.id = APP_ID_CUSTOM_0;
    s_dap.name = "DAP工具";
    s_dap.init = dap_init;
    s_dap.enter = dap_enter;
    s_dap.exit = dap_exit;
    s_dap.update = dap_update;
    s_dap.process_touch = dap_process_touch;
    s_dap.draw = dap_draw;
    return &s_dap;
}
