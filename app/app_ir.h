/*
 * @Author: 轩
 * @Date: 2026-10-06 15:22:59
 * @LastEditTime: 2026-10-06 15:23:05
 * @FilePath: \pico\app\app_ir.h
 */
#ifndef APP_IR_H
#define APP_IR_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// 导出 App 实例单例，供 app_manager 注册使用
app_t *app_ir_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_IR_H