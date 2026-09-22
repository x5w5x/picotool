#ifndef THUMB_H
#define THUMB_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t addr;
    uint16_t opcode16;
    bool     is_32bit;
    char     text[40]; // 解码出的汇编文本，如 "ldr r2, [r1, #0]"
} disasm_line_t;

uint8_t thumb_disasm_single(uint32_t addr, uint16_t hw1, uint16_t hw2, disasm_line_t *out);

#endif // !THUMB_H
