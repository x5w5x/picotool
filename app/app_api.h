#ifndef APP_API_H
#define APP_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui_tile.h"
#include "ui_btn.h"    // plugin 需要 ui_btn_t 与 ui_btn_cb_t 类型才能注册按钮回调
#include "chinese.h"   // FontSize_t / FONT_16 / FONT_20 / FONT_24

#define APP_MAGIC       0x50415050 // "PAPP"
#define APP_RAM_BASE    0x20038000 // 动态 App 物理基地址 (32KB 空间)
#define APP_RAM_SIZE    (32 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 主系统导出的 API 表 ================= */
typedef struct {
    uint32_t version;

    // 系统时钟与延时
    uint32_t (*get_time_ms)(void);
    uint64_t (*get_time_us)(void);
    void     (*delay_ms)(uint32_t ms);
    void     (*delay_us)(uint64_t us);

    // 屏幕显示 — LCD 原语(由主系统统一安全调度)
    void     (*lcd_set_window)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void     (*lcd_bulk_write)(const uint8_t *buf, uint32_t len);
    void     (*lcd_fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

    // 硬件外设读 — 暴露给 plugin 读取真实引脚电平
    bool     (*gpio_get)(uint32_t pin);

    // 系统交互
    void     (*post_log)(const char *str);
    void     (*request_exit)(void);
    void     (*request_redraw)(void);   // plugin 状态变化时请求主系统下一帧全屏重画

    /* ===== UI 渲染组件 (Core 0 上下文,即 plugin draw() 内调用) =====
     * plugin 是 freestanding (-nostdlib -nodefaultlibs),不链 libuigeom.a,
     * 所有 UI 函数必须通过 host_api 函数指针调用,不能直接调 ui_* 符号。
     * update_core1() 严禁碰以下任何 UI 函数(会跟 Core 0 的 LCD DMA 抢总线)。 */
    void (*ui_draw_fill_rect)(ui_tile_t *tile, const ui_rect_t *rect, uint16_t color);
    void (*ui_draw_fast_hline)(ui_tile_t *tile, int16_t x, int16_t y, int16_t w, uint16_t color);
    void (*ui_draw_string)   (ui_tile_t *tile, int16_t x, int16_t y, const char *str,
                              FontSize_t size, uint16_t color, uint16_t bg);

    /* 控件:按钮(同样跑 Core 0,host 框架在 loader_app_process_touch 里调用
     * ui_btn_process_touch,在 loader_app_draw 里调用 ui_btn_draw) */
    void (*ui_btn_init)         (ui_btn_t *btn, int16_t x, int16_t y, int16_t w, int16_t h,
                                 const char *text, ui_btn_cb_t on_click);
    void (*ui_btn_set_color)    (ui_btn_t *btn, uint16_t bg, uint16_t active, uint16_t text);
    bool (*ui_btn_process_touch)(ui_btn_t *btn, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
    void (*ui_btn_draw)         (ui_tile_t *tile, const ui_btn_t *btn);

    /* ⚡ 末尾扩展槽:wire_quality 杜邦线测试仪需要主动驱动引脚
     * (TX 推 HIGH/LOW, RX 回读)。放在结构体末尾以保持现有 plugin
     * (led_controller) 的所有字段偏移不变 —— 老 .papp 仍然兼容。
     * ⚠️ 注意:host 必须在 s_host_api_table 里把这两个槽填上 NULL 或真实函数指针,
     *         老的 led_controller.papp 不会读到这几个槽 */
    void     (*gpio_set_dir)  (uint32_t pin, bool is_output);
    void     (*gpio_put)      (uint32_t pin, bool value);
    void     (*gpio_set_pulls)(uint32_t pin, bool up, bool down);  // RX 上拉,OPEN 判别

    /* ===== FATFS 访问槽:novel_reader 等需要打开 .txt 的 plugin
     * plugin 是 freestanding,不链 fatfs.a;通过 host 持有的 fd table 访问
     * 最多同时开 MAX_FAT_FILES 个文件 (host 内部定义)
     *
     * fat_open    返 0..MAX-1 = fd,负数 = 失败
     * fat_read    返读到的字节数,负数 = 失败
     * fat_seek    返 0 = ok,负数 = 失败
     * fat_size    返文件字节数,负数 = 失败
     * fat_close   关闭文件释放 fd
     * fat_list_dir 列 dir 下文件名(out_buf 每个文件名 \0 结尾,全部写完再加一个 \0),
     *              max_files 限制返回数量,返实际数,负数 = 失败
     *
     * 文件名过滤:list_dir 只返后缀 == ".txt" 的文件 (给小说阅读器用) */
    int  (*fat_open)(const char *path, uint8_t mode);
    int  (*fat_read)(int fd, uint8_t *buf, uint32_t len);
    int  (*fat_write)(int fd, const uint8_t *buf, uint32_t len);
    int  (*fat_seek)(int fd, uint32_t pos);
    int  (*fat_size)(int fd);
    void (*fat_close)(int fd);
    int  (*fat_list_dir)(const char *dir, char *out_buf, int out_buf_size, int max_files);
} pico_host_api_t;

/* ================= 动态 App 导出的生命周期接口 ================= */
typedef struct {
    uint32_t magic;
    uint32_t version;
    const char *name;

    bool (*init)(const pico_host_api_t *api);
    void (*update_core1)(void); // Core 1 独占主循环
    void (*process_touch)(bool is_touch, int16_t x, int16_t y);
    void (*draw)(ui_tile_t *tile);
    void (*exit)(void);
} app_plugin_t;

// 入口点：二进制偏移 0x00 处执行
typedef const app_plugin_t* (*app_entry_t)(const pico_host_api_t *api);

#ifdef __cplusplus
}
#endif

#endif // APP_API_H