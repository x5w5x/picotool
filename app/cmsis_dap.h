#ifndef CMSIS_DAP_H
#define CMSIS_DAP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CMSIS-DAP 标准命令集[cite: 6, 14, 18]
#define ID_DAP_Info               0x00
#define ID_DAP_HostStatus         0x01
#define ID_DAP_Connect            0x02
#define ID_DAP_Disconnect         0x03
#define ID_DAP_TransferConfigure  0x04
#define ID_DAP_Transfer           0x05
#define ID_DAP_TransferBlock      0x06
#define ID_DAP_TransferAbort      0x07
#define ID_DAP_WriteABORT         0x08
#define ID_DAP_Delay              0x09
#define ID_DAP_ResetTarget        0x0A
#define ID_DAP_SWJ_Pins           0x10
#define ID_DAP_SWJ_Clock          0x11
#define ID_DAP_SWJ_Sequence       0x12
#define ID_DAP_SWD_Configure      0x13
#define ID_DAP_JTAG_Sequence      0x14
#define ID_DAP_JTAG_Configure     0x15
#define ID_DAP_JTAG_IDCODE        0x16

#define DAP_OK                    0x00
#define DAP_ERROR                 0xFF

#define DAP_INFO_VENDOR           0x01
#define DAP_INFO_PRODUCT          0x02
#define DAP_INFO_SER_NUM          0x03
#define DAP_INFO_FW_VER           0x04
#define DAP_INFO_CAPABILITIES     0xF0
#define DAP_INFO_PACKET_COUNT     0xFE
#define DAP_INFO_PACKET_SIZE      0xFF

// ⚡ 修复点：添加 volatile 保持与 .c 定义完全一致[cite: 14, 17, 18]
extern volatile bool     g_dap_enabled;
extern volatile uint32_t g_dap_packet_counter;

typedef enum {
    CMSIS_DAP_BACKEND_SOFTWARE = 0,
    CMSIS_DAP_BACKEND_PIO = 1
} cmsis_dap_backend_t;

void cmsis_dap_set_backend(cmsis_dap_backend_t backend);
cmsis_dap_backend_t cmsis_dap_get_backend(void);
extern volatile uint32_t g_dap_cur_clock_hz;

// 协议解析分发主函数[cite: 6, 14, 18]
uint32_t dap_process_command(const uint8_t *req, uint8_t *resp);

#ifdef __cplusplus
}
#endif

#endif // CMSIS_DAP_H
