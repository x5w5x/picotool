#include "app_i2c_spi.h"
#include "app_manager.h"
#include "ui_draw.h"
#include "ui_btn.h"
#include "ui_tabview.h"
#include "ui_stepper.h"
#include "lcd.h"
#include "chinese.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

// H1 排针定义
#define PIN_I2C_SDA     4  // H1 Pin 9
#define PIN_I2C_SCL     5  // H1 Pin 10

#define PIN_SPI_MISO    4  // H1 Pin 9  (RX)
#define PIN_SPI_CS      5  // H1 Pin 10 (CS)
#define PIN_SPI_SCK     6  // H1 Pin 11 (SCK)
#define PIN_SPI_MOSI    7  // H1 Pin 12 (TX)

#define MAX_FOUND_I2C   8

enum {
    TAB_BUS_I2C_SCAN = 0, // I2C 拓扑扫描
    TAB_BUS_I2C_RW   = 1, // I2C 读写与 OLED 测试
    TAB_BUS_SPI_SCAN = 2, // SPI Flash 探测
    TAB_BUS_PINOUT   = 3, // 接线说明
    TAB_BUS_COUNT    = 4
};

static const char *s_bus_tabs[] = {
    "I2C扫描",
    "I2C读写",
    "SPI探测",
    "接线说明"
};

typedef struct {
    uint8_t addr;
    char    chip_name[32];
} i2c_dev_info_t;

typedef struct {
    ui_rect_t       dirty_rect;     // ⚠️ 必须放在第 1 位
    ui_tabview_t    tabview;        // 侧边栏
    uint8_t         last_tab_idx;   // 上次 Tab 索引

    // Tab 0: I2C 扫描
    uint32_t        i2c_delay_us;
    uint8_t         i2c_found_cnt;
    i2c_dev_info_t  i2c_devs[MAX_FOUND_I2C];
    bool            i2c_scanned;
    ui_rect_t       i2c_card;
    ui_btn_t        btn_i2c_scan;
    ui_btn_t        btn_i2c_speed;

    // Tab 1: I2C 读写 (使用十进制表示)
    int32_t         rw_dev_addr;    // 从机十进制地址 (如 60 对应 0x3C)
    int32_t         rw_reg_addr;    // 寄存器地址
    int32_t         rw_val_byte;    // 写入数值
    uint8_t         rw_buf[16];
    bool            rw_success;
    bool            rw_executed;
    char            rw_msg[48];

    ui_stepper_t    stepper_dev_addr;
    ui_stepper_t    stepper_reg_addr;
    ui_stepper_t    stepper_val_byte;
    ui_btn_t        btn_exec_read;
    ui_btn_t        btn_exec_write;
    ui_btn_t        btn_oled_all_on;   // 全屏点亮
    ui_btn_t        btn_oled_all_off;  // 恢复显存
    ui_rect_t       rw_card;

    // Tab 2: SPI 探测
    bool            spi_detected;
    uint8_t         jedec_raw[3];
    uint8_t         uid_raw[8];
    char            spi_manuf[32];
    char            spi_capacity[32];
    uint32_t        spi_speed_hz;
    ui_rect_t       spi_card;
    ui_btn_t        btn_spi_probe;
    ui_btn_t        btn_spi_speed;
} app_bus_data_t;

static app_bus_data_t s_bus_data;
static app_t          s_app_bus;

static inline ui_rect_t rect_union_box(ui_rect_t a, ui_rect_t b) {
    if (a.w == 0 || a.h == 0) return b;
    if (b.w == 0 || b.h == 0) return a;
    int16_t x1 = (a.x < b.x) ? a.x : b.x;
    int16_t y1 = (a.y < b.y) ? a.y : b.y;
    int16_t x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int16_t y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

/* ================= 1. GPIO 开漏模拟 I2C 底层 ================= */

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

static void bb_i2c_init_pins(void) {
    gpio_init(PIN_I2C_SDA);
    gpio_init(PIN_I2C_SCL);
    bb_sda_high();
    bb_scl_high();
}

static void bb_i2c_start(uint32_t delay_us) {
    bb_sda_high();
    bb_scl_high();
    busy_wait_us_32(delay_us);
    bb_sda_low();
    busy_wait_us_32(delay_us);
    bb_scl_low();
    busy_wait_us_32(delay_us);
}

static void bb_i2c_stop(uint32_t delay_us) {
    bb_sda_low();
    busy_wait_us_32(delay_us);
    bb_scl_high();
    busy_wait_us_32(delay_us);
    bb_sda_high();
    busy_wait_us_32(delay_us);
}

static bool bb_i2c_write_byte(uint8_t data, uint32_t delay_us) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) {
            bb_sda_high();
        } else {
            bb_sda_low();
        }
        busy_wait_us_32(delay_us);
        bb_scl_high();
        busy_wait_us_32(delay_us);
        bb_scl_low();
        busy_wait_us_32(delay_us);
    }

    bb_sda_high();
    busy_wait_us_32(delay_us);
    bb_scl_high();
    busy_wait_us_32(delay_us);
    bool ack = (bb_sda_read() == 0);
    bb_scl_low();
    busy_wait_us_32(delay_us);

    return ack;
}

static uint8_t bb_i2c_read_byte(bool send_ack, uint32_t delay_us) {
    uint8_t data = 0;
    bb_sda_high();

    for (int i = 7; i >= 0; i--) {
        busy_wait_us_32(delay_us);
        bb_scl_high();
        busy_wait_us_32(delay_us);
        if (bb_sda_read()) {
            data |= (1 << i);
        }
        bb_scl_low();
        busy_wait_us_32(delay_us);
    }

    if (send_ack) {
        bb_sda_low();
    } else {
        bb_sda_high();
    }
    busy_wait_us_32(delay_us);
    bb_scl_high();
    busy_wait_us_32(delay_us);
    bb_scl_low();
    bb_sda_high();
    busy_wait_us_32(delay_us);

    return data;
}

/* ================= 2. SPI 驱动 ================= */

static void bus_init_spi(app_bus_data_t *data) {
    if (data->spi_speed_hz < 100000) data->spi_speed_hz = 10000000;

    spi_init(spi0, data->spi_speed_hz);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);

    gpio_init(PIN_SPI_CS);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    gpio_put(PIN_SPI_CS, 1);
}

static const char *identify_i2c_chip(uint8_t addr) {
    switch (addr) {
        case 0x3C: case 0x3D: return "SSD1306 OLED (60)";
        case 0x68: return "MPU6050/DS3231 (104)";
        case 0x69: return "MPU6050(AD0=1) (105)";
        case 0x44: case 0x45: return "SHT30/SHT31 (68)";
        case 0x76: case 0x77: return "BMP280/BME280 (118)";
        case 0x38: return "FT6336/AHT10 (56)";
        case 0x20: case 0x21: case 0x27: return "PCF8574 (32/39)";
        case 0x50: case 0x51: case 0x57: return "AT24Cxx EEPROM (80)";
        case 0x1E: return "HMC5883L 罗盘 (30)";
        case 0x48: case 0x49: return "ADS1115 ADC (72)";
        case 0x5A: return "MLX90614 红外 (90)";
        default:   return "I2C 从机设备";
    }
}

static void do_i2c_scan(app_bus_data_t *data) {
    bb_i2c_init_pins();
    data->i2c_found_cnt = 0;
    data->i2c_scanned   = true;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        bb_i2c_start(data->i2c_delay_us);
        bool ack = bb_i2c_write_byte((addr << 1) | 0x00, data->i2c_delay_us);
        bb_i2c_stop(data->i2c_delay_us);

        if (ack) {
            if (data->i2c_found_cnt < MAX_FOUND_I2C) {
                data->i2c_devs[data->i2c_found_cnt].addr = addr;
                strncpy(data->i2c_devs[data->i2c_found_cnt].chip_name,
                        identify_i2c_chip(addr), sizeof(data->i2c_devs[0].chip_name));
                data->i2c_found_cnt++;
            }
        }
    }
}

static bool do_i2c_write_bytes(app_bus_data_t *data, uint8_t dev, const uint8_t *bytes, uint8_t len) {
    bb_i2c_init_pins();
    bb_i2c_start(data->i2c_delay_us);
    if (!bb_i2c_write_byte((dev << 1) | 0x00, data->i2c_delay_us)) {
        bb_i2c_stop(data->i2c_delay_us);
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        if (!bb_i2c_write_byte(bytes[i], data->i2c_delay_us)) {
            bb_i2c_stop(data->i2c_delay_us);
            return false;
        }
    }
    bb_i2c_stop(data->i2c_delay_us);
    return true;
}

static void do_i2c_read_reg(app_bus_data_t *data) {
    bb_i2c_init_pins();
    uint8_t dev = (uint8_t)data->rw_dev_addr;
    uint8_t reg = (uint8_t)data->rw_reg_addr;

    data->rw_executed = true;
    data->rw_success  = false;
    memset(data->rw_buf, 0, sizeof(data->rw_buf));

    if (dev == 60 || dev == 61) { // 0x3C / 0x3D
        bb_i2c_start(data->i2c_delay_us);
        if (bb_i2c_write_byte((dev << 1) | 0x01, data->i2c_delay_us)) {
            data->rw_buf[0] = bb_i2c_read_byte(false, data->i2c_delay_us);
            data->rw_success = true;
            snprintf(data->rw_msg, sizeof(data->rw_msg), "SSD1306 状态字节: 0x%02X", data->rw_buf[0]);
        } else {
            strcpy(data->rw_msg, "SSD1306 只写屏无法读取 (正常现象)");
        }
        bb_i2c_stop(data->i2c_delay_us);
        return;
    }

    bb_i2c_start(data->i2c_delay_us);
    if (!bb_i2c_write_byte((dev << 1) | 0x00, data->i2c_delay_us) ||
        !bb_i2c_write_byte(reg, data->i2c_delay_us)) {
        bb_i2c_stop(data->i2c_delay_us);
        strcpy(data->rw_msg, "写寄存器失败 (NACK)");
        return;
    }

    bb_i2c_start(data->i2c_delay_us);
    if (!bb_i2c_write_byte((dev << 1) | 0x01, data->i2c_delay_us)) {
        bb_i2c_stop(data->i2c_delay_us);
        strcpy(data->rw_msg, "读从机应答失败 (NACK)");
        return;
    }

    data->rw_buf[0] = bb_i2c_read_byte(false, data->i2c_delay_us);
    bb_i2c_stop(data->i2c_delay_us);
    data->rw_success = true;
    snprintf(data->rw_msg, sizeof(data->rw_msg), "读 0x%02X[0x%02X] 成功: 0x%02X", dev, reg, data->rw_buf[0]);
}

static void do_spi_probe(app_bus_data_t *data) {
    bus_init_spi(data);
    memset(data->jedec_raw, 0, sizeof(data->jedec_raw));
    memset(data->uid_raw, 0, sizeof(data->uid_raw));

    gpio_put(PIN_SPI_CS, 0);
    uint8_t cmd = 0x9F;
    spi_write_blocking(spi0, &cmd, 1);
    spi_read_blocking(spi0, 0x00, data->jedec_raw, 3);
    gpio_put(PIN_SPI_CS, 1);

    gpio_put(PIN_SPI_CS, 0);
    uint8_t uid_cmd[5] = {0x4B, 0, 0, 0, 0};
    spi_write_blocking(spi0, uid_cmd, 5);
    spi_read_blocking(spi0, 0x00, data->uid_raw, 8);
    gpio_put(PIN_SPI_CS, 1);

    data->spi_detected = true;

    switch (data->jedec_raw[0]) {
        case 0xEF: strcpy(data->spi_manuf, "Winbond (华邦电子)"); break;
        case 0xC8: strcpy(data->spi_manuf, "GigaDevice (兆易创新)"); break;
        case 0x20: strcpy(data->spi_manuf, "XMC / Micron (新芯/美光)"); break;
        case 0xC2: strcpy(data->spi_manuf, "Macronix (旺宏电子)"); break;
        case 0x1F: strcpy(data->spi_manuf, "Adesto / Atmel"); break;
        case 0x00: case 0xFF:
            strcpy(data->spi_manuf, "未检测到芯片 / 无响应");
            strcpy(data->spi_capacity, "请检查 H1 接线与电源");
            return;
        default:
            snprintf(data->spi_manuf, sizeof(data->spi_manuf), "厂商代码: 0x%02X", data->jedec_raw[0]);
            break;
    }

    uint8_t cap_code = data->jedec_raw[2];
    if (cap_code >= 0x11 && cap_code <= 0x1B) {
        uint32_t bytes = 1UL << cap_code;
        uint32_t mb = bytes / (1024 * 1024);
        snprintf(data->spi_capacity, sizeof(data->spi_capacity), "W25Q%lu 系列 (%lu MB / %lu Mbit)", mb * 8, mb, mb * 8);
    } else {
        snprintf(data->spi_capacity, sizeof(data->spi_capacity), "容量代码: 0x%02X", cap_code);
    }
}

/* ================= 3. 交互回调 ================= */

static void on_tab_changed(ui_tabview_t *tv, uint8_t old_tab, uint8_t new_tab, void *user_data) {
    (void)tv; (void)old_tab;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    if (new_tab == TAB_BUS_I2C_SCAN || new_tab == TAB_BUS_I2C_RW) {
        bb_i2c_init_pins();
    } else if (new_tab == TAB_BUS_SPI_SCAN) {
        bus_init_spi(data);
    }
}

static void on_i2c_scan_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    do_i2c_scan(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_i2c_speed_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    if (data->i2c_delay_us == 5) {
        data->i2c_delay_us = 1;
        data->btn_i2c_speed.text = "400kHz (快速)";
    } else {
        data->i2c_delay_us = 5;
        data->btn_i2c_speed.text = "100kHz (标准)";
    }
    data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_i2c_speed));
}

static void on_stepper_dev_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    data->rw_dev_addr = val;
    data->dirty_rect = data->rw_card;
}

static void on_stepper_reg_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    data->rw_reg_addr = val;
    data->dirty_rect = data->rw_card;
}

static void on_stepper_val_change(ui_stepper_t *sp, int32_t val, void *user_data) {
    (void)sp;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    data->rw_val_byte = val;
}

static void on_exec_read_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    do_i2c_read_reg(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_exec_write_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    uint8_t payload[2] = {(uint8_t)data->rw_reg_addr, (uint8_t)data->rw_val_byte};
    data->rw_executed = true;
    data->rw_success = do_i2c_write_bytes(data, (uint8_t)data->rw_dev_addr, payload, 2);

    if (data->rw_success) {
        snprintf(data->rw_msg, sizeof(data->rw_msg), "写入成功: %ld <- [%ld, %ld]",
                 (long)data->rw_dev_addr, (long)payload[0], (long)payload[1]);
    } else {
        strcpy(data->rw_msg, "写入失败: 从机未应答 (NACK)");
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_oled_all_on_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    uint8_t cmds[] = {0x00, 0xAF, 0x00, 0xA5};
    data->rw_executed = true;
    data->rw_success = do_i2c_write_bytes(data, (uint8_t)data->rw_dev_addr, cmds, sizeof(cmds));
    if (data->rw_success) {
        strcpy(data->rw_msg, "已发送: 开启显示并全屏点亮 (0xA5)");
    } else {
        strcpy(data->rw_msg, "点亮失败: 未收到 60(0x3C) 应答");
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_oled_all_off_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    uint8_t cmds[] = {0x00, 0xA4};
    data->rw_executed = true;
    data->rw_success = do_i2c_write_bytes(data, (uint8_t)data->rw_dev_addr, cmds, sizeof(cmds));
    if (data->rw_success) {
        strcpy(data->rw_msg, "已发送: 恢复正常显存显示 (0xA4)");
    } else {
        strcpy(data->rw_msg, "恢复失败: 从机未应答 (NACK)");
    }
    data->dirty_rect = data->tabview.content_rect;
}

static void on_spi_probe_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    do_spi_probe(data);
    data->dirty_rect = data->tabview.content_rect;
}

static void on_spi_speed_click(ui_btn_t *btn, void *user_data) {
    (void)btn;
    app_bus_data_t *data = (app_bus_data_t *)user_data;
    if (data->spi_speed_hz == 10000000) {
        data->spi_speed_hz = 20000000;
        data->btn_spi_speed.text = "20MHz";
    } else if (data->spi_speed_hz == 20000000) {
        data->spi_speed_hz = 1000000;
        data->btn_spi_speed.text = "1MHz";
    } else {
        data->spi_speed_hz = 10000000;
        data->btn_spi_speed.text = "10MHz";
    }
    bus_init_spi(data);
    data->dirty_rect = rect_union_box(data->dirty_rect, ui_btn_get_dirty_rect(&data->btn_spi_speed));
}

/* ================= 4. 生命周期管理 ================= */

static void app_bus_init(app_t *self) {
    self->user_data = &s_bus_data;
    app_bus_data_t *data = &s_bus_data;
    memset(data, 0, sizeof(app_bus_data_t));

    data->dirty_rect   = ui_rect_make(0, 0, 0, 0);
    data->last_tab_idx = 0;

    data->i2c_delay_us = 5; // 100kHz
    data->rw_dev_addr  = 60; // 默认 60 (对应 0x3C OLED)
    data->rw_reg_addr  = 0;
    data->rw_val_byte  = 165; // 对应 0xA5 全屏点亮
    data->spi_speed_hz = 10000000;

    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_bus_tabs, TAB_BUS_COUNT, 0, on_tab_changed);
    data->tabview.user_data = data;

    // Tab 0: I2C 扫描
    data->i2c_card = ui_rect_make(120, 40, 350, 196);
    ui_btn_init(&data->btn_i2c_scan, 120, 244, 170, 36, "一键扫描 I2C", on_i2c_scan_click);
    data->btn_i2c_scan.user_data = data; data->btn_i2c_scan.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_i2c_scan, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_i2c_speed, 298, 244, 172, 36, "100kHz (标准)", on_i2c_speed_click);
    data->btn_i2c_speed.user_data = data; data->btn_i2c_speed.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_i2c_speed, 0x03DF, 0x0275, 0xFFFF);

    // Tab 1: I2C 读写与测试
    ui_stepper_init(&data->stepper_dev_addr, 120, 40, 110, 34, 8, 119, 60, 1, "", on_stepper_dev_change);//: 1, 2]
    ui_stepper_set_prefix(&data->stepper_dev_addr, "地址:");//: 1]
    data->stepper_dev_addr.user_data = data;

    ui_stepper_init(&data->stepper_reg_addr, 236, 40, 114, 34, 0, 255, 0, 1, "", on_stepper_reg_change);//: 1, 2]
    ui_stepper_set_prefix(&data->stepper_reg_addr, "寄存:");//: 1]
    data->stepper_reg_addr.user_data = data;

    ui_stepper_init(&data->stepper_val_byte, 356, 40, 114, 34, 0, 255, 165, 1, "", on_stepper_val_change);//: 1, 2]
    ui_stepper_set_prefix(&data->stepper_val_byte, "数值:");//: 1]
    data->stepper_val_byte.user_data = data;

    ui_btn_init(&data->btn_exec_read, 120, 80, 170, 34, "读状态/寄存器", on_exec_read_click);
    data->btn_exec_read.user_data = data; data->btn_exec_read.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_exec_read, 0x03DF, 0x0275, 0xFFFF);

    ui_btn_init(&data->btn_exec_write, 298, 80, 172, 34, "写 [寄存, 数值]", on_exec_write_click);
    data->btn_exec_write.user_data = data; data->btn_exec_write.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_exec_write, 0x05E0, 0x03E0, 0xFFFF);

    data->rw_card = ui_rect_make(120, 120, 350, 114);

    ui_btn_init(&data->btn_oled_all_on, 120, 242, 170, 36, "OLED 全亮 (0xA5)", on_oled_all_on_click);
    data->btn_oled_all_on.user_data = data; data->btn_oled_all_on.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_oled_all_on, 0xFA00, 0xC800, 0xFFFF);

    ui_btn_init(&data->btn_oled_all_off, 298, 242, 172, 36, "恢复显存 (0xA4)", on_oled_all_off_click);
    data->btn_oled_all_off.user_data = data; data->btn_oled_all_off.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_oled_all_off, 0x03DF, 0x0275, 0xFFFF);

    // Tab 2: SPI 探测
    data->spi_card = ui_rect_make(120, 40, 350, 194);
    ui_btn_init(&data->btn_spi_probe, 120, 242, 230, 38, "读取 SPI Flash ID", on_spi_probe_click);
    data->btn_spi_probe.user_data = data; data->btn_spi_probe.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_spi_probe, 0x05E0, 0x03E0, 0xFFFF);

    ui_btn_init(&data->btn_spi_speed, 358, 242, 112, 38, "10MHz", on_spi_speed_click);
    data->btn_spi_speed.user_data = data; data->btn_spi_speed.style = UI_BTN_STYLE_ROUND;
    ui_btn_set_color(&data->btn_spi_speed, 0x03DF, 0x0275, 0xFFFF);
}

static void app_bus_enter(app_t *self) {
    app_bus_data_t *data = (app_bus_data_t *)self->user_data;

    if (data->i2c_delay_us == 0) data->i2c_delay_us = 5;
    if (data->spi_speed_hz == 0) data->spi_speed_hz = 10000000;

    data->dirty_rect   = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
    data->last_tab_idx = data->tabview.active_tab_idx;

    if (data->tabview.active_tab_idx == TAB_BUS_SPI_SCAN) {
        bus_init_spi(data);
    } else {
        bb_i2c_init_pins();
    }
}

static void app_bus_exit(app_t *self) {
    (void)self;
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_NULL);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_NULL);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_NULL);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_NULL);
}

static void app_bus_update(app_t *self, uint32_t now_tick) {
    (void)now_tick;
    app_bus_data_t *data = (app_bus_data_t *)self->user_data;
    data->dirty_rect = ui_rect_make(0, 0, 0, 0);
}

static bool app_bus_process_touch(app_t *self, bool is_touching, int16_t tx, int16_t ty, uint32_t now_tick) {
    app_bus_data_t *data = (app_bus_data_t *)self->user_data;

    if (ui_tabview_process_touch(&data->tabview, is_touching, tx, ty, now_tick)) {
        if (data->tabview.active_tab_idx != data->last_tab_idx) {
            data->last_tab_idx = data->tabview.active_tab_idx;
            data->dirty_rect   = data->tabview.rect;
        } else {
            data->dirty_rect   = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }
        return true;
    }

    switch (data->tabview.active_tab_idx) {
        case TAB_BUS_I2C_SCAN:
            if (ui_btn_process_touch(&data->btn_i2c_scan, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_i2c_speed, is_touching, tx, ty, now_tick)) return true;
            break;

        case TAB_BUS_I2C_RW:
            if (ui_stepper_process_touch(&data->stepper_dev_addr, is_touching, tx, ty, now_tick)) {//: 1]
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_dev_addr));//: 2]
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_reg_addr, is_touching, tx, ty, now_tick)) {//: 1]
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_reg_addr));//: 2]
                return true;
            }
            if (ui_stepper_process_touch(&data->stepper_val_byte, is_touching, tx, ty, now_tick)) {//: 1]
                data->dirty_rect = rect_union_box(data->dirty_rect, ui_stepper_get_dirty_rect(&data->stepper_val_byte));//: 2]
                return true;
            }
            if (ui_btn_process_touch(&data->btn_exec_read, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_exec_write, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_oled_all_on, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_oled_all_off, is_touching, tx, ty, now_tick)) return true;
            break;

        case TAB_BUS_SPI_SCAN:
            if (ui_btn_process_touch(&data->btn_spi_probe, is_touching, tx, ty, now_tick)) return true;
            if (ui_btn_process_touch(&data->btn_spi_speed, is_touching, tx, ty, now_tick)) return true;
            break;
    }

    return (data->dirty_rect.w > 0 && data->dirty_rect.h > 0);
}

/* ================= 5. 切片光栅化渲染 (无任何边缘溢出) ================= */

static void app_bus_draw(app_t *self, ui_tile_t *tile) {
    app_bus_data_t *data = (app_bus_data_t *)self->user_data;
    ui_rect_t overlap;

    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    if (ui_rect_intersect(&tile->rect, &body, &overlap)) {
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    }

    ui_tabview_draw(tile, &data->tabview);

    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) {
        return;
    }

    char str_buf[64];

    switch (data->tabview.active_tab_idx) {
        case TAB_BUS_I2C_SCAN: {
            if (ui_rect_intersect(&tile->rect, &data->i2c_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->i2c_card, 0xC618);

                if (!data->i2c_scanned) {
                    ui_draw_string(tile, 130, 52, "I2C 拓扑扫描 (SDA:GP4 / SCL:GP5)", FONT_16, 0x1A7B, 0xEF7D);
                    ui_draw_string(tile, 130, 88, "点击下方按钮开始遍历 8 ~ 119 地址", FONT_16, 0x4208, 0xEF7D);
                    ui_draw_string(tile, 130, 120, "支持 OLED、MPU6050、SHT30 芯片识别", FONT_16, 0x4208, 0xEF7D);
                } else {
                    snprintf(str_buf, sizeof(str_buf), "扫描完成: 发现 %u 个 I2C 设备", data->i2c_found_cnt);
                    ui_draw_string(tile, 130, 48, str_buf, FONT_16, data->i2c_found_cnt > 0 ? 0x05E0 : 0xF800, 0xEF7D);

                    if (data->i2c_found_cnt == 0) {
                        ui_draw_string(tile, 130, 80, "未发现任何从机应答 (NACK)", FONT_16, 0x8410, 0xEF7D);
                        ui_draw_string(tile, 130, 108, "请确认传感器已供电并正确共地", FONT_16, 0x4208, 0xEF7D);
                    } else {
                        for (uint8_t i = 0; i < data->i2c_found_cnt && i < 5; i++) {
                            snprintf(str_buf, sizeof(str_buf), "- 地址 %u (0x%02X): %s",
                                     data->i2c_devs[i].addr, data->i2c_devs[i].addr, data->i2c_devs[i].chip_name);
                            ui_draw_string(tile, 130, 76 + i * 22, str_buf, FONT_16, 0x03DF, 0xEF7D);
                        }
                    }
                }
            }
            ui_btn_draw(tile, &data->btn_i2c_scan);
            ui_btn_draw(tile, &data->btn_i2c_speed);
            break;
        }

        case TAB_BUS_I2C_RW: {
            ui_stepper_draw(tile, &data->stepper_dev_addr);//: 1]
            ui_stepper_draw(tile, &data->stepper_reg_addr);//: 1]
            ui_stepper_draw(tile, &data->stepper_val_byte);//: 1]
            ui_btn_draw(tile, &data->btn_exec_read);
            ui_btn_draw(tile, &data->btn_exec_write);

            if (ui_rect_intersect(&tile->rect, &data->rw_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0x10A2);
                ui_draw_rect(tile, &data->rw_card, 0x07FF);

                // 显示十进制与十六进制双向对照
                snprintf(str_buf, sizeof(str_buf), "目标: 从机 %ld(0x%02X)  Reg %ld(0x%02X)",
                         (long)data->rw_dev_addr, (uint8_t)data->rw_dev_addr,
                         (long)data->rw_reg_addr, (uint8_t)data->rw_reg_addr);
                ui_draw_string(tile, 130, 128, str_buf, FONT_16, 0x07FF, 0x10A2);

                if (!data->rw_executed) {
                    ui_draw_string(tile, 130, 154, "OLED 测试: 设地址为 60 点击「OLED 全亮」", FONT_16, 0x8410, 0x10A2);
                    ui_draw_string(tile, 130, 180, "传感器测试: 设地址与 Reg 后点击读取", FONT_16, 0x8410, 0x10A2);
                } else {
                    ui_draw_string(tile, 130, 154, data->rw_msg, FONT_16, data->rw_success ? 0x05E0 : 0xF800, 0x10A2);

                    if (data->rw_success && data->rw_dev_addr != 60 && data->rw_dev_addr != 61) {
                        snprintf(str_buf, sizeof(str_buf), "读取值: %u (0x%02X) | 二进制: %c%c%c%c_%c%c%c%c",
                                 data->rw_buf[0], data->rw_buf[0],
                                 (data->rw_buf[0] & 0x80) ? '1':'0', (data->rw_buf[0] & 0x40) ? '1':'0',
                                 (data->rw_buf[0] & 0x20) ? '1':'0', (data->rw_buf[0] & 0x10) ? '1':'0',
                                 (data->rw_buf[0] & 0x08) ? '1':'0', (data->rw_buf[0] & 0x04) ? '1':'0',
                                 (data->rw_buf[0] & 0x02) ? '1':'0', (data->rw_buf[0] & 0x01) ? '1':'0');
                        ui_draw_string(tile, 130, 180, str_buf, FONT_16, 0xFFFF, 0x10A2);
                    }
                }
            }
            ui_btn_draw(tile, &data->btn_oled_all_on);
            ui_btn_draw(tile, &data->btn_oled_all_off);
            break;
        }

        case TAB_BUS_SPI_SCAN: {
            if (ui_rect_intersect(&tile->rect, &data->spi_card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &data->spi_card, 0xC618);

                if (!data->spi_detected) {
                    ui_draw_string(tile, 130, 52, "SPI 闪存探测 (W25Qxx JEDEC ID)", FONT_16, 0x1A7B, 0xEF7D);
                    ui_draw_string(tile, 130, 80, "接线: SCK:GP6 CS:GP5 MOSI:GP7 MISO:GP4", FONT_16, 0x05E0, 0xEF7D);
                    ui_draw_string(tile, 130, 110, "点击下方按钮读取 0x9F JEDEC 与 UID", FONT_16, 0x4208, 0xEF7D);
                } else {
                    ui_draw_string(tile, 130, 48, "SPI NOR Flash 探测结果:", FONT_16, 0x1A7B, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "厂商识别: %s", data->spi_manuf);
                    ui_draw_string(tile, 130, 74, str_buf, FONT_16, 0x03DF, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "器件容量: %s", data->spi_capacity);
                    ui_draw_string(tile, 130, 98, str_buf, FONT_16, 0x05E0, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "JEDEC ID: %02X %02X %02X (Hex)",
                             data->jedec_raw[0], data->jedec_raw[1], data->jedec_raw[2]);
                    ui_draw_string(tile, 130, 122, str_buf, FONT_16, 0x1A7B, 0xEF7D);

                    snprintf(str_buf, sizeof(str_buf), "唯一 UID: %02X%02X%02X%02X%02X%02X%02X%02X",
                             data->uid_raw[0], data->uid_raw[1], data->uid_raw[2], data->uid_raw[3],
                             data->uid_raw[4], data->uid_raw[5], data->uid_raw[6], data->uid_raw[7]);
                    ui_draw_string(tile, 130, 146, str_buf, FONT_16, 0x8410, 0xEF7D);
                }
            }
            ui_btn_draw(tile, &data->btn_spi_probe);
            ui_btn_draw(tile, &data->btn_spi_speed);
            break;
        }

        case TAB_BUS_PINOUT: { // ⚡ 彻底优化排版，防止右侧溢出
            ui_rect_t card = ui_rect_make(120, 40, 350, 240);
            if (ui_rect_intersect(&tile->rect, &card, &overlap)) {
                ui_draw_fill_rect(tile, &overlap, 0xEF7D);
                ui_draw_rect(tile, &card, 0xC618);

                ui_draw_string(tile, 130, 48, "H1 排针总线引脚对照表:", FONT_16, 0x03DF, 0xEF7D);

                // I2C 接线
                ui_draw_string(tile, 130, 74, "[I2C]  SDA: Pin 9(GP4)  SCL: Pin 10(GP5)", FONT_16, 0x1A7B, 0xEF7D);

                // SPI 接线 (拆分为两行显示)
                ui_draw_string(tile, 130, 102, "[SPI]  SCK: Pin 11(GP6)  CS: Pin 10(GP5)", FONT_16, 0x05E0, 0xEF7D);
                ui_draw_string(tile, 130, 126, "[SPI]  MOSI:Pin 12(GP7) MISO:Pin 9(GP4)", FONT_16, 0x05E0, 0xEF7D);

                // 电源接线
                ui_draw_string(tile, 130, 156, "[电源] 3.3V: Pin 2  5V: Pin 1  GND: Pin 3/4", FONT_16, 0xF800, 0xEF7D);

                ui_draw_string(tile, 130, 188, "注意: SPI 与 I2C 共用 GP4/GP5 引脚", FONT_16, 0x8410, 0xEF7D);
                ui_draw_string(tile, 130, 212, "切换功能前请确认外接线缆正确", FONT_16, 0x8410, 0xEF7D);
            }
            break;
        }
    }
}

/* ================= 6. 导出单例实体 ================= */

app_t *app_i2c_spi_get_instance(void) {
    s_app_bus.id            = APP_ID_CUSTOM_0 + 4;
    s_app_bus.name          = "总线分析";
    s_app_bus.init          = app_bus_init;
    s_app_bus.enter         = app_bus_enter;
    s_app_bus.exit          = app_bus_exit;
    s_app_bus.update        = app_bus_update;
    s_app_bus.process_touch = app_bus_process_touch;
    s_app_bus.draw          = app_bus_draw;
    return &s_app_bus;
}