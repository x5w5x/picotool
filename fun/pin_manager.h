/*
 * @Author: 轩
 * @Date: 2026-09-12 17:14:37
 * @LastEditTime: 2026-09-12 18:26:23
 * @FilePath: \pico\fun\pin_manager.h
 */
#ifndef PIN_MANAGER_H
#define PIN_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    PIN_FUNC_NONE     = 0,
    PIN_FUNC_GPIO     = 1u << 0,
    PIN_FUNC_UART_TX  = 1u << 1,
    PIN_FUNC_UART_RX  = 1u << 2,
    PIN_FUNC_SPI_SCK  = 1u << 3,
    PIN_FUNC_SPI_MOSI = 1u << 4,
    PIN_FUNC_SPI_MISO = 1u << 5,
    PIN_FUNC_SPI_CS   = 1u << 6,
    PIN_FUNC_I2C_SDA  = 1u << 7,
    PIN_FUNC_I2C_SCL  = 1u << 8,
    PIN_FUNC_PWM      = 1u << 9,
    PIN_FUNC_ADC      = 1u << 10,
    PIN_FUNC_PIO      = 1u << 11
} pin_fun_t;

typedef struct
{
    uint8_t gpio;
    uint32_t capabilities;
    const char *label;
} pin_init_t;

typedef enum
{
    PIN_OWNER_NONE = 0,
    PIN_OWNER_SYSTEM,
    PIN_OWNER_UART,
    PIN_OWNER_SPI,
    PIN_OWNER_I2C,
    PIN_OWNER_PWM,
    PIN_OWNER_ADC,
    PIN_OWNER_SWD
} pin_owner_t;

typedef enum
{
    PIN_SOURCE_NONE = 0,
    PIN_SOURCE_HARDWARE,
    PIN_SOURCE_PIO
} pin_source_t;

typedef struct
{
    bool allocated;
    pin_owner_t owner;
    pin_source_t source;
    pin_fun_t function;
    uint8_t instance;
    const char *role;
    const char *owner_name;
} pin_runtime_t;

/* 申请一个功能引脚时由调用者提供的信息。 */
typedef struct
{
    pin_owner_t owner;
    pin_source_t source;
    pin_fun_t function;
    uint8_t instance;
    const char *role;
    const char *owner_name;
} pin_claim_t;

#define PIN_MANAGER_PIN_COUNT 12u

typedef struct {
    pin_runtime_t runtime[PIN_MANAGER_PIN_COUNT];
    bool initialized;
} pin_manager_t;
void pin_manager_init(pin_manager_t *manager);
size_t pin_manager_get_count(void);
const pin_init_t *pin_manager_get_pin_at(size_t index);
const pin_init_t *pin_manager_get_pin(uint8_t gpio);
bool pin_manager_supports(uint8_t gpio,pin_fun_t fun);
const pin_runtime_t *pin_manager_get_runtime_at(
    const pin_manager_t *manager,
    size_t index
);
const pin_runtime_t *pin_manager_get_runtime(
    const pin_manager_t *manager,
    uint8_t gpio
);
bool pin_manager_is_free(
    const pin_manager_t *manager,
    uint8_t gpio
);
bool pin_manager_claim(
    pin_manager_t *manager,
    uint8_t gpio,
    const pin_claim_t *claim
);
bool pin_manager_release(
    pin_manager_t *manager,
    uint8_t gpio,
    pin_owner_t owner,
    uint8_t instance
);
bool pin_manager_claim_pair(
    pin_manager_t *manager,
    uint8_t first_gpio,
    const pin_claim_t *first_claim,
    uint8_t second_gpio,
    const pin_claim_t *second_claim
);
bool pin_manager_release_pair(
    pin_manager_t *manager,
    uint8_t first_gpio,
    uint8_t second_gpio,
    pin_owner_t owner,
    uint8_t instance
);
void pin_manager_system_init(void);
pin_manager_t *pin_manager_get_default(void);

#ifdef __cplusplus
}
#endif

#endif // PIN_MANAGER_H
