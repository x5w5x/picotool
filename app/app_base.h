/*
 * @Author: 轩
 * @Date: 2026-08-20 16:38:44
 * @LastEditTime: 2026-08-25 10:16:12
 * @FilePath: \pico\app\app_base.h
 */
#ifndef APP_BASE_H
#define APP_BASE_H

#include <stdint.h>
#include <stdbool.h>
#include "ui_tile.h"
#include "ui_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

// 应用唯一标识枚举
typedef enum {
    APP_ID_GPIO_MATRIX = 0, // 0: GPIO 矩阵 / LED 控制
    APP_ID_SETTINGS,        // 1: 系统设置
    APP_ID_LOGIC_LA,        // 2: 逻辑分析
    APP_ID_OSCILLOSCOPE,    // 3: 示波时序
    APP_ID_DAP_FLASHER,     // 4: 离线烧录
    APP_ID_UART_TERM,       // 5: 串口助手
    APP_ID_I2C_SNIFFER,     // 6: I2C 嗅探
    APP_ID_PWM_GEN,         // 7: PWM 发生
    APP_ID_HW_INFO,         // 8: 硬件自检
    APP_ID_MAX,
    APP_ID_CUSTOM_0
} app_id_t;

typedef struct app app_t;

struct app {
    app_id_t    id;
    const char *name;           // 应用全名 (如 "LED 控制器")
    const char *icon_symbol;    // 图标字符 (如 "LED", "PWM")
    uint16_t    icon_color;     // 图标底色 (RGB565)

    // 生命周期与渲染回调
    void (*init)(app_t *self);
    void (*enter)(app_t *self);
    void (*exit)(app_t *self);
    void (*update)(app_t *self, uint32_t now_tick);
    bool (*process_touch)(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick);
    void (*draw)(app_t *self, ui_tile_t *tile);

    void *user_data;
};

#ifdef __cplusplus
}
#endif

#endif // APP_BASE_H