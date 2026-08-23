#ifndef OS_PORT_H
#define OS_PORT_H

/*
 * RP2040 (Cortex-M0+) 平台适配层
 *
 * 移植要点:
 *  - RP2040 同样是 Cortex-M 系列,SCB->ICSR / SysTick 寄存器布局与
 *    STM32 完全一致,PendSV 触发位仍是 bit 28。
 *  - RP2040.h(CMSIS 设备头)提供 SCB / SysTick / NVIC /
 *    SysTick_Config / NVIC_SetPriority 等标准 CMSIS 接口,直接可用。
 *  - __clz 是 ARMv6-M 通用指令,GCC 内建可以直接用。
 *
 * 警告控制:
 *  - RP2040.h 中有大量 *_BASE 地址宏,会和 SDK 的 hardware_regs/
 *    addressmap.h 重定义。我们用 #ifndef 包裹,把 SDK 的版本当作权威
 *    版本(已经在 main 的 SDK include 链里被引入)。
 */

#include <stdint.h>

/* main.cpp 已经 include SDK 的 RP2040.h(通过 pico/stdlib.h 等),
 * 但 RTOS 子模块编译时不一定有此 include,所以需要 include。 */
#include "RP2040.h"

/* 计算位图最高优先级位索引(0..31),bitmap 非零 */
#define OS_PORT_GET_HIGHEST_PRI(bitmap) (31 - __builtin_clz((uint32_t)(bitmap)))

/* 触发 PendSV 异常,引发上下文切换 */
#define OS_PORT_YIELD() do { __DSB(); SCB->ICSR = SCB_ICSR_PENDSVSET_Msk; } while (0)

void os_port_systick_init(void);

#endif /* !OS_PORT_H */