/*
 * @Author: 轩
 * @Date: 2026-09-02 20:26:06
 * @LastEditTime: 2026-09-06 12:13:06
 * @FilePath: \pico\config.h
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

#define FW_VERSION_STRING "v1.0.0"
#define HW_VERSION_STRING "Pico Board Rev A"
#define SDK_VERSION_STRING "Pico SDK 2.2.0"
#define BOARD_FLASH_SIZE_BYTES (2u * 1024u * 1024u)

#define FW_BUILD_DATE __DATE__
#define FW_BUILD_TIME __TIME__

    extern bool sys_led_blink; // 系统灯
    extern uint8_t led_brigthness;
    typedef enum
    {
        PIN_FUNC_NONE = 0,
        PIN_FUNC_GPIO = 1u << 0,
        PIN_FUNC_UART_TX = 1u << 1,
        PIN_FUNC_UART_RX = 1u << 2,
        PIN_FUNC_SPI_SCK = 1u << 3,
        PIN_FUNC_SPI_MOSI = 1u << 4,
        PIN_FUNC_SPI_MISO = 1u << 5,
        PIN_FUNC_SPI_CS = 1u << 6,
        PIN_FUNC_I2C_SDA = 1u << 7,
        PIN_FUNC_I2C_SCL = 1u << 8,
        PIN_FUNC_PWM=1u << 9,
        PIN_FUNC_ADC=1u << 10,

        PIN_FUNC_PIO = 1u << 11

    } pin_fun_t;
typedef struct {
    uint8_t gpio;
    uint32_t cap;
    const char *label;
}pin_init_t;

static const pin_init_t fun_pins[] = {
 
    {2,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP2"},
    {3,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MOSI | PIN_FUNC_I2C_SCL, "GP3"},
    {4,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP4"},
    {5,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_CS   | PIN_FUNC_UART_RX | PIN_FUNC_I2C_SCL, "GP5"},
    {6,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP6"},
    {20, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP20"},
    {21, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_CS   | PIN_FUNC_UART_RX | PIN_FUNC_I2C_SCL, "GP21"},
    {22, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP22"},
    {26, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC      | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP26"},
    {27, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC      | PIN_FUNC_SPI_MOSI | PIN_FUNC_I2C_SCL, "GP27"},
    {28, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC      | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP28"},
};
typedef enum {
    PIN_OWNER_NONE = 0,
    PIN_OWNER_SYSTEM,
    PIN_OWNER_UART,
    PIN_OWNER_SPI,
    PIN_OWNER_I2C,
    PIN_OWNER_PWM,
    PIN_OWNER_ADC,
    PIN_OWNER_SWD
} pin_owner_t;

typedef enum {
    PIN_SOURCE_NONE = 0,
    PIN_SOURCE_HARDWARE,
    PIN_SOURCE_PIO
} pin_source_t;
typedef struct {
    bool allocated;         
    pin_owner_t owner;      
    pin_source_t source;     
    uint8_t instance;       
    const char *role;        
    const char *owner_name; 
} pin_runtime_t;
#ifdef __cplusplus
}
#endif

#endif // CONFIG_H