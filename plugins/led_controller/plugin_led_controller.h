#ifndef PLUGIN_LED_CONTROLLER_H
#define PLUGIN_LED_CONTROLLER_H

#include "app_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// 获取本 plugin 的描述符单例
// host loader app_entry() 调到这里拿到 app_plugin_t*,
// 与 app_led_get_instance() 风格对齐
const app_plugin_t *led_controller_get_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif // PLUGIN_LED_CONTROLLER_H
