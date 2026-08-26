/*
 * @Author: 轩
 * @Date: 2026-08-26 12:32:14
 * @LastEditTime: 2026-08-26 14:45:50
 * @FilePath: \pico\app\tusb_config.h
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// --- 基础硬件平台配置 ---
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU             OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS              OPT_OS_NONE
#endif

#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE   64

// ⚡ 核心：同时启用 1 个 HID 与 1 个 CDC
#define CFG_TUD_HID              1
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              1
#define CFG_TUD_MSC_EP_BUFSIZE  512
// HID 报表缓冲区大小 (标准 64 字节包)
#define CFG_TUD_HID_EP_BUFSIZE   64

// CDC 串口缓冲区大小
#define CFG_TUD_CDC_EP_BUFSIZE   64
#define CFG_TUD_CDC_RX_BUFSIZE   512
#define CFG_TUD_CDC_TX_BUFSIZE   512

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_