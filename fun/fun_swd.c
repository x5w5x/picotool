#include "fun_swd.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
static volatile uint32_t s_swd_delay_count = 25;
static volatile uint8_t s_last_write_ack = 0;
static volatile uint8_t s_last_read_ack = 0;

#define SWCLK_HIGH() gpio_put(FUN_SWD_SWCLK_PIN, 1)
#define SWCLK_LOW()  gpio_put(FUN_SWD_SWCLK_PIN, 0)
#define SWDIO_HIGH() gpio_put(FUN_SWD_SWDIO_PIN, 1)
#define SWDIO_LOW()  gpio_put(FUN_SWD_SWDIO_PIN, 0)
#define SWDIO_READ() gpio_get(FUN_SWD_SWDIO_PIN)

static inline void swd_delay(void)
{
    for (volatile uint32_t i = 0; i < s_swd_delay_count; ++i)
        __asm volatile("nop");
}

static inline void swdio_output(void) { gpio_set_dir(FUN_SWD_SWDIO_PIN, GPIO_OUT); }
static inline void swdio_input(void)
{
    gpio_set_dir(FUN_SWD_SWDIO_PIN, GPIO_IN);
    gpio_pull_up(FUN_SWD_SWDIO_PIN);
}

static void swd_write_bit(uint8_t bit)
{
    if (bit) SWDIO_HIGH(); else SWDIO_LOW();
    swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
}

static uint8_t swd_read_bit(void)
{
    SWCLK_LOW(); swd_delay();
    uint8_t bit = SWDIO_READ() ? 1u : 0u;
    SWCLK_HIGH(); swd_delay();
    return bit;
}

static void swd_write_bits(uint32_t value, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) swd_write_bit((value >> i) & 1u);
}

static uint8_t swd_parity(uint32_t value)
{
    uint8_t parity = 0;
    for (uint8_t i = 0; i < 32; ++i) parity ^= (value >> i) & 1u;
    return parity;
}

static uint8_t swd_make_request(uint8_t ap, uint8_t read, uint8_t address)
{
    uint8_t request = 0x81;
    if (ap) request |= 1u << 1;
    if (read) request |= 1u << 2;
    request |= (address & 0x0cu) << 1;
    uint8_t parity = 0;
    for (uint8_t i = 1; i <= 4; ++i) parity ^= (request >> i) & 1u;
    if (parity) request |= 1u << 5;
    return request;
}

void fun_swd_line_reset(void)
{
    swdio_output();
    for (uint8_t i = 0; i < 64; ++i) swd_write_bit(1);
    swd_write_bits(0xE79Eu, 16);
    for (uint8_t i = 0; i < 64; ++i) swd_write_bit(1);
    swd_write_bits(0, 16);
}

uint8_t fun_swd_transfer(uint8_t request, uint32_t *data, bool read)
{
    if (!data) return 0;
    uint8_t ack = 0, parity = 0;
    for (uint8_t i = 0; i < 8; ++i) swd_write_bit((request >> i) & 1u);
    swdio_input();
    swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
    ack |= swd_read_bit() << 0;
    ack |= swd_read_bit() << 1;
    ack |= swd_read_bit() << 2;
    if (ack != 1u) { swdio_output(); return ack; }

    if (read) {
        uint32_t value = 0;
        for (uint8_t i = 0; i < 32; ++i) {
            uint8_t bit = swd_read_bit();
            if (bit) value |= 1u << i;
            parity ^= bit;
        }
        uint8_t received_parity = swd_read_bit();
        *data = value;
        swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
        swdio_output();
        if (received_parity != parity) return 4u;
    } else {
        swd_delay(); SWCLK_LOW(); swd_delay(); SWCLK_HIGH();
        swdio_output();
        parity = swd_parity(*data);
        for (uint8_t i = 0; i < 32; ++i) swd_write_bit((*data >> i) & 1u);
        swd_write_bit(parity);
    }
    SWDIO_LOW();
    for (uint8_t i = 0; i < 12; ++i) {
        SWCLK_LOW(); swd_delay(); SWCLK_HIGH(); swd_delay();
    }
    return ack;
}

static void swd_abort(void)
{
    uint32_t value = 0x1e;
    (void)fun_swd_transfer(swd_make_request(0, 0, 0x00), &value, false);
}

static uint8_t swd_transfer_retry(uint8_t request, uint32_t *data, bool read)
{
    uint8_t ack = 0;
    for (uint8_t retry = 0; retry < 50; ++retry) {
        ack = fun_swd_transfer(request, data, read);
        if (ack == 1u) return ack;
        if (ack == 2u) continue;
        if (ack == 4u) { swd_abort(); continue; }
        break;
    }
    return ack;
}

static bool swd_power_up(void)
{
    uint32_t status = 0;
    uint32_t request = (1u << 28) | (1u << 30);
    swd_abort();
    if (swd_transfer_retry(swd_make_request(0, 0, 0x04), &request, false) != 1u)
        return false;
    for (uint16_t i = 0; i < 500; ++i) {
        if (swd_transfer_retry(swd_make_request(0, 1, 0x04), &status, true) == 1u &&
            (status & (1u << 29)) && (status & (1u << 31))) return true;
    }
    return false;
}

void fun_swd_set_delay(uint8_t delay) { s_swd_delay_count = delay; }
uint8_t fun_swd_last_write_ack(void) { return s_last_write_ack; }
uint8_t fun_swd_last_read_ack(void) { return s_last_read_ack; }

bool fun_swd_init(void)
{
    gpio_init(FUN_SWD_SWCLK_PIN); gpio_init(FUN_SWD_SWDIO_PIN);
    gpio_set_dir(FUN_SWD_SWCLK_PIN, GPIO_OUT);
    gpio_set_dir(FUN_SWD_SWDIO_PIN, GPIO_OUT);
    SWCLK_HIGH(); SWDIO_HIGH(); gpio_pull_up(FUN_SWD_SWDIO_PIN);
    sleep_ms(20);
    s_swd_delay_count = 25;
    for (uint8_t i = 0; i < 5; ++i) { fun_swd_line_reset(); sleep_ms(2); }
    uint32_t id = 0;
    if (swd_transfer_retry(swd_make_request(0, 1, 0x00), &id, true) != 1u || id == 0)
        return false;
    if (!swd_power_up()) return false;
    s_swd_delay_count = 2;
    return true;
}

void fun_swd_write(uint32_t address, uint32_t value)
{
    uint32_t temp = 0;
    swd_transfer_retry(swd_make_request(0, 0, 0x08), &temp, false);
    temp = 0x23000002; swd_transfer_retry(swd_make_request(1, 0, 0x00), &temp, false);
    temp = address; swd_transfer_retry(swd_make_request(1, 0, 0x04), &temp, false);
    s_last_write_ack = swd_transfer_retry(swd_make_request(1, 0, 0x0c), &value, false);
}

void fun_swd_write_half(uint32_t address, uint16_t value)
{
    uint32_t temp = 0;
    swd_transfer_retry(swd_make_request(0, 0, 0x08), &temp, false);
    temp = 0x23000001; swd_transfer_retry(swd_make_request(1, 0, 0x00), &temp, false);
    temp = address; swd_transfer_retry(swd_make_request(1, 0, 0x04), &temp, false);
    uint32_t data = (address & 2u) ? ((uint32_t)value << 16) : value;
    s_last_write_ack = swd_transfer_retry(swd_make_request(1, 0, 0x0c), &data, false);
    temp = 0x23000002; swd_transfer_retry(swd_make_request(1, 0, 0x00), &temp, false);
}

uint32_t fun_swd_read(uint32_t address)
{
    uint32_t temp = 0, value = 0;
    swd_transfer_retry(swd_make_request(0, 0, 0x08), &temp, false);
    temp = 0x23000002; swd_transfer_retry(swd_make_request(1, 0, 0x00), &temp, false);
    temp = address; swd_transfer_retry(swd_make_request(1, 0, 0x04), &temp, false);
    uint8_t ap_ack = swd_transfer_retry(swd_make_request(1, 1, 0x0c), &value, true);
    s_last_read_ack = ap_ack == 1u
        ? swd_transfer_retry(swd_make_request(0, 1, 0x0c), &value, true)
        : ap_ack;
    return value;
}
