/* =============================================================
 * @file   app/tusb_config.h
 * @brief  TinyUSB 编译配置:HID + CDC + MSC 三类接口全开
 *
 * @section 角色
 *   - 项目级 TinyUSB 配置,被 usb_descriptors.c / cmsis_dap.c / msc_disk.c 共同依赖
 *   - MCU 选 OPT_MCU_RP2040,OS 选 OPT_OS_NONE(裸机)
 *   - Endpoint 0 = 64(USB 2.0 FS 全速)
 *   - HID(1 个,DAP 通道) + CDC(1 个,串口) + MSC(1 个,虚拟 U 盘)
 *   - EP 缓冲:HID 64 / CDC 64 / CDC RX/TX 512 / MSC 512
 *
 * @section 导出符号
 *   - CFG_TUSB_MCU / CFG_TUSB_OS / CFG_TUSB_RHPORT0_MODE
 *   - CFG_TUD_HID / CDC / MSC / *_EP_BUFSIZE / *_RX_BUFSIZE / *_TX_BUFSIZE
 *
 * @section 依赖
 *   - tinyusb 公共头 / Pico SDK
 *
 * @section 备注
 *   - 注释里标了"核心:同时启用 1 个 HID 与 1 个 CDC" — 这只是 v1,
 *     当前版本已加 MSC
 * ============================================================= */
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