/*
 * @Author: 轩
 * @Date: 2026-09-02 20:26:06
 * @LastEditTime: 2026-09-03 21:03:25
 * @FilePath: \pico\config.h
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_VERSION_STRING    "v1.0.0"
#define HW_VERSION_STRING    "Pico Board Rev A"
#define SDK_VERSION_STRING   "Pico SDK 2.2.0"
#define BOARD_FLASH_SIZE_BYTES (2u * 1024u * 1024u)

#define FW_BUILD_DATE        __DATE__
#define FW_BUILD_TIME        __TIME__
extern bool sys_led_blink;

#ifdef __cplusplus
}
#endif

#endif // CONFIG_H