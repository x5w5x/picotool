#include "fun_spi.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_spi_baud = SPI_DEFAULT_BAUD;
static uint8_t  s_spi_mode = 0;
static bool     s_inited   = false;

void fun_spi_cs_put(bool level) {
    gpio_put(PIN_SPI_CS, level ? 1 : 0);
}

void fun_spi_set_mode(uint8_t mode) {
    s_spi_mode = mode & 0x03;
    spi_cpol_t cpol = (s_spi_mode & 2) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (s_spi_mode & 1) ? SPI_CPHA_1 : SPI_CPHA_0;
    spi_set_format(SPI_HW_INST, 8, cpol, cpha, SPI_MSB_FIRST);
}

uint8_t fun_spi_get_mode(void) {
    return s_spi_mode;
}

void fun_spi_set_baud(uint32_t baud_hz) {
    if (!baud_hz) return;
    s_spi_baud = baud_hz;
    spi_set_baudrate(SPI_HW_INST, s_spi_baud);
}

uint32_t fun_spi_get_baud(void) {
    return s_spi_baud;
}

void fun_spi_init(uint8_t mode, uint32_t baud_hz) {
    s_spi_baud = baud_hz ? baud_hz : SPI_DEFAULT_BAUD;
    s_spi_mode = mode & 0x03;

    // 1. 初始化并拉高独立片选 CS
    gpio_init(PIN_SPI_CS);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    fun_spi_cs_put(true);

    // 2. 将引脚接入硬件 SPI 控制器
    spi_init(SPI_HW_INST, s_spi_baud);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);

    fun_spi_set_mode(s_spi_mode);
    s_inited = true;
}

void fun_spi_deinit(void) {
    if (!s_inited) return;
    spi_deinit(SPI_HW_INST);

    // 彻底释放引脚为浮空输入，消除对 I2C 模拟模式的电气污染
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_NULL);
    gpio_set_function(PIN_SPI_CS,   GPIO_FUNC_NULL);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_NULL);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_NULL);

    gpio_set_dir(PIN_SPI_MISO, GPIO_IN);
    gpio_set_dir(PIN_SPI_CS,   GPIO_IN);
    gpio_set_dir(PIN_SPI_SCK,  GPIO_IN);
    gpio_set_dir(PIN_SPI_MOSI, GPIO_IN);

    gpio_disable_pulls(PIN_SPI_MISO);
    gpio_disable_pulls(PIN_SPI_CS);
    gpio_disable_pulls(PIN_SPI_SCK);
    gpio_disable_pulls(PIN_SPI_MOSI);

    s_inited = false;
}

int fun_spi_transfer_raw(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_inited) fun_spi_init(0, SPI_DEFAULT_BAUD);
    if (len == 0) return 0;

    static const uint8_t dummy_tx = 0xFF;
    if (tx && rx) {
        return spi_write_read_blocking(SPI_HW_INST, tx, rx, len);
    } else if (tx && !rx) {
        return spi_write_blocking(SPI_HW_INST, tx, len);
    } else if (!tx && rx) {
        return spi_read_blocking(SPI_HW_INST, dummy_tx, rx, len);
    }
    return -1;
}

int fun_spi_transfer_hex(const char *tx_hex, uint8_t *rx_buf, size_t max_rx_len) {
    if (!tx_hex || !rx_buf || max_rx_len == 0) return -1;

    uint8_t tx_buf[64];
    size_t len = 0;
    const char *p = tx_hex;

    while (*p && len < sizeof(tx_buf) && len < max_rx_len) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *endp = NULL;
        unsigned long val = strtoul(p, &endp, 16);
        if (endp == p) break;
        tx_buf[len++] = (uint8_t)(val & 0xFF);
        p = endp;
    }
    if (len == 0) return 0;

    fun_spi_cs_put(false);
    int ret = fun_spi_transfer_raw(tx_buf, rx_buf, len);
    fun_spi_cs_put(true);
    return (ret >= 0) ? (int)len : -1;
}

int fun_spi_flash_probe(spi_flash_info_t *info) {
    if (!info) return -1;
    memset(info, 0, sizeof(spi_flash_info_t));

    // 1. 读取 JEDEC ID (指令 0x9F)
    uint8_t cmd_jedec = 0x9F;
    fun_spi_cs_put(false);
    spi_write_blocking(SPI_HW_INST, &cmd_jedec, 1);
    spi_read_blocking(SPI_HW_INST, 0x00, info->jedec, 3);
    fun_spi_cs_put(true);

    if ((info->jedec[0] == 0x00 && info->jedec[1] == 0x00) ||
        (info->jedec[0] == 0xFF && info->jedec[1] == 0xFF)) {
        info->detected = false;
        strcpy(info->manuf_name, "None");
        strcpy(info->model_name, "Check Wiring");
        return -1;
    }

    info->detected = true;

    // 2. 读取 64-bit 唯一 UID (指令 0x4B + 4 字节 Dummy)
    uint8_t cmd_uid[5] = { 0x4B, 0x00, 0x00, 0x00, 0x00 };
    fun_spi_cs_put(false);
    spi_write_blocking(SPI_HW_INST, cmd_uid, sizeof(cmd_uid));
    spi_read_blocking(SPI_HW_INST, 0x00, info->uid, 8);
    fun_spi_cs_put(true);

    // 3. 读取状态寄存器 1 (0x05) 与 2 (0x35)
    uint8_t cmd_sr1 = 0x05;
    fun_spi_cs_put(false);
    spi_write_blocking(SPI_HW_INST, &cmd_sr1, 1);
    spi_read_blocking(SPI_HW_INST, 0x00, &info->status1, 1);
    fun_spi_cs_put(true);

    uint8_t cmd_sr2 = 0x35;
    fun_spi_cs_put(false);
    spi_write_blocking(SPI_HW_INST, &cmd_sr2, 1);
    spi_read_blocking(SPI_HW_INST, 0x00, &info->status2, 1);
    fun_spi_cs_put(true);

    // 厂商匹配: 统一纯 ASCII，杜绝字库缺失引起的乱码
    switch (info->jedec[0]) {
        case 0xEF: strcpy(info->manuf_name, "Winbond");    break;
        case 0xC8: strcpy(info->manuf_name, "GigaDevice"); break;
        case 0x20: strcpy(info->manuf_name, "Micron");     break;
        case 0xC2: strcpy(info->manuf_name, "Macronix");   break;
        case 0x1F: strcpy(info->manuf_name, "Atmel");      break;
        case 0x0B: strcpy(info->manuf_name, "XTX");        break;
        case 0x85: strcpy(info->manuf_name, "Puya");       break;
        default:   snprintf(info->manuf_name, sizeof(info->manuf_name), "ID:0x%02X", info->jedec[0]); break;
    }

    // 容量计算: 2^capacity_code
    uint8_t cap_code = info->jedec[2];
    if (cap_code >= 0x11 && cap_code <= 0x1B) {
        info->size_bytes = 1UL << cap_code;
        uint32_t mb = info->size_bytes / (1024 * 1024);
        const char *prefix = (info->jedec[0] == 0xEF) ? "W25Q" :
                             (info->jedec[0] == 0xC8) ? "GD25Q" : "25Q";
        snprintf(info->model_name, sizeof(info->model_name), "%s%lu (%luMB)",
                 prefix, (unsigned long)(mb * 8), (unsigned long)mb);
    } else {
        info->size_bytes = 0;
        snprintf(info->model_name, sizeof(info->model_name), "Cap:0x%02X", cap_code);
    }
    return 0;
}

int fun_spi_flash_read_block(uint32_t addr, uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;
    uint8_t cmd[4] = {
        0x03, // 标准慢读指令 (Read Data)
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8)  & 0xFF),
        (uint8_t)(addr & 0xFF)
    };

    fun_spi_cs_put(false);
    spi_write_blocking(SPI_HW_INST, cmd, 4);
    spi_read_blocking(SPI_HW_INST, 0x00, buf, len);
    fun_spi_cs_put(true);
    return (int)len;
}