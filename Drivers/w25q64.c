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

/* W25 当前使用的 SPI 速率。默认跟 LCD 一起跑 62.5MHz。
 * 擦除/编程在长线上用 62.5MHz 可能不可靠(命令字节采样错误 → 芯片收不到),
 * 烧录时可用 W25_SetBaud() 临时降到 20MHz。 */
static uint32_t s_w25_baud = W25_SPI_BAUD;

void W25_SetBaud(uint32_t baud)
{
    s_w25_baud = baud;
}

uint32_t W25_GetBaud(void)
{
    return s_w25_baud;
}

static inline void W25_CS_Select(void)
{
    LCD_WaitDMA();
    /* ⚡ 注意:这里同时决定了 LCD 的速率 —— lcd.c 自己不设速率,
     * 完全依赖上一个碰总线的人。烧录时降到 20MHz 会更稳,
     * 代价是屏幕刷新变慢(可接受)。 */
    SPI_Bus_SetBaud(s_w25_baud);

    // 核心保护：读前抽干残留，杜绝旧数据冒充字模第一字节
    while (spi_is_readable(W25_SPI))
    {
        (void)spi_get_hw(W25_SPI)->dr;
    }

    gpio_put(PIN_W25_CS, 0);
}

static inline void W25_CS_Release(void)
{
    /* ⚡ 必须在最后一个字节结束后**立即**拉高 CS#,不能再多发时钟。
     *
     * 这里原来在 CS 仍为低时多发了一个 0xFF,造成两个后果:
     *
     * 1) Write Enable(0x06) 永不生效
     *    W25Q64 手册要求:指令第 8 位结束后必须马上拉高 CS#,否则
     *    Write Enable 不执行。多发 8 个时钟 → WEL 一直是 0 →
     *    后续的擦除/页编程全部被芯片静默丢弃(BUSY 位也不置),
     *    表现就是"擦除秒完成、内容一点没变、校验必失败"。
     *
     * 2) Page Program(0x02) 写坏每页第 0 字节
     *    多出的字节被当成数据接收,页内地址回卷 → 绕回该页偏移 0。
     */
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

uint8_t W25_ReadStatus2(void)
{
    uint8_t s;
    W25_CS_Select();
    SPI_RW(W25_CMD_READ_STATUS_2);
    s = SPI_RW(0xFF);
    W25_CS_Release();
    return s;
}

uint8_t W25_ReadStatus3(void)
{
    uint8_t s;
    W25_CS_Select();
    SPI_RW(W25_CMD_READ_STATUS_3);
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

bool W25_WriteEnableChecked(void)
{
    /* 写使能后 WEL(bit1)必须为 1,否则后续擦除/编程命令会被芯片丢弃。
     * 给 3 次重试:命令到 WEL 生效有 tWEL 延迟,偶尔第一次读早了。 */
    for (int attempt = 0; attempt < 3; ++attempt) {
        W25_WriteEnable();
        if (W25_ReadStatus1() & 0x02u) {
            return true;
        }
        busy_wait_us(50);
    }
    return false;
}

/* SR2 的 CMP 位(bit7)与 BP 组合可以保护整片:
 *   CMP=1 且 BP=0  → 全片写保护(SR1 看起来却是 0x00,极具迷惑性) */
bool W25_IsProtected(void)
{
    uint8_t s1 = W25_ReadStatus1();
    uint8_t s2 = W25_ReadStatus2();
    if (s1 & 0x3Cu) return true;       /* BP2..BP0 任一置位 */
    if (s2 & 0x80u) return true;       /* CMP 置位 */
    return false;
}

bool W25_ClearBlockProtect(void)
{
    /* 1) 清 SR3 的 WPS(若支持)
     * 2) 写 SR1=0 + SR2=0:CMP 与 BP 全部清零 */
    W25_WriteEnable();
    W25_CS_Select();
    SPI_RW(W25_CMD_WRITE_STATUS);
    SPI_RW(0x00);          /* SR1: BP=0, 且 SRP=0 */
    SPI_RW(0x00);          /* SR2: CMP=0 */
    W25_CS_Release();
    if (!W25_WaitBusy(500)) return false;

    W25_WriteEnable();
    W25_CS_Select();
    SPI_RW(W25_CMD_WRITE_STATUS_3);
    SPI_RW(0x00);
    W25_CS_Release();
    if (!W25_WaitBusy(500)) return false;

    /* 清完后必须能正常置 WEL,才算成功 */
    return W25_WriteEnableChecked();
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

/* ⚠️ 这两个函数之前在 w25q64.h 里声明了,但 .c 里从来没实现过 ——
 * 任何调用都会链接失败(undefined reference)。
 * 烧字库需要按 64KB 块擦除(比 4KB 扇区快 16 倍),这里补上。 */

bool W25_EraseBlock64K(uint32_t addr)
{
    /* 64KB 块擦除典型 150ms,最坏可达 2s(W25Q64 数据手册 tBE2)。
     * 超时留 3s 余量,避免大块擦除被误判超时。 */
    /* ⚡ 写使能必须确认 WEL 置位,否则擦除命令被芯片丢弃,
     * 而 BUSY 位一直是 0,看上去"擦除秒完成"。 */
    if (!W25_WriteEnableChecked()) {
        return false;
    }

    W25_CS_Select();
    SPI_RW(W25_CMD_BLOCK64_ERASE);
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    W25_CS_Release();

    if (!W25_WaitBusy(3000)) return false;
    if (W25_ReadStatus1() & 0x01u) return false;   /* 仍在忙 */
    return true;
}

bool W25_EraseChip(void)
{
    /* 全片擦除:W25Q64(8MB)典型 20s,最坏 100s。
     * 只在需要彻底清空时用,正常烧字库请用块擦除。 */
    if (!W25_WriteEnableChecked()) {
        return false;
    }
    W25_CS_Select();
    SPI_RW(W25_CMD_CHIP_ERASE);
    W25_CS_Release();
    if (!W25_WaitBusy(120000)) return false;
    if (W25_ReadStatus1() & 0x01u) return false;
    return true;
}

bool W25_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (len == 0)
        return true;
    uint16_t page_offset = (uint16_t)(addr % W25_PAGE_SIZE);
    uint16_t max_allowed = W25_PAGE_SIZE - page_offset;
    if (len > max_allowed)
        len = max_allowed;

    if (!W25_WriteEnableChecked()) {
        return false;
    }

    W25_CS_Select();
    SPI_RW(W25_CMD_PAGE_PROGRAM);
    SPI_RW((uint8_t)(addr >> 16));
    SPI_RW((uint8_t)(addr >> 8));
    SPI_RW((uint8_t)addr);
    spi_write_blocking(W25_SPI, buf, len);
    W25_CS_Release();

    /* 页编程典型 0.7ms,最坏 3ms;给 20ms 余量 */
    if (!W25_WaitBusy(20)) return false;
    if (W25_ReadStatus1() & 0x01u) return false;
    return true;
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

/* 返回实际写入的字节数。调用方应该比较返回值是否等于 len ——
 * 中途失败时的部分写入必须让上层知道,否则会静默丢数据。 */
uint32_t W25_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (len > 0)
    {
        uint32_t page_offset = addr % W25_PAGE_SIZE;
        uint16_t page_remain = (uint16_t)(W25_PAGE_SIZE - page_offset);
        uint16_t write_len = (len < page_remain) ? (uint16_t)len : page_remain;

        if (!W25_PageProgram(addr, buf, write_len)) {
            break;   /* 写失败,立即停止,别继续往后面的页写 */
        }
        addr += write_len;
        buf += write_len;
        len -= write_len;
        done += write_len;
    }
    return done;
}