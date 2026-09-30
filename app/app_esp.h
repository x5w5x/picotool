/*
 * @Author: 轩
 * @Date: 2026-09-30 15:34:29
 * @LastEditTime: 2026-09-30 15:38:38
 * @FilePath: \pico\app\app_esp.h
 */
#ifndef APP_ESP_H
#define APP_ESP_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// 获取 ESP 调试应用单例
app_t *app_esp_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_ESP_H