#ifndef APP_LED_H
#define APP_LED_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// 获取 LED 控制 App 实例单例
app_t *app_led_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_LED_H