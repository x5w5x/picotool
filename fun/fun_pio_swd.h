#ifndef FUN_PIO_SWD_H
#define FUN_PIO_SWD_H

#include <stdbool.h>
#include <stdint.h>
#include "pio_manager.h"

#define SWD_DHCSR   0xE000EDF0u
#define SWD_DCRSR   0xE000EDF4u
#define SWD_DCRDR   0xE000EDF8u
#define SWD_DEMCR   0xE000EDFCu
#define SWD_AIRCR   0xE000ED0Cu
//DHCSR控制位
#define DHCSR_DBGKEY       0xA05F0000u

#define FP_CTRL            0xE0002000u  
#define FP_REMAP           0xE0002004u  
#define FP_COMP0           0xE0002008u  
#define FP_CTRL_KEY        (1u << 1)     
#define FP_CTRL_ENABLE     (1u << 0)    

#define DHCSR_C_DEBUGEN    (1u << 0)
#define DHCSR_C_HALT       (1u << 1)
#define DHCSR_C_STEP       (1u << 2)
#define DHCSR_C_MASKINTS   (1u << 3)

#define DHCSR_S_REGRDY     (1u << 16)
#define DHCSR_S_HALT       (1u << 17)
#define DHCSR_S_SLEEP      (1u << 18)
#define DHCSR_S_LOCKUP     (1u << 19)
#define DHCSR_S_RETIRE_ST  (1u << 24)
#define DHCSR_S_RESET_ST   (1u << 25)


typedef struct {
    bool opened;
    pio_manager_t *manager;
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
    uint offset;
    uint8_t swclk_gpio;
    uint8_t swdio_gpio;
    uint32_t clock_hz;
    bool write_opened;
    PIO write_pio;
    uint8_t write_pio_index;
    uint8_t write_sm;
    uint write_offset;
    uint8_t last_write_ack;
    uint8_t power_stage;
    bool mem_select_valid;
    bool mem_csw_valid;
    bool mem_tar_valid;
    uint32_t mem_csw;
    uint32_t mem_tar;
} fun_pio_swd_t;

void fun_pio_swd_init(fun_pio_swd_t *swd);
bool fun_pio_swd_open_auto(fun_pio_swd_t *swd, pio_manager_t *manager,
                           uint8_t swclk_gpio, uint8_t swdio_gpio,
                           uint32_t clock_hz);
bool fun_pio_swd_open_session(fun_pio_swd_t *swd, pio_manager_t *manager,
                              uint8_t swclk_gpio, uint8_t swdio_gpio,
                              uint32_t clock_hz, bool line_reset);
/* 不释放PIO资源，重新发送SWD切换序列并重启状态机。 */
bool fun_pio_swd_recover(fun_pio_swd_t *swd);
void fun_pio_swd_close(fun_pio_swd_t *swd);
/* 运行时调整 SWCLK 频率；返回 false 表示句柄无效或频率为 0。 */
bool fun_pio_swd_set_clock(fun_pio_swd_t *swd, uint32_t clock_hz);
uint8_t fun_pio_swd_transfer(fun_pio_swd_t *swd, uint8_t request,uint32_t *data);
bool fun_pio_swd_read_dp_idcode(fun_pio_swd_t *swd, uint32_t *idcode,uint8_t *ack);
bool fun_pio_swd_read_dp_register(fun_pio_swd_t *swd, uint8_t address,uint32_t *value, uint8_t *ack);
bool fun_pio_swd_read_register(fun_pio_swd_t *swd, bool ap, uint8_t address,uint32_t *value, uint8_t *ack);
bool fun_pio_swd_write_dp_register(fun_pio_swd_t *swd, uint8_t address,uint32_t value, uint8_t *ack);
bool fun_pio_swd_write_register(fun_pio_swd_t *swd, bool ap, uint8_t address,uint32_t value, uint8_t *ack);
bool fun_pio_swd_power_up(fun_pio_swd_t *swd);
bool fun_pio_swd_read_memory32(fun_pio_swd_t *swd, uint32_t address,uint32_t *value);
bool fun_pio_swd_write_memory32(fun_pio_swd_t *swd, uint32_t address,uint32_t value);
bool fun_pio_swd_write_memory16(fun_pio_swd_t *swd, uint32_t address,uint16_t value);
bool fun_pio_swd_write_memory_block32(fun_pio_swd_t *swd, uint32_t address,const uint8_t *data, uint32_t length);
//调试器
bool fun_pio_swd_is_halted(fun_pio_swd_t *swd, bool *halted);
bool fun_pio_swd_halt_target(fun_pio_swd_t *swd, uint32_t timeout_ms);
bool fun_pio_swd_run_target(fun_pio_swd_t *swd);
bool fun_pio_swd_step_target(fun_pio_swd_t *swd, uint32_t timeout_ms);
bool fun_pio_swd_reset_target(fun_pio_swd_t *swd);
bool fun_pio_swd_read_core_reg(fun_pio_swd_t *swd, uint8_t reg_id, uint32_t *value);
bool fun_pio_swd_write_core_reg(fun_pio_swd_t *swd, uint8_t reg_id, uint32_t value);
bool fun_pio_swd_read_memory_block32(fun_pio_swd_t *swd, uint32_t address,uint32_t *data, uint32_t word_count);

bool fun_pio_swd_fpb_init(fun_pio_swd_t *swd);
bool fun_pio_swd_set_breakpoint(fun_pio_swd_t *swd, uint32_t flash_addr);
bool fun_pio_swd_clear_breakpoint(fun_pio_swd_t *swd);

bool fun_pio_swd_step_over(fun_pio_swd_t *swd, uint32_t timeout_ms);
bool fun_pio_swd_step_out(fun_pio_swd_t *swd, uint32_t timeout_ms);
#endif
