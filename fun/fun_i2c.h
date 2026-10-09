/*
 * @Author: 轩
 * @Date: 2026-10-09 14:47:31
 * @LastEditTime: 2026-10-09 18:18:18
 * @FilePath: \pico\fun\fun_i2c.h
 */
#ifndef FUN_I2C_H
#define FUN_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PIN_I2C_SDA         4  // H1 Pin 9
#define PIN_I2C_SCL         5  // H1 Pin 10
#define I2C_DEFAULT_BAUD    100000

typedef struct {
    uint8_t     addr;
    const char *name;
    const char *desc;
} i2c_chip_info_t;

// 驱动生命周期与状态查询
void        fun_i2c_init(uint32_t baud);
void        fun_i2c_set_baud(uint32_t baud);
uint32_t    fun_i2c_get_baud(void);
bool        fun_i2c_bus_is_idle(void);

// 总线探测与芯片库匹配
int         fun_i2c_scan(uint8_t found_addrs[], int max_count);
bool        fun_i2c_probe(uint8_t addr);
const char* fun_i2c_lookup_name(uint8_t addr);

// 通用物理层数据流收发
int         fun_i2c_write_raw(uint8_t addr, const uint8_t *data, size_t len);
int         fun_i2c_read_raw(uint8_t addr, uint8_t *buf, size_t len);

// 寄存器操作
int         fun_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val);
int         fun_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val);

// HEX 字符串辅助解析与发送
size_t      fun_i2c_parse_hex(const char *str, uint8_t *out_buf, size_t max_len);
int         fun_i2c_send_hex(uint8_t addr, const char *hex_str);

#endif // FUN_I2C_H