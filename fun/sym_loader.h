/*
 * @Author: 轩
 * @Date: 2026-09-23 14:54:39
 * @LastEditTime: 2026-09-23 16:15:53
 * @FilePath: \pico\fun\sym_loader.h
 */
#ifndef SYM_LOADER_H
#define SYM_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#pragma pack(push, 1)

typedef struct {
    char     magic[4];       // "SYMB"
    uint16_t version;        // 2
    uint16_t func_count;
    uint16_t var_count;
} sym_header_t;

typedef struct {
    uint32_t addr;
    uint16_t size;
    uint16_t str_offset;
} sym_func_entry_t;

typedef struct {
    uint32_t addr;
    uint8_t  size;
    uint8_t  type_hint;
    uint16_t str_offset;
} sym_var_entry_t;

#pragma pack(pop)

// --- 运行时暴露的接口 ---

// 初始化/清理内存
void sym_loader_init(void);
void sym_loader_free(void);

// 从 TF 卡加载指定的 .sym 文件
bool sym_loader_load_from_sd(const char *filepath);

// 根据 PC 地址查找对应的函数名与内部偏移
const char *sym_loader_find_func(uint32_t pc, uint32_t *offset);

// (预留) 根据 SRAM 地址查找变量名
const char *sym_loader_find_var(uint32_t addr);

// 获取已载入的函数总数
uint16_t sym_loader_get_func_count(void);

// 按索引获取函数表项
const sym_func_entry_t *sym_loader_get_func(uint16_t index);

// 获取函数名字符串
const char *sym_loader_get_func_name(const sym_func_entry_t *func);
uint16_t sym_loader_get_var_count(void);
const sym_var_entry_t *sym_loader_get_var(uint16_t index);
const char *sym_loader_get_var_name(const sym_var_entry_t *var);
#endif // SYM_LOADER_H