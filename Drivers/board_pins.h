/*
 * @Author: 轩
 * @Date: 2026-08-18 16:28:24
 * @LastEditTime: 2026-09-01 15:28:06
 * @FilePath: \pico\Drivers\board_pins.h
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
/* ===================== LCD BL可以调光但是暂时不接 ==========================*/

/* ===================== Flash W25Q64 (SPI1, CS=GP14) ===================== */
#define PIN_W25_CS     14  /* 软件 CS,Flash 独立 */

/* ===================== TF (SD 卡, SPI1, CS=GP15) ===================== */
#define PIN_TF_CS      15  /* 软件 CS,TF 卡独立 */

/* ===================== Touch FT6336 (I2C1) ===================== */
#define PIN_TOUCH_SDA  16  /* I2C1 SDA */
#define PIN_TOUCH_SCL  17  /* I2C1 SCL */
#define PIN_TOUCH_INT  18  /* 触摸中断(低有效),目前轮询 */
#define PIN_TOUCH_RST  19  /* 触摸复位(低有效) */

#endif /* BOARD_PINS_H */