#include "touch.h"
#include "board_pins.h"
#include "lcd.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#define TOUCH_I2C       i2c0
#define TOUCH_I2C_BAUD  (400 * 1000) //iic时钟频率

#define TOUCH_RST_LOW()  gpio_put(PIN_TOUCH_RST, 0)
#define TOUCH_RST_HIGH() gpio_put(PIN_TOUCH_RST, 1)

static volatile bool g_has_touch_irq = false;

/* EMA 滤波与滑动速度估计历史队列 */
static uint16_t filter_x = 0;
static uint16_t filter_y = 0;
static bool     is_first_touch_point = true;

static int16_t  history_y[4];
static uint32_t history_time[4];
static uint8_t  history_idx = 0;

// uint8_t Touch_ReadReg(uint8_t reg)
// {
//     uint8_t val = 0xFF;
//     int ret = i2c_write_timeout_us(TOUCH_I2C, FT6336_ADDR, &reg, 1, true, 10000);
//     if (ret < 0) return 0xFF;
//     ret = i2c_read_timeout_us(TOUCH_I2C, FT6336_ADDR, &val, 1, false, 10000);
//     if (ret < 0) return 0xFF;
//     return val;
// }

void Touch_WriteReg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    i2c_write_timeout_us(TOUCH_I2C, FT6336_ADDR, buf, 2, false, 10000);
}

static void Touch_HardwareReset(void)
{
    TOUCH_RST_LOW();
    sleep_ms(15);
    TOUCH_RST_HIGH();
    sleep_ms(80);
}

static void touch_gpio_irq_handler(uint gpio, uint32_t events)
{
    if (gpio == PIN_TOUCH_INT) {
        g_has_touch_irq = true;
    }
}

bool Touch_Init(void)
{
    gpio_set_function(PIN_TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_TOUCH_SDA);
    gpio_pull_up(PIN_TOUCH_SCL);

    gpio_init(PIN_TOUCH_INT);
    gpio_set_dir(PIN_TOUCH_INT, GPIO_IN);
    gpio_pull_up(PIN_TOUCH_INT);

    gpio_init(PIN_TOUCH_RST);
    gpio_set_dir(PIN_TOUCH_RST, GPIO_OUT);
    TOUCH_RST_HIGH();

    i2c_init(TOUCH_I2C, TOUCH_I2C_BAUD);
    Touch_HardwareReset();

    if (Touch_GetChipID() == 0xFF) return false;

    Touch_WriteReg(FT_REG_TH_GROUP, 0x14);     // 提升轻触灵敏度
    Touch_WriteReg(FT_REG_PERIODACTIVE, 0x06); // 6ms 极速扫描
    Touch_WriteReg(0xA4, 0x00);                // Level 中断模式

    gpio_set_irq_enabled_with_callback(PIN_TOUCH_INT, GPIO_IRQ_EDGE_FALL, true, &touch_gpio_irq_handler);
    return true;
}

// uint8_t Touch_GetChipID(void)
// {
//     return Touch_ReadReg(FT_REG_CHIP_ID);
// }

inline bool Touch_HasEvent(void)
{
    return (gpio_get(PIN_TOUCH_INT) == 0) || g_has_touch_irq;
}

bool Touch_ReadData(Touch_Data_t *data)
{
    if (!data) return false;

    if (!Touch_HasEvent()) {
        data->touch_num = 0;
        is_first_touch_point = true;
        data->vy = 0.0f;
        return false;
    }

    g_has_touch_irq = false;

    uint8_t raw[14];
    uint8_t start_reg = FT_REG_GEST_ID; //连续读14字节

    if (i2c_write_timeout_us(TOUCH_I2C, FT6336_ADDR, &start_reg, 1, true, 10000) < 0 ||
        i2c_read_timeout_us(TOUCH_I2C, FT6336_ADDR, raw, sizeof(raw), false, 10000) < 0) {
        data->touch_num = 0;
        return false;
    }

    data->gesture   = raw[0];  
    data->touch_num = raw[1] & 0x0F;

    if (data->touch_num == 0 || data->touch_num > 2) {
        data->touch_num = 0;
        is_first_touch_point = true;
        data->vy = 0.0f;
        return false;
    }

    // 触点 1 坐标解析
    uint16_t raw_x = ((uint16_t)(raw[2] & 0x0F) << 8) | raw[3];
    uint16_t raw_y = ((uint16_t)(raw[4] & 0x0F) << 8) | raw[5];

    // ⚡ 核心修改：raw_y (0~479) 映射给横屏 cur_x，raw_x (0~319) 映射给横屏 cur_y
    uint16_t cur_x = (raw_y < LCD_WIDTH)  ? (uint16_t)(LCD_WIDTH  - 1 - raw_y) : 0;
    uint16_t cur_y = (raw_x < LCD_HEIGHT) ? raw_x : (LCD_HEIGHT - 1);

    // 核心优化 1：EMA 指数移动平均滤波 (消灭手抖微颤)
    if (is_first_touch_point) {
        filter_x = cur_x;
        filter_y = cur_y;
        is_first_touch_point = false;
    } else {
        filter_x = (uint16_t)((filter_x * 3 + cur_x * 7) / 10);
        filter_y = (uint16_t)((filter_y * 3 + cur_y * 7) / 10);
    }

    data->x0 = filter_x;
    data->y0 = filter_y;

    // 核心优化 2：计算滑动瞬时速度
    uint32_t now = to_ms_since_boot(get_absolute_time());
    history_y[history_idx] = filter_y;
    history_time[history_idx] = now;
    history_idx = (history_idx + 1) % 4;

    uint8_t oldest_idx = history_idx;
    if (history_time[oldest_idx] > 0) {
        int16_t dy = (int16_t)filter_y - history_y[oldest_idx];
        uint32_t dt = now - history_time[oldest_idx];
        if (dt > 0 && dt <= 100) {
            data->vy = (float)dy / (float)dt;
        } else {
            data->vy = 0.0f;
        }
    } else {
        data->vy = 0.0f;
    }

    // 触点 2 坐标解析 (同步适配横屏)
    if (data->touch_num >= 2) {
        uint16_t x1_raw = ((uint16_t)(raw[8] & 0x0F) << 8) | raw[9];
        uint16_t y1_raw = ((uint16_t)(raw[10] & 0x0F) << 8) | raw[11];
        uint16_t x1 = (y1_raw < LCD_WIDTH)  ? (uint16_t)(LCD_WIDTH  - 1 - y1_raw) : 0;
        uint16_t y1 = (x1_raw < LCD_HEIGHT) ? x1_raw : (LCD_HEIGHT - 1);
        data->x1 = x1;
        data->y1 = y1;
    }

    return true;
}

bool Touch_IsTouched(void)
{
    Touch_Data_t dummy;
    return Touch_ReadData(&dummy);
}

uint8_t Touch_ReadReg(uint8_t reg)
{
    uint8_t val = 0xFF;
    // 将 nostop 改为 false，发送标准 STOP 信号，兼容所有批次触控 IC
    int ret = i2c_write_timeout_us(TOUCH_I2C, FT6336_ADDR, &reg, 1, false, 5000);
    if (ret < 0) return 0xFF;
    ret = i2c_read_timeout_us(TOUCH_I2C, FT6336_ADDR, &val, 1, false, 5000);
    if (ret < 0) return 0xFF;
    return val;
}

uint8_t Touch_GetChipID(void)
{
    // 依次尝试 0xA3 (ChipID), 0xA8 (VendorID), 0xA6 (FirmwareID)
    uint8_t id = Touch_ReadReg(0xA3);
    if (id != 0xFF && id != 0x00) return id;

    id = Touch_ReadReg(0xA8);
    if (id != 0xFF && id != 0x00) return id;

    id = Touch_ReadReg(0xA6);
    if (id != 0xFF && id != 0x00) return id;

    uint8_t dev_mode = Touch_ReadReg(0x00);
    if (dev_mode != 0xFF) {
        return 0x63; // 标明 FT6336 在线
    }

    return 0xFF; // 彻底离线
}