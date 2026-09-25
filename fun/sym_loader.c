/*
 * @Author: 轩
 * @Date: 2026-09-23 14:54:47
 * @LastEditTime: 2026-09-24 21:35:55
 * @FilePath: \pico\fun\sym_loader.c
 */
#include "sym_loader.h"
#include "ff.h"
#include <string.h>
#include "app_loader.h"
#include <stdio.h>

#define SYM_BUFFER_MAX_SIZE (16 * 1024) 

static uint8_t *s_sym_buffer = NULL;
static sym_header_t *s_header = NULL;
static sym_func_entry_t *s_funcs = NULL;
static sym_var_entry_t *s_vars = NULL;
static char *s_str_pool = NULL;

void sym_loader_init(void)
{
    //零堆分配，直接借用全局 48KB 动态 App 内存池
    s_sym_buffer = app_loader_get_ram_pool();
    sym_loader_free();
}

void sym_loader_free(void)
{
    s_header = NULL;
    s_funcs = NULL;
    s_vars = NULL;
    s_str_pool = NULL;
}

bool sym_loader_load_from_sd(const char *filepath)
{
    if (!s_sym_buffer || !filepath) return false;

    FIL fil;
    if (f_open(&fil, filepath, FA_READ) != FR_OK) {
        return false;
    }

    UINT bytes_read = 0;
    f_read(&fil, s_sym_buffer, SYM_BUFFER_MAX_SIZE, &bytes_read);
    f_close(&fil);

    if (bytes_read < sizeof(sym_header_t)) {
        sym_loader_free();
        return false;
    }

    s_header = (sym_header_t *)s_sym_buffer;
    if (strncmp(s_header->magic, "SYMB", 4) != 0 || s_header->version != 2) {
        sym_loader_free();
        return false;
    }

    // 根据头部记录的偏移量切分指针
    uint32_t func_offset = sizeof(sym_header_t);
    uint32_t var_offset  = func_offset + s_header->func_count * sizeof(sym_func_entry_t);
    uint32_t pool_offset = var_offset + s_header->var_count * sizeof(sym_var_entry_t);

    if (pool_offset >= bytes_read) {
        sym_loader_free();
        return false; // 文件不完整
    }

    s_funcs = (sym_func_entry_t *)(s_sym_buffer + func_offset);
    s_vars  = (sym_var_entry_t *)(s_sym_buffer + var_offset);
    s_str_pool = (char *)(s_sym_buffer + pool_offset);
    return true;
}

const char *sym_loader_find_func(uint32_t pc, uint32_t *offset)
{
    if (!s_header || !s_funcs || s_header->func_count == 0) return NULL;

    pc &= ~1u;
    int low = 0, high = s_header->func_count - 1, hit = -1;

    // 二分查找
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (s_funcs[mid].addr <= pc) {
            hit = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (hit >= 0) {
        sym_func_entry_t *f = &s_funcs[hit];
        if (pc < (f->addr + f->size)) {
            if (offset) *offset = pc - f->addr;
            return s_str_pool + f->str_offset;
        }
    }
    return NULL;
}

uint16_t sym_loader_get_func_count()
{
    return s_header ? s_header->func_count:0;
}
const sym_func_entry_t *sym_loader_get_func(uint16_t index)
{
    if(!s_funcs||!s_header||index >= s_header->func_count) return NULL;
    return &s_funcs[index];
}
const char *sym_loader_get_func_name(const sym_func_entry_t *func)
{
    if(!func || !s_str_pool) return "";
    return s_str_pool + func->str_offset;
}

uint16_t sym_loader_get_var_count(void)
{
    return s_header ? s_header->var_count : 0;
}

const sym_var_entry_t *sym_loader_get_var(uint16_t index)
{
    if (!s_vars || !s_header || index >= s_header->var_count) return NULL;
    return &s_vars[index];
}

const char *sym_loader_get_var_name(const sym_var_entry_t *var)
{
    if (!var || !s_str_pool) return "";
    return s_str_pool + var->str_offset;
}