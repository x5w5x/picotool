#include "os_port.h"
#include "os_core.h"
#include "RP2040.h"

/*
 * 1ms tick 初始化
 */
void os_port_systick_init(void)
{
    SysTick_Config(125000);
}

/*
 * SDK vector table 引用 isr_pendsv/isr_systick(不是 PendSV_Handler /
 * SysTick_Handler)。SDK 默认 weak 是 BKPT,触发异常 → 死锁。
 *
 * 关键:汇编里 .global + 函数定义,但 GNU 链接器仍然优先使用 SDK 的
 * weak 默认。所以我们在 C 中显式 alias,这样 alias 自动产生 strong
 * 引用,强迫链接器拉取我们的汇编实现并保留 PendSV_Handler 段不被 gc。
 */

void isr_systick(void)
{
    os_tick_handler();
}

/* 让 SysTick_Handler = isr_systick(CMSIS 标准名映射) */
void SysTick_Handler(void) __attribute__((alias("isr_systick")));

/* 让 isr_pendsv = PendSV_Handler(Pico SDK 的 vector table 名映射)
 * 由 os_port.s 中 .global isr_pendsv + .global PendSV_Handler
 * 两个同名 label 实现。这里不再重复定义。 */