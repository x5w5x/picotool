/*
 * @Author: 轩
 * @Date: 2026-08-17 21:04:03
 * @LastEditTime: 2026-08-18 18:25:43
 * @FilePath: \pico\Drivers\w25q64.h
 */
#ifndef W25Q64_H
#define W25Q64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ===================== W25Q64 指令集 ===================== */
#define W25_CMD_WRITE_ENABLE    0x06
#define W25_CMD_WRITE_DISABLE   0x04
#define W25_CMD_READ_STATUS_1   0x05
#define W25_CMD_READ_STATUS_2   0x35
#define W25_CMD_READ_DATA       0x03   /* 标准读 (≤50MHz) */
#define W25_CMD_FAST_READ       0x0B   /* 快速读 (≤104MHz/133MHz, 需1字节Dummy) */
#define W25_CMD_PAGE_PROGRAM    0x02
#define W25_CMD_SECTOR_ERASE    0x20   /* 4KB 扇区擦除 */
#define W25_CMD_BLOCK32_ERASE   0x52   /* 32KB 块擦除 */
#define W25_CMD_BLOCK64_ERASE   0xD8   /* 64KB 块擦除 */
#define W25_CMD_CHIP_ERASE      0xC7   /* 全片擦除 */
#define W25_CMD_READ_JEDEC_ID   0x9F
#define W25_CMD_READ_UNIQUE_ID  0x4B

/* ===================== 芯片规格参数 ===================== */
#define W25_FLASH_SIZE          (8 * 1024 * 1024)  /* 8 MBytes */
#define W25_SECTOR_SIZE         4096               /* 4 KBytes */
#define W25_PAGE_SIZE           256                /* 256 Bytes */
#define W25_JEDEC_ID            0x00EF4017         /* W25Q64 标准 JEDEC ID */

/* ===================== 核心控制 API ===================== */
void     W25_Init(void);
uint32_t W25_ReadJEDECID(void);
uint8_t  W25_ReadStatus1(void);
void     W25_WriteEnable(void);
void     W25_WriteDisable(void);
bool     W25_WaitBusy(uint32_t timeout_ms);

/* ===================== 擦除与读写 API ===================== */
void     W25_EraseSector(uint32_t addr);
void     W25_EraseBlock64K(uint32_t addr);
void     W25_EraseChip(void);
void     W25_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len);
void     W25_ReadData(uint32_t addr, uint8_t *buf, uint32_t len);
void     W25_FastReadData(uint32_t addr, uint8_t *buf, uint32_t len);
void     W25_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* W25Q64_H */