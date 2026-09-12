/*
 * @Author: 轩
 * @Date: 2026-09-12 17:17:26
 * @LastEditTime: 2026-09-12 18:26:43
 * @FilePath: \pico\fun\pin_manager.c
 */
#include "pin_manager.h"
#include <string.h>

static const pin_init_t s_fun_pins[PIN_MANAGER_PIN_COUNT] = {
    {2,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP2"},
    {3,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MOSI | PIN_FUNC_I2C_SCL, "GP3"},
    {4,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP4"},
    {5,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_CS   | PIN_FUNC_UART_RX | PIN_FUNC_I2C_SCL, "GP5"},
    {6,  PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP6"},
    {20, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP20"},
    {21, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_CS   | PIN_FUNC_UART_RX | PIN_FUNC_I2C_SCL, "GP21"},
    {22, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP22"},
    {26, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC | PIN_FUNC_SPI_SCK  | PIN_FUNC_I2C_SDA, "GP26"},
    {27, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC | PIN_FUNC_SPI_MOSI | PIN_FUNC_I2C_SCL, "GP27"},
    {28, PIN_FUNC_GPIO | PIN_FUNC_PIO | PIN_FUNC_PWM | PIN_FUNC_ADC | PIN_FUNC_SPI_MISO | PIN_FUNC_UART_TX | PIN_FUNC_I2C_SDA, "GP28"},
};

/* 整个系统共用这一份运行时引脚占用表。 */
static pin_manager_t s_default_manager;
static bool s_default_manager_initialized = false;


void pin_manager_init(pin_manager_t *manager)
{
    if(manager==NULL) return;

    memset(manager,0,sizeof(*manager));
    manager->initialized = true;
}

size_t pin_manager_get_count(void)
{
    return PIN_MANAGER_PIN_COUNT;
}

const pin_init_t *pin_manager_get_pin_at(size_t index)
{
    if (index >= PIN_MANAGER_PIN_COUNT) {
        return NULL;
    }

    return &s_fun_pins[index];
}

static int pin_manager_find_index(uint8_t gpio)
{
    for(size_t i=0;i<PIN_MANAGER_PIN_COUNT;++i)
    {
        if(s_fun_pins[i].gpio==gpio) return (int)i;
    }
    return -1;
}

const pin_init_t *pin_manager_get_pin(uint8_t gpio)
{
   int index =pin_manager_find_index(gpio);
   if(index<0) return NULL;
   return &s_fun_pins[index];
}
bool pin_manager_supports(uint8_t gpio,pin_fun_t fun)
{
    const pin_init_t *pin =pin_manager_get_pin(gpio);
    if(pin==NULL||fun==PIN_FUNC_NONE) return false;
    return (pin->capabilities&(uint32_t)fun)==(uint32_t)fun;
}

const pin_runtime_t *pin_manager_get_runtime_at(
    const pin_manager_t *manager,
    size_t index
)
{
    if (manager == NULL || !manager->initialized ||
        index >= PIN_MANAGER_PIN_COUNT) {
        return NULL;
    }

    return &manager->runtime[index];
}

const pin_runtime_t *pin_manager_get_runtime(const pin_manager_t *manager,uint8_t gpio)
{
    if(manager==NULL||!manager->initialized) return NULL;
    int index = pin_manager_find_index(gpio);
     if(index<0) return NULL;
     return &manager->runtime[index];
}

bool pin_manager_is_free(const pin_manager_t *manager, uint8_t gpio)
{
    const pin_runtime_t *runtime =
        pin_manager_get_runtime(manager, gpio);

    if (runtime == NULL) {
        return false;
    }

    return !runtime->allocated;
}

bool pin_manager_claim(pin_manager_t *manager,uint8_t gpio,const pin_claim_t *claim)
{
    if (manager == NULL || !manager->initialized || claim == NULL) {
        return false;
    }

    if (claim->owner == PIN_OWNER_NONE ||
        claim->source == PIN_SOURCE_NONE ||
        claim->function == PIN_FUNC_NONE ||
        claim->role == NULL ||
        claim->owner_name == NULL) {
        return false;
    }

    int index = pin_manager_find_index(gpio);
    if (index < 0 || manager->runtime[index].allocated) {
        return false;
    }

    /*
     * 硬件外设必须检查该GPIO是否具备对应的硬件复用功能。
     * PIO功能不受硬件复用表限制，只要求该GPIO能够交给PIO控制。
     */
    if (claim->source == PIN_SOURCE_HARDWARE) {
        if (!pin_manager_supports(gpio, claim->function)) {
            return false;
        }
    }
    else if (claim->source == PIN_SOURCE_PIO) {
        if (!pin_manager_supports(gpio, PIN_FUNC_PIO)) {
            return false;
        }
    }
    else {
        return false;
    }

    pin_runtime_t *runtime = &manager->runtime[index];
    runtime->allocated = true;
    runtime->owner = claim->owner;
    runtime->source = claim->source;
    runtime->function = claim->function;
    runtime->instance = claim->instance;
    runtime->role = claim->role;
    runtime->owner_name = claim->owner_name;

    return true;
}

bool pin_manager_release(pin_manager_t *manager,uint8_t gpio,pin_owner_t owner,uint8_t instance)
{
    if (manager == NULL || !manager->initialized ||
        owner == PIN_OWNER_NONE) {
        return false;
    }

    int index = pin_manager_find_index(gpio);
    if (index < 0) {
        return false;
    }

    pin_runtime_t *runtime = &manager->runtime[index];

    if (!runtime->allocated ||
        runtime->owner != owner ||
        runtime->instance != instance) {
        return false;
    }

    /* 清零后该引脚重新回到未分配状态。 */
    memset(runtime, 0, sizeof(*runtime));
    return true;
}

bool pin_manager_claim_pair(
    pin_manager_t *manager,
    uint8_t first_gpio,
    const pin_claim_t *first_claim,
    uint8_t second_gpio,
    const pin_claim_t *second_claim
)
{
    if (manager == NULL || first_claim == NULL || second_claim == NULL ||
        first_gpio == second_gpio) {
        return false;
    }

    /* 一组引脚必须属于同一个功能实例，例如同一个UART2。 */
    if (first_claim->owner != second_claim->owner ||
        first_claim->instance != second_claim->instance) {
        return false;
    }

    if (!pin_manager_claim(manager, first_gpio, first_claim)) {
        return false;
    }

    if (!pin_manager_claim(manager, second_gpio, second_claim)) {
        /* 第二根申请失败时释放第一根，保证不会残留半组资源。 */
        (void)pin_manager_release(
            manager,
            first_gpio,
            first_claim->owner,
            first_claim->instance
        );
        return false;
    }

    return true;
}

bool pin_manager_release_pair(
    pin_manager_t *manager,
    uint8_t first_gpio,
    uint8_t second_gpio,
    pin_owner_t owner,
    uint8_t instance
)
{
    if (manager == NULL || !manager->initialized ||
        owner == PIN_OWNER_NONE || first_gpio == second_gpio) {
        return false;
    }

    const pin_runtime_t *first =
        pin_manager_get_runtime(manager, first_gpio);
    const pin_runtime_t *second =
        pin_manager_get_runtime(manager, second_gpio);

    /* 先完整验证，防止参数错误时只释放一根引脚。 */
    if (first == NULL || second == NULL ||
        !first->allocated || !second->allocated ||
        first->owner != owner || second->owner != owner ||
        first->instance != instance || second->instance != instance) {
        return false;
    }

    bool first_released = pin_manager_release(
        manager,
        first_gpio,
        owner,
        instance
    );
    bool second_released = pin_manager_release(
        manager,
        second_gpio,
        owner,
        instance
    );

    return first_released && second_released;
}

void pin_manager_system_init(void)
{
    if (s_default_manager_initialized) {
        return;
    }

    pin_manager_init(&s_default_manager);
    s_default_manager_initialized = true;
}

pin_manager_t *pin_manager_get_default(void)
{
    /* 即使主程序忘记显式初始化，首次获取时也能安全初始化。 */
    pin_manager_system_init();
    return &s_default_manager;
}
