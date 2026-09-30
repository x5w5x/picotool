/*
 * @Author: 轩
 * @Date: 2026-09-06 13:48:06
 * @LastEditTime: 2026-09-30 18:25:34
 * @FilePath: \pico\app\app_uart.h
 */
#ifndef APP_UART_H
#define APP_UART_H

#include "app_base.h"
#include "fun_uart_profile.h"
#ifdef __cplusplus
extern "C" {
#endif

app_t *app_uart_get_instance(void);
const fun_uart_profile_t *app_uart_get_profile(void);
#ifdef __cplusplus
}
#endif

#endif
