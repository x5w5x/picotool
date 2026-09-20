/*
 * @Author: 轩
 * @Date: 2026-09-02 20:26:06
 * @LastEditTime: 2026-09-18 19:33:03
 * @FilePath: \pico\config.h
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

#define FW_VERSION_STRING "v1.1.3"
#define HW_VERSION_STRING "Pico Board Rev A"
#define SDK_VERSION_STRING "Pico SDK 2.2.0"
#define BOARD_FLASH_SIZE_BYTES (2u * 1024u * 1024u)

#define FW_BUILD_DATE __DATE__
#define FW_BUILD_TIME __TIME__

    extern bool sys_led_blink; // 系统灯
    extern uint8_t led_brigthness;
#ifdef __cplusplus
}
#endif

#endif // CONFIG_H
