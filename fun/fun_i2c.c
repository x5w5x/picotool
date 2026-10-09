#include "fun_i2c.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <stdlib.h>
#include <string.h>

static uint32_t s_cur_baud = I2C_DEFAULT_BAUD;
static uint32_t s_delay_us = 5; // 100kHz 对应半周期 5µs

static const i2c_chip_info_t k_known_chips[] = {
    { 0x20, "PCF8574",   "8位IO扩展" },
    { 0x27, "PCF8574T",  "1602液晶转接板" },
    { 0x38, "FT6336/AHT","触控/温湿度" },
    { 0x3C, "SSD1306",   "0.96寸 OLED 屏幕" },
    { 0x3D, "SSD1306",   "OLED 屏幕 (备用地址)" },
    { 0x44, "SHT30/31",  "高精度温湿度" },
    { 0x48, "ADS1115",   "16位精密 ADC" },
    { 0x50, "AT24C02",   "EEPROM 存储器" },
    { 0x57, "AT24C32",   "RTC板载 EEPROM" },
    { 0x68, "MPU6050",   "六轴陀螺仪 / DS3231" },
    { 0x69, "MPU6050",   "六轴陀螺仪 (AD0=1)" },
    { 0x76, "BMP280",    "气压/温度传感器" },
    { 0x77, "BME280",    "温湿度气压三合一" }
};

/* ================= 1. GPIO 开漏底层原语 ================= */

static inline void bb_sda_high(void) {
    gpio_set_dir(PIN_I2C_SDA, GPIO_IN);
    gpio_pull_up(PIN_I2C_SDA);
}

static inline void bb_sda_low(void) {
    gpio_put(PIN_I2C_SDA, 0);
    gpio_set_dir(PIN_I2C_SDA, GPIO_OUT);
}

static inline void bb_scl_high(void) {
    gpio_set_dir(PIN_I2C_SCL, GPIO_IN);
    gpio_pull_up(PIN_I2C_SCL);
}

static inline void bb_scl_low(void) {
    gpio_put(PIN_I2C_SCL, 0);
    gpio_set_dir(PIN_I2C_SCL, GPIO_OUT);
}

static inline bool bb_sda_read(void) {
    return gpio_get(PIN_I2C_SDA);
}

static void bb_start(void) {
    bb_sda_high();
    bb_scl_high();
    busy_wait_us_32(s_delay_us);
    bb_sda_low();
    busy_wait_us_32(s_delay_us);
    bb_scl_low();
    busy_wait_us_32(s_delay_us);
}

static void bb_stop(void) {
    bb_sda_low();
    busy_wait_us_32(s_delay_us);
    bb_scl_high();
    busy_wait_us_32(s_delay_us);
    bb_sda_high();
    busy_wait_us_32(s_delay_us);
}

static bool bb_write_byte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) {
            bb_sda_high();
        } else {
            bb_sda_low();
        }
        busy_wait_us_32(s_delay_us);
        bb_scl_high();
        busy_wait_us_32(s_delay_us);
        bb_scl_low();
        busy_wait_us_32(s_delay_us);
    }

    bb_sda_high();
    busy_wait_us_32(s_delay_us);
    bb_scl_high();
    busy_wait_us_32(s_delay_us);
    bool ack = (bb_sda_read() == 0);
    bb_scl_low();
    busy_wait_us_32(s_delay_us);

    return ack;
}

static uint8_t bb_read_byte(bool send_ack) {
    uint8_t data = 0;
    bb_sda_high();

    for (int i = 7; i >= 0; i--) {
        busy_wait_us_32(s_delay_us);
        bb_scl_high();
        busy_wait_us_32(s_delay_us);
        if (bb_sda_read()) {
            data |= (1 << i);
        }
        bb_scl_low();
        busy_wait_us_32(s_delay_us);
    }

    if (send_ack) {
        bb_sda_low();
    } else {
        bb_sda_high();
    }
    busy_wait_us_32(s_delay_us);
    bb_scl_high();
    busy_wait_us_32(s_delay_us);
    bb_scl_low();
    bb_sda_high();
    busy_wait_us_32(s_delay_us);

    return data;
}

/* ================= 2. 核心导出接口 ================= */

bool fun_i2c_bus_is_idle(void) {
    return (gpio_get(PIN_I2C_SDA) != 0) && (gpio_get(PIN_I2C_SCL) != 0);
}

void fun_i2c_init(uint32_t baud) {
    s_cur_baud = baud ? baud : I2C_DEFAULT_BAUD;
    s_delay_us = (s_cur_baud >= 400000) ? 1 : 5;

    gpio_init(PIN_I2C_SDA);
    gpio_init(PIN_I2C_SCL);
    bb_sda_high();
    bb_scl_high();
}

void fun_i2c_set_baud(uint32_t baud) {
    if (!baud) return;
    s_cur_baud = baud;
    s_delay_us = (s_cur_baud >= 400000) ? 1 : 5;
}

uint32_t fun_i2c_get_baud(void) {
    return s_cur_baud;
}

bool fun_i2c_probe(uint8_t addr) {
    bb_start();
    bool ack = bb_write_byte((addr << 1) | 0x00);
    bb_stop();
    return ack;
}

int fun_i2c_scan(uint8_t found_addrs[], int max_count) {
    fun_i2c_init(s_cur_baud);
    int count = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (fun_i2c_probe(addr)) {
            if (found_addrs && count < max_count) {
                found_addrs[count] = addr;
            }
            count++;
        }
    }
    return count;
}

const char* fun_i2c_lookup_name(uint8_t addr) {
    for (size_t i = 0; i < sizeof(k_known_chips) / sizeof(k_known_chips[0]); i++) {
        if (k_known_chips[i].addr == addr) {
            return k_known_chips[i].name;
        }
    }
    return "未知设备";
}

int fun_i2c_write_raw(uint8_t addr, const uint8_t *data, size_t len) {
    if (!data || len == 0) return -1;

    bb_start();
    if (!bb_write_byte((addr << 1) | 0x00)) {
        bb_stop();
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (!bb_write_byte(data[i])) {
            bb_stop();
            return -1;
        }
    }
    bb_stop();
    return (int)len;
}

int fun_i2c_read_raw(uint8_t addr, uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    bb_start();
    if (!bb_write_byte((addr << 1) | 0x01)) {
        bb_stop();
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = bb_read_byte(i < len - 1);
    }
    bb_stop();
    return (int)len;
}

int fun_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t pkt[2] = { reg, val };
    return (fun_i2c_write_raw(addr, pkt, 2) == 2) ? 0 : -1;
}

int fun_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val) {
    if (!val) return -1;

    bb_start();
    if (!bb_write_byte((addr << 1) | 0x00) || !bb_write_byte(reg)) {
        bb_stop();
        return -1;
    }
    bb_start();
    if (!bb_write_byte((addr << 1) | 0x01)) {
        bb_stop();
        return -1;
    }
    *val = bb_read_byte(false);
    bb_stop();
    return 0;
}

size_t fun_i2c_parse_hex(const char *str, uint8_t *out_buf, size_t max_len) {
    if (!str || !out_buf || max_len == 0) return 0;
    size_t count = 0;
    const char *p = str;

    while (*p && count < max_len) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;

        char *endp = NULL;
        unsigned long val = strtoul(p, &endp, 16);
        if (endp == p) break;

        out_buf[count++] = (uint8_t)(val & 0xFF);
        p = endp;
    }
    return count;
}

int fun_i2c_send_hex(uint8_t addr, const char *hex_str) {
    uint8_t buf[64];
    size_t len = fun_i2c_parse_hex(hex_str, buf, sizeof(buf));
    if (len == 0) return -1;
    return fun_i2c_write_raw(addr, buf, len);
}