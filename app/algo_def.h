/*
 * @Author: 轩
 * @Date: 2026-08-26 16:43:17
 * @LastEditTime: 2026-08-26 16:43:23
 * @FilePath: \pico\app\algo_def.h
 */
#ifndef ALGO_DEF_H
#define ALGO_DEF_H

#include <stdint.h>

#define ALGO_MAGIC 0x4F474C41 // "ALGO"

typedef struct {
    uint32_t magic;               // 魔数校验 "ALGO" (0x4F474C41)
    uint32_t flash_base;          // Flash 编程基地址 (如 0x08000000)
    uint32_t ram_base;            // 算法装载至目标 SRAM 的基地址 (如 0x20000000)
    uint32_t ram_size;            // 算法运行所需 SRAM 总大小
    uint32_t buffer_addr;         // 目标 SRAM 中用于接收分块数据的 Buffer 地址
    uint32_t buffer_size;         // 写入缓冲区分块大小 (如 1024 字节)
    
    // 各函数入口相对于 ram_base 的偏移 (Thumb 模式最低位置 1)
    uint32_t init_offset;         // Init(adr, clk, fnc)
    uint32_t uninit_offset;       // UnInit(fnc)
    uint32_t erase_chip_offset;   // EraseChip()
    uint32_t erase_sector_offset; // EraseSector(adr)
    uint32_t program_page_offset; // ProgramPage(adr, sz, buf)
    
    uint32_t code_size;           // 算法代码段二进制总长度
    uint8_t  chip_name[16];       // 芯片名称字符串
} __attribute__((packed)) algo_header_t;

#endif // ALGO_DEF_H