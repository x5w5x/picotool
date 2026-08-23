/*
 * os_port.s -- RP2040 (Cortex-M0+) PendSV 上下文切换
 *
 * 栈布局(与 STM32 版本一致):
 *   sp[0..3]   = R4..R7 saved
 *   sp[4..7]   = R8..R11 saved
 *   sp[8..15]  = R0..xPSR(硬件异常自动压栈)
 *
 * 保存顺序(基于 Gemini 的方案,栈布局反向:R8-R11 在低地址 sp[0..3],
 * R4-R7 在高地址 sp[4..7],但只要 save/load 镜像一致就 OK):
 *   1) 把当前 R4-R7 暂存到 sp[4..7](覆盖 R8-R11 位置)
 *   2) 把 R8-R11 搬到 R4-R7
 *   3) 把 R4-R7(=R8-R11) 写到 sp[0..3]
 *   最终 stack_ptr = sp[0](R8-R11 saved 位置)
 *
 * 加载顺序:
 *   1) LDMIA R0!, {r4-r7}   ← 读 sp[0..3] = R8-R11 saved
 *   2) MOV r8..r11 = r4..r7  ← 恢复到 R8-R11
 *   3) LDMIA R0!, {r4-r7}   ← 读 sp[4..7] = R4-R7 saved
 *   4) R0 此时 = sp[8],MSR PSP, r0
 *
 * M0+ 立即数限制 + 对应处理:
 *   - SUB/ADD 立即数 imm3 仅 0..7
 *   - 没有 STMDB / POP {lr} 16 位编码
 *   - R8-R11 必须先 MOV 到 R4-R7 再 STMIA
 *   - EXC_RETURN 用 LDR 字面池 + mov lr, Rn(SDK 已验证 16 位)
 */

    .syntax unified
    .cpu    cortex-m0plus
    .thumb

    /* 关键:SDK vector table 引用 isr_pendsv/isr_systick(不是
     * PendSV_Handler/SysTick_Handler)。SDK 默认 weak 实现是 BKPT,
     * 触发异常会死锁。 */
    .global PendSV_Handler
    .global isr_pendsv

    .type   PendSV_Handler, %function
    .type   isr_pendsv, %function

isr_pendsv:
PendSV_Handler:
    /* 1) 取 PSP */
    mrs     r0, psp
    cmp     r0, #0
    beq     Switch_Task          /* PSP==0 跳过保存 */

    /* 2) R0 -= 16 → sp[4](R8-R11 位置) */
    subs    r0, r0, #7
    subs    r0, r0, #7
    subs    r0, r0, #2

    /* 3) 把当前 R4-R7 暂存到 sp[4..7] */
    stmia   r0!, { r4-r7 }       /* R0 += 16 → sp[8] */

    /* 4) R8-R11 → R4-R7 */
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11

    /* 5) R0 -= 32 → sp[0](R4 位置;M0+ SUB imm3 限制累计) */
    subs    r0, r0, #7
    subs    r0, r0, #7
    subs    r0, r0, #7
    subs    r0, r0, #7
    subs    r0, r0, #4

    /* 6) 把 R4-R7(=R8-R11)写到 sp[0..3] */
    stmia   r0!, { r4-r7 }       /* R0 += 16 → sp[4] */

    /* 7) R0 -= 16 → sp[0] 保存 stack_ptr */
    subs    r0, r0, #7
    subs    r0, r0, #7
    subs    r0, r0, #2

    /* 8) 保存 stack_ptr 到当前 TCB */
    ldr     r1, =os_current_task
    ldr     r2, [ r1 ]
    str     r0, [ r2 ]

Switch_Task:
    /* 9) os_current_task = os_next_task */
    ldr     r1, =os_current_task
    ldr     r2, =os_next_task
    ldr     r3, [ r2 ]
    str     r3, [ r1 ]

    /* 10) R0 = 新任务的 stack_ptr */
    ldr     r0, [ r3 ]           /* R0 = new_tcb->stack_ptr = sp[0] */

    /* 11) 读 sp[0..3] = R8-R11 saved */
    ldmia   r0!, { r4-r7 }       /* R0 += 16 → sp[4] */

    /* 12) 恢复 R8-R11 */
    mov     r8,  r4
    mov     r9,  r5
    mov     r10, r6
    mov     r11, r7

    /* 13) 读 sp[4..7] = R4-R7 saved */
    ldmia   r0!, { r4-r7 }       /* R0 += 16 → sp[8] */

    /* 14) PSP = R0 = sp[8](硬件栈起点) */
    msr     psp, r0

    /* 15) EXC_RETURN → LR,然后 bx lr 触发异常返回。
     *    M0+ 上 `bx lr` 把 LR 加载到 PC。处理器看到 PC = 0xFFFFFFFD
     *    时识别 EXC_RETURN 并执行异常返回序列。 */
    ldr     r0, =os_exc_return
    ldr     r1, [ r0 ]
    push    { r1 }                  /* R1 → MSP 栈顶 */
    pop     { pc }                  /* 0xFFFFFFFD → PC → EXC_RETURN */

    .size   PendSV_Handler, .-PendSV_Handler
    .size   isr_pendsv, .-isr_pendsv

    .end