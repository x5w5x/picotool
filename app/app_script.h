/*
 * @Author: 轩
 * @Date: 2026-10-06 14:17:54
 * @LastEditTime: 2026-10-06 14:18:05
 * @FilePath: \pico\app\app_script.h
 */
#ifndef APP_SCRIPT_H
#define APP_SCRIPT_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// 导出 App 实例入口，供 app_manager 注册使用
app_t *app_script_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_SCRIPT_H