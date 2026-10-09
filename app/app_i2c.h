/*
 * @Author: 轩
 * @Date: 2026-10-09 14:49:35
 * @LastEditTime: 2026-10-09 14:49:42
 * @FilePath: \pico\app\app_i2c.h
 */
#ifndef APP_I2C_H
#define APP_I2C_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

app_t *app_i2c_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_I2C_H