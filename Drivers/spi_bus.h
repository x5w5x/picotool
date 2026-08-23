/**
 * @file spi_bus.h
 * @brief SPI1 总线速率管理 (LCD + Flash + TF 三方共享)
 *
 * 三方都挂在 SPI1 上,速率不同:
 *   - LCD  和  W25Q64  → SPI_BUS_BAUD_HIGH  (62.5 MHz,默认上电速度)
 *   - TF (SD 卡)       → SPI_BUS_BAUD_TF    (20   MHz,SD 卡 SPI 模式上限)
 *
 * 切速度的原则:谁用谁负责。
 *   LCD_Init / W25_Init / TF_Init 各自把 SPI 切到自己需要的速率;
 *   函数退出前不切回 —— 留着给后续调用方用,免得每次操作都改一次。
 *
 * 如果你担心 Flash/TF 跨操作干扰(例如 LCD 写完立刻读 Flash),
 *   在每次访问前显式调一次 SPI_Bus_SetBaud(SPI_BUS_BAUD_HIGH) 即可。
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== SPI 速率档位 =====================
 *
 * 62.5 MHz = 125 MHz / 2,SPI 外设分频器最贴近整数的最高速率。
 *            ST7796 数据手册标到 50 MHz,实际多数屏能跑到 62.5 MHz;
 *            跑不动时屏幕会出现撕裂/白屏,降回 40 MHz 即可。
 * 20   MHz = SD 卡 SPI 模式下的安全上限,初始化时更应降到 < 400 kHz。
 */
#define SPI_BUS_BAUD_HIGH  (62500 * 1000)   /* LCD + Flash 默认 */
#define SPI_BUS_BAUD_TF    (20000 * 1000)   /* TF 卡运行时 */

/* ===================== API ===================== */

/* 把 SPI1 切到指定速率(Mode 0 / MSB 已在 init 时设好,这里只动速率)
 * 内部用 spi_set_baudrate(),不会重置 SPI 外设。 */
static inline void SPI_Bus_SetBaud(uint32_t baud)
{
    spi_set_baudrate(spi1, baud);
}

/* 让 SPI1 回到高速档 (62.5 MHz),LCD/Flash 用 */
static inline void SPI_Bus_High(void)
{
    SPI_Bus_SetBaud(SPI_BUS_BAUD_HIGH);
}

/* 让 SPI1 切到低速档 (20 MHz),TF 卡用 */
static inline void SPI_Bus_TF(void)
{
    SPI_Bus_SetBaud(SPI_BUS_BAUD_TF);
}

#ifdef __cplusplus
}
#endif

#endif /* SPI_BUS_H */
