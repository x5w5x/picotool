#include "w25q64.h"
#include "board_pins.h"
#include "spi_bus.h"
#include "lcd.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/platform.h"

#define W25_SPI spi1                   // 公用spi1
#define W25_SPI_BAUD SPI_BUS_BAUD_HIGH // 高速spi 240Mhz/4=60Mhz

static inline void W25_CS_Select(void)
{
    LCD_WaitDMA();
    SPI_Bus_High();

    // 核心保护：读前抽干残留，杜绝旧数据冒充字模第一字节
    while (spi_is_readable(W25_SPI))
    {
        (void)spi_get_hw(W25_SPI)->dr;
    }

    gpio_put(PIN_W25_CS, 0);
}

static inline void W25_CS_Release(void)
{

    uint8_t dummy = 0xFF;
    spi_write_blocking(W25_SPI, &dummy, 1);
    gpio_put(PIN_W25_CS, 1);
}

static inline uint8_t SPI_RW(uint8_t data)
{
    uint8_t rx;
    spi_write_read_blocking(W25_SPI, &data, &rx, 1);
    return rx;
}

void W25_Init(void)
{
    gpio_init(PIN_W25_CS);
    gpio_set_dir(PIN_W25_CS, GPIO_OUT);
    gpio_put(PIN_W25_CS, 1);

    if (!spi_is_writable(W25_SPI))
    {
        spi_init(W25_SPI, W25_SPI_BAUD);
        spi_set_format(W25_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
        gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
        gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
        gpio_pull_up(PIN_SPI_MISO);
    }
    SPI_Bus_High();
}

uint32_t W25_ReadJEDECID(void)
{
    uint32_t id = 0;
    W25_CS_Select();
    SPI_RW(W25_CMD_READ_JEDEC_ID);
    id = (uint32_t)SPI_RW(0xFF) << 16;
    id |= (uint32_t)SPI_RW(0xFF) << 8;
    id |= (uint32_t)SPI_RW(0xFF);
    W25_CS_Release();
    return id;
}

uint8_t W25_ReadStatus1(void)
{
    uint8_t s;
    W25_CS_Select();
    SPI_RW(W25_CMD_READ_STATUS_1);
    s = SPI_RW(0xFF);
    W25_CS_Release();
    return s;
}

void W25_WriteEnable(void)
{
    W25_CS_Select();
    SPI_RW(W25_CMD_WRITE_ENABLE);
    W25_CS_Release();
}

void W25_WriteDisable(void)
{
    W25_CS_Select();
    SPI_RW(W25_CMD_WRITE_DISABLE);
    W25_CS_Release();
}

bool W25_WaitBusy(uint32_t timeout_ms)
{
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (W25_ReadStatus1() & 0x01)
    {
        if ((to_ms_since_boot(get_absolute_time()) - t0) >= timeout_ms)
        {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

void W25_EraseSector(uint32_t addr)
{
    W25_WriteEnable();
    W25_CS_Select();
    SPI_RW(W25_CMD_SECTOR_ERASE);
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    W25_CS_Release();
    W25_WaitBusy(500);
}

void W25_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (len == 0)
        return;
    uint16_t page_offset = (uint16_t)(addr % W25_PAGE_SIZE);
    uint16_t max_allowed = W25_PAGE_SIZE - page_offset;
    if (len > max_allowed)
        len = max_allowed;

    W25_WriteEnable();
    W25_CS_Select();
    SPI_RW(W25_CMD_PAGE_PROGRAM);
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    spi_write_blocking(W25_SPI, buf, len);
    W25_CS_Release();
    W25_WaitBusy(10);
}

void W25_ReadData(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0)
        return;
    W25_CS_Select();
    SPI_RW(W25_CMD_READ_DATA); // 0x03
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    spi_read_blocking(W25_SPI, 0xFF, buf, len);
    W25_CS_Release();
}

void W25_FastReadData(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0)
        return;
    /* 0x0B 高速读:支持到 104 MHz(比我们用的 62.5 MHz 高得多),
     * 需要在地址之后多发 1 个 dummy 字节让片内采样器对齐。 */
    W25_CS_Select();
    SPI_RW(W25_CMD_FAST_READ); /* 0x0B */
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    SPI_RW(0xFF); /* 必须的 dummy 字节 */
    spi_read_blocking(W25_SPI, 0xFF, buf, len);
    W25_CS_Release();
}

void W25_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0)
    {
        uint32_t page_offset = addr % W25_PAGE_SIZE;
        uint16_t page_remain = (uint16_t)(W25_PAGE_SIZE - page_offset);
        uint16_t write_len = (len < page_remain) ? (uint16_t)len : page_remain;

        W25_PageProgram(addr, buf, write_len);
        addr += write_len;
        buf += write_len;
        len -= write_len;
    }
}