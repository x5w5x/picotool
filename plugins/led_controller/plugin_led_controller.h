/* =============================================================
 * @file   plugins/led_controller/plugin_led_controller.h
 * @brief  LED Controller plugin 描述符头 (唯一对外符号声明)
 *
 * @section 角色
 *   - plugins/led_controller 插件对外公共头
 *   - host loader 通过 main.c → led_controller_get_descriptor() 拿到
 *     const app_plugin_t* (s_led_controller_plugin)
 *   - 与 app_led.h 风格对齐:只有 descriptor getter 一个符号
 *
 * @section 导出符号
 *   - led_controller_get_descriptor (返回 const app_plugin_t*)
 *
 * @section 依赖
 *   "app_api.h" (引出 app_plugin_t / APP_MAGIC)
 *
 * @section 待重构 (P2)
 *   - 三个 plugin 的 .h 都是单声明 header,完全一致,模板化意义不大,
 *     保留即可;但若以后插件需要动态配置,这里应加 plugin_get_config()
 * ============================================================= */
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
