/*
 * @Author: 轩
 * @Date: 2026-08-24 13:13:52
 * @LastEditTime: 2026-08-25 11:09:38
 * @FilePath: \pico\app\app_uart_term.h
 */
#ifndef APP_UART_TERM_H
#define APP_UART_TERM_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取基于 TinyUSB CDC 与硬件 DMA 双核架构的串口助手 App 单例
 */
app_t *app_uart_term_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_UART_TERM_H