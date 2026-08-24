#ifndef APP_UART_TERM_H
#define APP_UART_TERM_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取基于 SDK 原生 stdio 与双核架构的串口助手 App 单例
 */
app_t *app_uart_term_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_UART_TERM_H