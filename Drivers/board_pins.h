/**
 * @file board_pins.h
 * @brief 单文件集中所有外设引脚映射,改接线只动这里
 *
 * 真实接线(U3 14p FPC + U2 6p 排针):
 *   - LCD ST7796 / Flash W25Q64 / TF SD 卡 三者共用 SPI1
 *     (SCK=GP10, MOSI=GP11, MISO=GP8)
 *   - LCD 用 CS=GP9,Flash 用 CS=GP14,TF 用 CS=GP15
 *   - 触摸 FT6336 用 I2C1(SDA=GP16, SCL=GP17, INT=GP18, RST=GP19)
 *   - 屏幕背光直接接 3.3V(没有 BL 控制 GPIO)
 *
 * SPI 速率分配(运行时切换):
 *   LCD + Flash  → 62.5 MHz
 *   TF (SD 卡)   → 20   MHz(SPI 模式下 SD 卡最高 25 MHz)
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* ===================== SPI1 总线 (LCD + Flash + TF 三方共享) ===================== */
#define PIN_SPI_SCK    10  /* SPI1 SCK */
#define PIN_SPI_MOSI   11  /* SPI1 MOSI */
#define PIN_SPI_MISO    8  /* SPI1 MISO (Flash/TF 读用) */

/* ===================== LCD ST7796U (SPI1, CS=GP9) ===================== */
#define PIN_LCD_CS      9  /* 软件 CS,LCD 独立 */
#define PIN_LCD_DC     12  /* D/C 脚 */
#define PIN_LCD_RST    13  /* 复位(低有效) */
/* BL 没有 GPIO 控制 —— 模块上直接接 3.3V,恒亮 */

/* ===================== Flash W25Q64 (SPI1, CS=GP14) ===================== */
#define PIN_W25_CS     14  /* 软件 CS,Flash 独立 */

/* ===================== TF (SD 卡, SPI1, CS=GP15) ===================== */
#define PIN_TF_CS      15  /* 软件 CS,TF 卡独立 */

/* ===================== Touch FT6336 (I2C1) ===================== */
#define PIN_TOUCH_SDA  16  /* I2C1 SDA,需 4.7k 外接上拉 */
#define PIN_TOUCH_SCL  17  /* I2C1 SCL,需 4.7k 外接上拉 */
#define PIN_TOUCH_INT  18  /* 触摸中断(低有效),目前轮询 */
#define PIN_TOUCH_RST  19  /* 触摸复位(低有效) */

#endif /* BOARD_PINS_H */