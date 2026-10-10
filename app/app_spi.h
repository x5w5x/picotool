/*
 * @Author: 轩
 * @Date: 2026-10-09 19:36:43
 * @LastEditTime: 2026-10-09 19:36:49
 * @FilePath: \pico\app\app_spi.h
 */
#ifndef APP_SPI_H
#define APP_SPI_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

app_t *app_spi_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_SPI_H