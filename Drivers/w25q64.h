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
#define W25_CMD_READ_STATUS_3   0x15
#define W25_CMD_WRITE_STATUS    0x01   /* 写 SR1+SR2 */
#define W25_CMD_WRITE_STATUS_3  0x11   /* 写 SR3 (FV 系列) */
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
/* 设置 W25 使用的 SPI 速率(同时也影响 LCD,因为 lcd.c 自己不设速率)。
 * 擦除/编程不可靠时降到 SPI_BUS_BAUD_TF(25MHz) 甚至 10MHz 试试。 */
void     W25_SetBaud(uint32_t baud);
uint32_t W25_GetBaud(void);
uint32_t W25_ReadJEDECID(void);
uint8_t  W25_ReadStatus1(void);
uint8_t  W25_ReadStatus2(void);
uint8_t  W25_ReadStatus3(void);
void     W25_WriteEnable(void);
void     W25_WriteDisable(void);

/* 写使能并确认 WEL 置位(带重试)。返回 false 表示芯片拒绝写使能 —— 
 * 通常意味着块保护被打开(SR1.BP 或 SR2.CMP 置位)。 */
bool     W25_WriteEnableChecked(void);

/* 清除全部写保护:写 SR1=0、SR2=0(CMP/BP 全清),再清 SR3。
 * 返回 true 表示清除后 WEL 可以正常置位。 */
bool     W25_ClearBlockProtect(void);

/* 根据 SR1/SR2 判断当前是否处于写保护状态 */
bool     W25_IsProtected(void);

bool     W25_WaitBusy(uint32_t timeout_ms);

/* ===================== 擦除与读写 API ===================== */
void     W25_EraseSector(uint32_t addr);
/* 返回 false 表示命令没被芯片接受(WEL 未置位)或擦除超时未完成。
 * 调用方应据此报错,而不是默默继续 —— 否则后面写入/校验会莫名其妙失败。 */
bool     W25_EraseBlock64K(uint32_t addr);
bool     W25_EraseChip(void);
/* 返回 false 表示写使能失败或页编程超时 */
bool     W25_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len);
void     W25_ReadData(uint32_t addr, uint8_t *buf, uint32_t len);
void     W25_FastReadData(uint32_t addr, uint8_t *buf, uint32_t len);
uint32_t W25_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* W25Q64_H */