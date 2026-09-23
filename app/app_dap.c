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
static const char *s_dap_tabs[] = { "状态", "配置", "终端" };
#define DAP_TAB_COUNT          ((uint8_t)(sizeof(s_dap_tabs) / sizeof(s_dap_tabs[0])))

#define DAP_STATUS_CONTENT_H   260
#define DAP_CONFIG_CONTENT_H   360   //260
#define DAP_TERM_CONTENT_H     360
#define MAX_SYM_ITEMS 16
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
static void dap_read_all_regs(dap_data_t *data)
{
    if(!data|| !data->pio_swd.opened) return;
    for(uint8_t i=0;i<=16;i++){
        if(!fun_pio_swd_read_core_reg(&data->pio_swd,i,&data->core_regs[i]))
            data->core_regs[i] = 0x0000000000;
    }
    data->regs_valid=true;
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
    bool in_viewport = (tx >= vr.x && tx < (vr.x + vr.w) &&ty >= vr.y && ty < (vr.y + vr.h));
    static bool    s_was_touching = false;
    static int16_t s_down_idx = -1;
    int16_t rel_y = ty - vr.y + modal->sv_body.scroll_y;
    int idx = (in_viewport && rel_y >= 0) ? (rel_y / 34) : -1;
    if (is_touching && !s_was_touching) {
        s_was_touching = true;
        s_down_idx = idx;
        return false;
    }
    if (!is_touching && s_was_touching) {
        s_was_touching = false;
        if (modal->sv_body.is_dragged) {
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
    if(fun_pio_swd_step_target(&data->pio_swd,100))
    {
        data->is_halted=true;
        dap_read_all_regs(data);
    }
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_reset_click(ui_btn_t *btn,void *user_data)
{
     dap_data_t *data = (dap_data_t *)user_data;
    if (!data) return;
    if (!swd_open(data)) return;
    fun_pio_swd_reset_target(&data->pio_swd);
    data->is_halted=false;
    data->pio_swd.mem_select_valid=false;
    data->pio_swd.mem_csw_valid=false;
    data->pio_swd.mem_tar_valid=false;
    auto_default(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, data->term_scroll.view_rect);
}
static void on_step_over_click(ui_btn_t *btn,void *user_data)
{
    dap_data_t *data = (dap_data_t *)user_data;
    if (!data||!swd_open(data)) return;
    bool halted =false;
    fun_pio_swd_is_halted(&data->pio_swd,&halted);
    if(!halted) fun_pio_swd_halt_target(&data->pio_swd,100);
    if(fun_pio_swd_step_over(&data->pio_swd,3000)){
        data->is_halted=true;
        dap_read_all_regs(data);
    }
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
    if(fun_pio_swd_step_out(&data->pio_swd,3000)){
        data->is_halted=true;
        dap_read_all_regs(data);
    }
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
    snprintf(path_buf, sizeof(path_buf), "当前: %s", data->sym_file_path[0] ? data->sym_file_path : "未设置");
    ui_draw_string(tile, x, sym_y + 32, path_buf, FONT_16, 0x1A7B, 0xFFFF);

    ui_btn_t sym_btn = data->choose_sym;
    sym_btn.rect = ui_scrollview_content_rect_to_screen(&data->config_scroll, data->choose_sym.rect);
    ui_btn_draw(tile, &sym_btn);




}

// static bool dap_config_process_touch(dap_data_t *data, bool touching,int16_t x, int16_t y, uint32_t tick)
// {
//     if (!data) {
//         return false;
//     }

//     /* 弹窗打开时优先交给弹窗，避免外层页面抢走滑动手势。 */
//     if (data->pin_swdio_cf.modal.is_open) {
//         bool handled = ui_pin_config_process_modal_touch(
//             &data->pin_swdio_cf, touching, x, y, tick);
//         if (handled) {
//             data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//         }
//         return handled;
//     }

//     if (data->pin_swclk_cf.modal.is_open) {
//         bool handled = ui_pin_config_process_modal_touch(
//             &data->pin_swclk_cf, touching, x, y, tick);
//         if (handled) {
//             data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//         }
//         return handled;
//     }

//     /* Tab1 是固定布局，不调用 config_scroll 的触摸处理。 */
//     /* 普通控件使用 ScrollView 内容坐标；当前配置页不滚动，
//      * 但仍需减去视口原点，保证绘制区域和触摸区域一致。 */
//     int16_t content_x = (int16_t)(x - data->config_scroll.view_rect.x);
//     int16_t content_y = (int16_t)(y - data->config_scroll.view_rect.y +
//                                    data->config_scroll.scroll_y);

//     if (ui_pin_config_process_touch(&data->pin_swdio_cf, touching, content_x, content_y, tick)) {
//         data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//         return true;
//     }

//     if (ui_pin_config_process_touch(&data->pin_swclk_cf, touching, content_x, content_y, tick)) {
//         data->dirty_rect = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
//         return true;
//     }
//     if (ui_btn_process_touch(&data->btn_apply,touching,content_x,content_y,tick)) {
//     data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
//     return true;
//     }
//     if (ui_switch_process_touch(&data->dap_switch, touching,content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
//         return true;
//     }
//     if (ui_btn_process_touch(&data->choose_sym, touching, content_x, content_y, tick)) {
//         data->dirty_rect = rect_union_box(data->dirty_rect, data->config_scroll.view_rect);
//         return true;
//     }

//     return false;
// }
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
    return false;
}

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
    /* 1. 初始化 TabView 容器 */
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_dap_tabs, DAP_TAB_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    /* 2. 分别初始化三个页面的 ScrollView */
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
}

static void dap_exit(app_t *self)
{
    dap_data_t *data = (dap_data_t *)self->user_data;

    if (data && data->pio_swd.opened) {
        fun_pio_swd_close(&data->pio_swd);
    }
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
    const fun_swd_config_t *cfg=fun_swd_config_get();
    if(cfg&&cfg->clock_hz!=data->swd.clock_hz){
        data->swd=*cfg;
        if(data->tabview.active_tab_idx==0)
        data->dirty_rect =rect_union_box(data->dirty_rect,data->status_card);
    }
    if(data->tabview.active_tab_idx == 2&&data->auto_step_enablde&&data->is_halted&&data->pio_swd.opened){
        if(now_tick-data->auto_step_last_tick >= data->auto_step_ms){
            data->auto_step_last_tick=now_tick;
            if(fun_pio_swd_step_over(&data->pio_swd,1000))
            {
                data->is_halted=true;
                dap_read_all_regs(data);
            }else
            auto_default(data);
            data->dirty_rect=rect_union_box(data->dirty_rect,data->term_scroll.view_rect);
        }
    }
}
static bool dap_process_touch(app_t *self, bool touching, int16_t x,
                              int16_t y, uint32_t tick)
{
    dap_data_t *data = (dap_data_t *)self->user_data;
    if (!data) return false;

  if (data->sym_modal.is_open) {
        bool need_refresh = ui_modal_process_touch(&data->sym_modal, touching, x, y, tick);
        if (need_refresh) {
            data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        return true; // 无论弹窗内发生什么，直接就地截杀，底层代码碰都别想碰到！
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
        default:
            break;
        }

        /* 引脚配置弹窗是覆盖层，必须在页面内容之后绘制。 */
        if (data->tabview.active_tab_idx == 1) {
            ui_pin_config_draw_modal(tile, &data->pin_swdio_cf);
            ui_pin_config_draw_modal(tile, &data->pin_swclk_cf);
        }
    }

    /* Toast 是全局覆盖层，即使切片不在 Tab 内容区域也必须绘制。 */
    ui_toast_draw(tile, &data->toast);
    ui_modal_draw(tile, &data->sym_modal);
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
