// /*
//  * @Author: 轩
//  * @Date: 2026-09-22 20:04:20
//  * @LastEditTime: 2026-09-22 21:05:01
//  * @FilePath: \pico\fun\thumb_disasm.c
//  */#include "thumb_disasm.h"
// #include <stdio.h>

// static const char *regs[] = {
//     "r0","r1","r2","r3","r4","r5","r6","r7",
//     "r8","r9","r10","r11","r12","sp","lr","pc"
// };
// static uint32_t thumb_expand_imm(uint8_t i, uint8_t imm3, uint8_t imm8)
// {
//     uint8_t ctrl = (i << 3) | imm3;
//     if ((ctrl & 0x0C) == 0) {
//         switch (ctrl & 0x03) {
//             case 0: return imm8;
//             case 1: return ((uint32_t)imm8 << 16) | imm8;
//             case 2: return ((uint32_t)imm8 << 24) | ((uint32_t)imm8 << 8);
//             case 3: return ((uint32_t)imm8 << 24) | ((uint32_t)imm8 << 16) | ((uint32_t)imm8 << 8) | imm8;
//         }
//     }
//     uint32_t unrotated = 0x80u | (imm8 & 0x7Fu);
//     uint8_t rot = ((ctrl << 1) | (imm8 >> 7)) & 0x1Fu;
//     return (unrotated >> rot) | (unrotated << (32 - rot));
// }

// uint8_t thumb_disasm_single(uint32_t addr, uint16_t hw1, uint16_t hw2, disasm_line_t *out)
// {
//     out->addr = addr;
//     out->opcode16 = hw1;
//     out->is_32bit = false;
//     if ((hw1 & 0xF800) == 0xF000 || (hw1 & 0xF800) == 0xF800 || (hw1 & 0xE800) == 0xE800) {
//         out->is_32bit = true;
//         if ((hw1 & 0xF800) == 0xF000 && ((hw2 & 0xD000) == 0xD000 || (hw2 & 0xD000) == 0xC000)) {
//             int32_t s = (hw1 >> 10) & 1;
//             int32_t j1 = (hw2 >> 13) & 1;
//             int32_t j2 = (hw2 >> 11) & 1;
//             int32_t imm10 = hw1 & 0x03FF;
//             int32_t imm11 = hw2 & 0x07FF;
//             int32_t i1 = !(j1 ^ s);
//             int32_t i2 = !(j2 ^ s);
//             int32_t offset = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
//             if (s) offset |= 0xFE000000;
//             uint32_t target = addr + 4 + offset;
//             snprintf(out->text, sizeof(out->text), "bl   0x%08lX", (unsigned long)target);
//             return 4;
//         }
//         if ((hw1 & 0xFBFF) == 0xF04F && (hw2 & 0x8000) == 0) {
//             uint8_t rd = (hw2 >> 8) & 0x0F;
//             uint8_t i = (hw1 >> 10) & 1;
//             uint8_t imm3 = (hw2 >> 12) & 7;
//             uint8_t imm8 = hw2 & 0xFF;
//             uint32_t imm_val = thumb_expand_imm(i, imm3, imm8);
//             if (imm_val <= 9) {
//                 snprintf(out->text, sizeof(out->text), "mov  %s, #%lu", regs[rd], (unsigned long)imm_val);
//             } else {
//                 snprintf(out->text, sizeof(out->text), "mov  %s, #0x%lX", regs[rd], (unsigned long)imm_val);
//             }
//             return 4;
//         }
//         snprintf(out->text, sizeof(out->text), ".word 0x%04X%04X", hw1, hw2);
//         return 4;
//     }
//     if (hw1 == 0xBF00) {
//         snprintf(out->text, sizeof(out->text), "nop");
//         return 2;
//     }
//     if ((hw1 & 0xFF00) == 0x4700) {
//         uint8_t rm = (hw1 >> 3) & 0xF;
//         bool l = (hw1 >> 7) & 1;
//         snprintf(out->text, sizeof(out->text), "%s  %s", l ? "blx" : "bx", regs[rm]);
//         return 2;
//     }
//     if ((hw1 & 0xFE00) == 0xB400) {
//         bool pop = (hw1 >> 11) & 1;
//         snprintf(out->text, sizeof(out->text), "%s {regs}", pop ? "pop " : "push");
//         return 2;
//     }
//     if ((hw1 & 0xF800) == 0xE000) {
//         int32_t imm11 = hw1 & 0x07FF;
//         if (imm11 & 0x0400) imm11 |= 0xFFFFF800;
//         uint32_t target = addr + 4 + (imm11 * 2);
//         snprintf(out->text, sizeof(out->text), "b.n  0x%08lX", (unsigned long)target);
//         return 2;
//     }
//     if ((hw1 & 0xF000) == 0xD000 && (hw1 & 0x0E00) != 0x0E00) {
//         int8_t imm8 = (int8_t)(hw1 & 0xFF);
//         uint32_t target = addr + 4 + (imm8 * 2);
//         snprintf(out->text, sizeof(out->text), "b<c> 0x%08lX", (unsigned long)target);
//         return 2;
//     }
//     if ((hw1 & 0xF800) == 0x4800) {
//         uint8_t rd = (hw1 >> 8) & 0x7;
//         uint8_t imm8 = hw1 & 0xFF;
//         snprintf(out->text, sizeof(out->text), "ldr  %s, [pc, #%d]", regs[rd], imm8 * 4);
//         return 2;
//     }
//     if ((hw1 & 0xF000) == 0x6000) {
//         bool ldr = (hw1 >> 11) & 1;
//         uint8_t rd = hw1 & 0x7;
//         uint8_t rn = (hw1 >> 3) & 0x7;
//         uint8_t imm5 = (hw1 >> 6) & 0x1F;
//         snprintf(out->text, sizeof(out->text), "%s  %s, [%s, #%d]", ldr ? "ldr" : "str", regs[rd], regs[rn], imm5 * 4);
//         return 2;
//     }
//     if ((hw1 & 0xF800) == 0x2000) {
//         uint8_t rd = (hw1 >> 8) & 0x7;
//         uint8_t imm8 = hw1 & 0xFF;
//         snprintf(out->text, sizeof(out->text), "movs %s, #%d", regs[rd], imm8);
//         return 2;
//     }

//     snprintf(out->text, sizeof(out->text), ".short 0x%04X", hw1);
//     return 2;
// }
#include "thumb_disasm.h"
#include "sym_loader.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// 常用通用寄存器名称
static const char *regs[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
};

// 常见条件码后缀 (0~13)
static const char *cond_names[] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le"
};

// ARM Thumb-2 修改型立即数标准算法 (用于还原 MOV.W 的常数)
static uint32_t thumb_expand_imm(uint8_t i, uint8_t imm3, uint8_t imm8)
{
    uint8_t ctrl = (i << 3) | imm3;
    if ((ctrl & 0x0C) == 0) {
        switch (ctrl & 0x03) {
            case 0: return imm8;
            case 1: return ((uint32_t)imm8 << 16) | imm8;
            case 2: return ((uint32_t)imm8 << 24) | ((uint32_t)imm8 << 8);
            case 3: return ((uint32_t)imm8 << 24) | ((uint32_t)imm8 << 16) | ((uint32_t)imm8 << 8) | imm8;
        }
    }
    uint32_t unrotated = 0x80u | (imm8 & 0x7Fu);
    uint8_t rot = ((ctrl << 1) | (imm8 >> 7)) & 0x1Fu;
    return (unrotated >> rot) | (unrotated << (32 - rot));
}

uint8_t thumb_disasm_single(uint32_t addr, uint16_t hw1, uint16_t hw2, disasm_line_t *out)
{
    if (!out) return 2;

    out->addr = addr;
    out->opcode16 = hw1;
    out->is_32bit = false;
    if ((hw1 & 0xF800) == 0xF000 || (hw1 & 0xF800) == 0xF800 || (hw1 & 0xE800) == 0xE800) {
        out->is_32bit = true;
        if ((hw1 & 0xF800) == 0xF000 && ((hw2 & 0xD000) == 0xD000 || (hw2 & 0xD000) == 0xC000)) {
            int32_t s = (hw1 >> 10) & 1;
            int32_t j1 = (hw2 >> 13) & 1;
            int32_t j2 = (hw2 >> 11) & 1;
            int32_t imm10 = hw1 & 0x03FF;
            int32_t imm11 = hw2 & 0x07FF;
            int32_t i1 = !(j1 ^ s);
            int32_t i2 = !(j2 ^ s);
            int32_t offset = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
            if (s) offset |= 0xFE000000;
            uint32_t target = addr + 4 + offset;
            uint32_t sym_offset = 0;
            const char *fn_name = sym_loader_find_func(target, &sym_offset);

            if (fn_name && sym_offset == 0) {
                // snprintf(out->text, sizeof(out->text), "bl %s(0x%08lX)", fn_name, (unsigned long)target);
                snprintf(out->text, sizeof(out->text), "bl %s", fn_name, (unsigned long)target);
            } else if (fn_name && sym_offset > 0) {
                snprintf(out->text, sizeof(out->text), "bl %s+0x%lX(0x%08lX)", fn_name, (unsigned long)sym_offset, (unsigned long)target);
            } else {
                snprintf(out->text, sizeof(out->text), "bl 0x%08lX", (unsigned long)target);
            }
            return 4;
        }
        if ((hw1 & 0xFBFF) == 0xF04F && (hw2 & 0x8000) == 0) {
            uint8_t rd = (hw2 >> 8) & 0x0F;
            uint8_t i = (hw1 >> 10) & 1;
            uint8_t imm3 = (hw2 >> 12) & 7;
            uint8_t imm8 = hw2 & 0xFF;
            uint32_t imm_val = thumb_expand_imm(i, imm3, imm8);

            if (imm_val <= 9) {
                snprintf(out->text, sizeof(out->text), "mov  %s, #%lu", regs[rd], (unsigned long)imm_val);
            } else {
                snprintf(out->text, sizeof(out->text), "mov  %s, #0x%lX", regs[rd], (unsigned long)imm_val);
            }
            return 4;
        }
        snprintf(out->text, sizeof(out->text), ".word 0x%04X%04X", hw1, hw2);
        return 4;
    }

    // ========================================================================
    // 2. 16 位 Thumb 指令判断
    // ========================================================================

    // --- 2.1 空指令: NOP ---
    if (hw1 == 0xBF00) {
        snprintf(out->text, sizeof(out->text), "nop");
        return 2;
    }

    // --- 2.2 寄存器跳转 / 调用: BX / BLX Rm ---
    if ((hw1 & 0xFF00) == 0x4700) {
        uint8_t rm = (hw1 >> 3) & 0xF;
        bool l = (hw1 >> 7) & 1;
        snprintf(out->text, sizeof(out->text), "%s  %s", l ? "blx" : "bx", regs[rm]);
        return 2;
    }

    // --- 2.3 压栈与出栈: PUSH / POP ---
    if ((hw1 & 0xFE00) == 0xB400 || (hw1 & 0xFE00) == 0xBC00) {
        bool pop = (hw1 & 0x0800) != 0;
        snprintf(out->text, sizeof(out->text), "%s {regs}", pop ? "pop " : "push");
        return 2;
    }

    // --- 2.4 无条件相对跳转: B.N <label> ---
    if ((hw1 & 0xF800) == 0xE000) {
        int32_t imm11 = hw1 & 0x07FF;
        if (imm11 & 0x0400) imm11 |= 0xFFFFF800; // 符号扩展
        uint32_t target = addr + 4 + (imm11 * 2);
        snprintf(out->text, sizeof(out->text), "b.n  0x%08lX", (unsigned long)target);
        return 2;
    }

    // --- 2.5 条件相对跳转: B<cond> <label> ---
    if ((hw1 & 0xF000) == 0xD000 && (hw1 & 0x0E00) != 0x0E00) {
        uint8_t cond = (hw1 >> 8) & 0x0F;
        int8_t imm8 = (int8_t)(hw1 & 0xFF);
        uint32_t target = addr + 4 + (imm8 * 2);

        if (cond < 14) {
            snprintf(out->text, sizeof(out->text), "b%-3s 0x%08lX", cond_names[cond], (unsigned long)target);
        } else {
            snprintf(out->text, sizeof(out->text), "b<c> 0x%08lX", (unsigned long)target);
        }
        return 2;
    }

    // --- 2.6 PC 相对寻址读取: LDR Rd, [PC, #imm] ---
    if ((hw1 & 0xF800) == 0x4800) {
        uint8_t rd = (hw1 >> 8) & 0x7;
        uint8_t imm8 = hw1 & 0xFF;
        snprintf(out->text, sizeof(out->text), "ldr  %s, [pc, #%d]", regs[rd], imm8 * 4);
        return 2;
    }

    // --- 2.7 基址偏移读写: LDR / STR Rd, [Rn, #imm5] ---
    if ((hw1 & 0xF000) == 0x6000) {
        bool ldr = (hw1 >> 11) & 1;
        uint8_t rd = hw1 & 0x7;
        uint8_t rn = (hw1 >> 3) & 0x7;
        uint8_t imm5 = (hw1 >> 6) & 0x1F;
        snprintf(out->text, sizeof(out->text), "%s  %s, [%s, #%d]",
                 ldr ? "ldr" : "str", regs[rd], regs[rn], imm5 * 4);
        return 2;
    }

    // --- 2.8 8 位立即数移动: MOVS Rd, #imm8 ---
    if ((hw1 & 0xF800) == 0x2000) {
        uint8_t rd = (hw1 >> 8) & 0x7;
        uint8_t imm8 = hw1 & 0xFF;
        snprintf(out->text, sizeof(out->text), "movs %s, #%d", regs[rd], imm8);
        return 2;
    }

    // --- 2.9 寄存器加减法: ADDS / SUBS Rd, Rn, Rm/imm3 ---
    if ((hw1 & 0xFC00) == 0x1800 || (hw1 & 0xFC00) == 0x1A00) {
        bool sub = (hw1 >> 9) & 1;
        uint8_t rd = hw1 & 0x7;
        uint8_t rn = (hw1 >> 3) & 0x7;
        uint8_t rm = (hw1 >> 6) & 0x7;
        snprintf(out->text, sizeof(out->text), "%s %s, %s, %s",
                 sub ? "subs" : "adds", regs[rd], regs[rn], regs[rm]);
        return 2;
    }

    // 16 位未匹配指令兜底显示半字机器码
    snprintf(out->text, sizeof(out->text), ".short 0x%04X", hw1);
    return 2;
}