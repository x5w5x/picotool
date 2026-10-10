/*
 * @Author: 轩
 * @Date: 2026-10-09 19:34:54
 * @LastEditTime: 2026-10-09 19:35:01
 * @FilePath: \pico\fun\fun_spi.h
 */
#ifndef FUN_SPI_H
#define FUN_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/spi.h"

// H1 排针物理映射 (RP2040 硬件 spi0)
#define PIN_SPI_MISO        4  // H1 Pin 9  (RX)
#define PIN_SPI_CS          5  // H1 Pin 10 (软件控制 GPIO)
#define PIN_SPI_SCK         6  // H1 Pin 11 (SCK)
#define PIN_SPI_MOSI        7  // H1 Pin 12 (TX)

#define SPI_HW_INST         spi0
#define SPI_DEFAULT_BAUD    10000000 // 默认 10 MHz

typedef struct {
    uint8_t     jedec[3];       // [0]: Manuf, [1]: MemType, [2]: Capacity
    uint8_t     uid[8];         // 64-bit 唯一序列号
    uint8_t     status1;        // 状态寄存器 1 (BUSY, WEL, BP0-BP3)
    uint8_t     status2;        // 状态寄存器 2 (QE 等)
    char        manuf_name[32]; // 厂商名称
    char        model_name[32]; // 芯片系列及容量描述
    uint32_t    size_bytes;     // 字节容量 (如 8MB 为 8388608)
    bool        detected;
} spi_flash_info_t;

// 驱动生命周期与状态控制
void        fun_spi_init(uint8_t mode, uint32_t baud_hz);
void        fun_spi_deinit(void);
void        fun_spi_set_baud(uint32_t baud_hz);
uint32_t    fun_spi_get_baud(void);
void        fun_spi_set_mode(uint8_t mode); // 0: CPOL=0,CPHA=0; 1: 0,1; 2: 1,0; 3: 1,1
uint8_t     fun_spi_get_mode(void);
void        fun_spi_cs_put(bool level);

// 通用全双工收发原语
int         fun_spi_transfer_raw(const uint8_t *tx, uint8_t *rx, size_t len);
int         fun_spi_transfer_hex(const char *tx_hex, uint8_t *rx_buf, size_t max_rx_len);

// SPI Flash 专用功能原语
int         fun_spi_flash_probe(spi_flash_info_t *info);
int         fun_spi_flash_read_block(uint32_t addr, uint8_t *buf, size_t len);

#endif // FUN_SPI_H