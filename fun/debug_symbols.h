#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t addr;
    uint16_t size;
    const char *name;
} debug_symbol_t;

#define DEBUG_SYMBOL_COUNT 31
extern const debug_symbol_t g_debug_symbols[DEBUG_SYMBOL_COUNT];

// 根据 PC 地址二分查找所属函数名与指令偏移
const char *debug_symbol_find(uint32_t pc, uint32_t *offset);
