#include "tf.h"
#include "board_pins.h"
#include "spi_bus.h"
#include "lcd.h"            // 新增：引入 LCD_WaitDMA() 保证总线安全

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/platform.h"

#define TF_SPI             spi1

#ifndef PIN_TF_CS
#define PIN_TF_CS          PIN_SD_CS
#endif

#define TF_INIT_BAUD       (300 * 1000)     // 初始化低速 (< 400kHz)
#define TF_RUN_BAUD        SPI_BUS_BAUD_TF  // 运行时高速 (20MHz)

#define CMD0   0   /* GO_IDLE_STATE */
#define CMD8   8   /* SEND_IF_COND */
#define CMD9   9   /* SEND_CSD */
#define CMD10  10  /* SEND_CID */
#define CMD16  16  /* SET_BLOCKLEN */
#define CMD17  17  /* READ_SINGLE_BLOCK */
#define CMD24  24  /* WRITE_BLOCK */
#define CMD55  55  /* APP_CMD */
#define CMD58  58  /* READ_OCR */
#define ACMD41 41  /* SD_SEND_OP_COND */

#define R1_IDLE           (1 << 0)
#define R1_ILLEGAL_CMD    (1 << 2)

static uint8_t tf_ready = 0;

static inline uint8_t tf_xchg(uint8_t b)
{
    uint8_t rx;
    spi_write_read_blocking(TF_SPI, &b, &rx, 1);
    return rx;
}

static inline void tf_cs_select(void)
{
    gpio_put(PIN_TF_CS, 0);
}

static inline void tf_cs_release(void)
{
    gpio_put(PIN_TF_CS, 1);
    tf_xchg(0xFF); // 补充 8 个 Dummy 时钟，使 SD 卡彻底释放 MISO 进入高阻态
}

static int tf_wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    do {
        if (tf_xchg(0xFF) == 0xFF) return 1;
        tight_loop_contents();
    } while ((to_ms_since_boot(get_absolute_time()) - t0) < timeout_ms);
    return 0;
}

static uint8_t tf_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t buf[6];
    uint8_t r1 = 0xFF;

    buf[0] = 0x40 | (cmd & 0x3F);
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)(arg);
    buf[5] = crc;

    tf_cs_select();

    if (!tf_wait_ready(300)) {
        tf_cs_release();
        return 0xFF;
    }

    for (int i = 0; i < 6; i++) {
        tf_xchg(buf[i]);
    }

    for (int i = 0; i < 10; i++) {
        r1 = tf_xchg(0xFF);
        if ((r1 & 0x80) == 0) break;
    }
    return r1;
}

static uint8_t tf_send_acmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t r1 = tf_send_cmd(CMD55, 0, 0xFF);
    tf_cs_release();
    if (r1 > 1) return r1;
    return tf_send_cmd(cmd, arg, crc);
}

int TF_Init(void)
{
    uint8_t r1;
    uint32_t t0;

    LCD_WaitDMA(); // 确保 LCD 的 DMA 搬运已彻底完成

    gpio_init(PIN_TF_CS);
    gpio_set_dir(PIN_TF_CS, GPIO_OUT);
    gpio_put(PIN_TF_CS, 1);

    if (!spi_is_writable(TF_SPI)) {
        spi_init(TF_SPI, TF_RUN_BAUD);
        spi_set_format(TF_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
        gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
        gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    }

    // 开启 MISO 弱上拉，防止总线悬空读回 0x00
    gpio_pull_up(PIN_SPI_MISO);

    // 1. 降频至 300kHz
    SPI_Bus_SetBaud(TF_INIT_BAUD);

    // 2. 发送至少 74 个时钟脉冲同步
    gpio_put(PIN_TF_CS, 1);
    for (int i = 0; i < 20; i++) {
        tf_xchg(0xFF);
    }

    // 3. 循环发送 CMD0 尝试进入 SPI 模式
    uint8_t retry = 0;
    do {
        r1 = tf_send_cmd(CMD0, 0, 0x95);
        tf_cs_release();
        if (r1 == R1_IDLE) break;
        sleep_ms(10);
    } while (++retry < 10);

    if (r1 != R1_IDLE) {
        SPI_Bus_High();
        return TF_ERR_INIT;
    }

    // 4. CMD8 接口条件检测
    r1 = tf_send_cmd(CMD8, 0x000001AA, 0x87);
    if (r1 & R1_ILLEGAL_CMD) {
        tf_cs_release();
        SPI_Bus_High();
        return TF_ERR_INIT;
    } else if (r1 != R1_IDLE) {
        tf_cs_release();
        SPI_Bus_High();
        return TF_ERR_INIT;
    }

    uint8_t r7[4];
    for (int i = 0; i < 4; i++) {
        r7[i] = tf_xchg(0xFF);
    }
    tf_cs_release();

    if (r7[2] != 0x01 || r7[3] != 0xAA) {
        SPI_Bus_High();
        return TF_ERR_INIT;
    }

    // 5. 循环 ACMD41 初始化卡片
    t0 = to_ms_since_boot(get_absolute_time());
    do {
        r1 = tf_send_acmd(ACMD41, 0x40000000, 0xFF);
        tf_cs_release();
        if (r1 == 0) break;
        if (to_ms_since_boot(get_absolute_time()) - t0 > 1500) {
            SPI_Bus_High();
            return TF_ERR_INIT;
        }
        sleep_ms(10);
    } while (1);

    // 6. CMD58 检查 OCR
    r1 = tf_send_cmd(CMD58, 0, 0xFF);
    if (r1 != 0) {
        tf_cs_release();
        SPI_Bus_High();
        return TF_ERR_INIT;
    }
    for (int i = 0; i < 4; i++) {
        tf_xchg(0xFF);
    }
    tf_cs_release();

    // 7. CMD16 锁定 512 字节块大小
    r1 = tf_send_cmd(CMD16, TF_BLOCK_SIZE, 0xFF);
    tf_cs_release();
    if (r1 != 0) {
        SPI_Bus_High();
        return TF_ERR_INIT;
    }

    tf_ready = 1;
    SPI_Bus_High(); // 还原 62.5MHz 给屏幕与 Flash
    return TF_OK;
}

int TF_ReadCID(uint8_t buf[16])
{
    if (!tf_ready) return TF_ERR_INIT;

    LCD_WaitDMA();
    SPI_Bus_TF();

    uint8_t r1 = tf_send_cmd(CMD10, 0, 0xFF);
    if (r1 != 0) {
        tf_cs_release();
        SPI_Bus_High();
        return TF_ERR_CMD;
    }

    uint8_t tok = 0xFF;
    for (int i = 0; i < 100; i++) {
        tok = tf_xchg(0xFF);
        if (tok == 0xFE) break;
    }

    if (tok != 0xFE) {
        tf_cs_release();
        SPI_Bus_High();
        return TF_ERR_READ;
    }

    for (int i = 0; i < 16; i++) {
        buf[i] = tf_xchg(0xFF);
    }

    tf_xchg(0xFF); // CRC
    tf_xchg(0xFF);
    tf_cs_release();

    SPI_Bus_High();
    return TF_OK;
}

int TF_ReadBlocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!tf_ready) return TF_ERR_INIT;

    LCD_WaitDMA();
    SPI_Bus_TF();

    for (uint32_t n = 0; n < count; n++) {
        uint8_t r1 = tf_send_cmd(CMD17, lba + n, 0xFF);
        if (r1 != 0) {
            tf_cs_release();
            SPI_Bus_High();
            return TF_ERR_READ;
        }

        uint8_t tok = 0xFF;
        for (int i = 0; i < 200; i++) {
            tok = tf_xchg(0xFF);
            if (tok == 0xFE) break;
        }

        if (tok != 0xFE) {
            tf_cs_release();
            SPI_Bus_High();
            return TF_ERR_READ;
        }

        for (uint16_t i = 0; i < TF_BLOCK_SIZE; i++) {
            buf[n * TF_BLOCK_SIZE + i] = tf_xchg(0xFF);
        }

        tf_xchg(0xFF);
        tf_xchg(0xFF);
        tf_cs_release();
    }

    SPI_Bus_High();
    return TF_OK;
}

int TF_WriteBlocks(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    if (!tf_ready) return TF_ERR_INIT;

    LCD_WaitDMA();
    SPI_Bus_TF();

    for (uint32_t n = 0; n < count; n++) {
        uint8_t r1 = tf_send_cmd(CMD24, lba + n, 0xFF);
        if (r1 != 0) {
            tf_cs_release();
            SPI_Bus_High();
            return TF_ERR_WRITE;
        }

        tf_xchg(0xFF);
        tf_xchg(0xFE);

        for (uint16_t i = 0; i < TF_BLOCK_SIZE; i++) {
            tf_xchg(buf[n * TF_BLOCK_SIZE + i]);
        }

        tf_xchg(0xFF);
        tf_xchg(0xFF);

        uint8_t dr = tf_xchg(0xFF);
        if ((dr & 0x1F) != 0x05) {
            tf_cs_release();
            SPI_Bus_High();
            return TF_ERR_WRITE;
        }

        if (!tf_wait_ready(500)) {
            tf_cs_release();
            SPI_Bus_High();
            return TF_ERR_TIMEOUT;
        }

        tf_cs_release();
    }

    SPI_Bus_High();
    return TF_OK;
}

uint8_t TF_IsReady(void)
{
    return tf_ready;
}