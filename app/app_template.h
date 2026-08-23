/*
 * @Author: 轩
 * @Date: 2026-08-23 10:49:13
 * @LastEditTime: 2026-08-23 10:49:19
 * @FilePath: \pico\app\app_template.h
 */
#ifndef APP_TEMPLATE_H
#define APP_TEMPLATE_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取当前 App 单例实例指针
 */
app_t *app_template_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_TEMPLATE_H