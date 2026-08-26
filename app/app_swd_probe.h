#ifndef APP_SWD_PROBE_H
#define APP_SWD_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "app_manager.h"

app_t *app_swd_probe_get_instance(void);

/* SWD 底层调试接口(供 swd_flasher.c 调用) */
void     Target_Write(uint32_t addr, uint32_t val);
void     Target_WriteHalf(uint32_t addr, uint16_t val16);  /* STM32F1 flash 编程专用 */
uint32_t Target_Read(uint32_t addr);
uint8_t  target_get_write_ack(void);  /* 最近一次 Target_Write 的 SWD ack:1=OK,2=WAIT,4=FAULT */
uint8_t  target_get_read_ack(void);   /* 最近一次 Target_Read 的 SWD ack */
void     target_set_swd_delay(uint8_t d);  /* 设置 SWD 半位延时(默认 2,慢用 10-25) */
bool     target_swd_init(void);            /* SWD 总线初始化(LINE RESET + POWER UP),返回是否连上目标 */

#ifdef __cplusplus
}
#endif

#endif // APP_SWD_PROBE_H