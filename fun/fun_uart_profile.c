#include "fun_uart_profile.h"

#include <stddef.h>
#include <string.h>

static uint32_t profile_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

void fun_uart_profile_set_defaults(fun_uart_profile_t *profile)
{
    if (profile == NULL) return;

    memset(profile, 0, sizeof(*profile));
    profile->magic = FUN_UART_PROFILE_MAGIC;
    profile->version = FUN_UART_PROFILE_VERSION;
    profile->size = (uint16_t)sizeof(*profile);
    profile->baudrate = 115200u;
    profile->data_bits = 8u;
    profile->parity = 0u;
    profile->stop_bits = 1u;
    profile->tx_gpio = 20u;
    profile->rx_gpio = 21u;
    fun_uart_profile_finalize(profile);
}

void fun_uart_profile_finalize(fun_uart_profile_t *profile)
{
    if (profile == NULL) return;

    profile->magic = FUN_UART_PROFILE_MAGIC;
    profile->version = FUN_UART_PROFILE_VERSION;
    profile->size = (uint16_t)sizeof(*profile);
    profile->crc32 = profile_crc32(
        (const uint8_t *)profile,
        offsetof(fun_uart_profile_t, crc32)
    );
}

bool fun_uart_profile_is_valid(const fun_uart_profile_t *profile)
{
    if (profile == NULL ||
        profile->magic != FUN_UART_PROFILE_MAGIC ||
        profile->version != FUN_UART_PROFILE_VERSION ||
        profile->size != sizeof(*profile)) {
        return false;
    }

    if (profile->baudrate == 0u ||
        (profile->data_bits != 7u && profile->data_bits != 8u) ||
        profile->parity > 2u ||
        (profile->stop_bits != 1u && profile->stop_bits != 2u) ||
        profile->tx_gpio >= 32u ||
        profile->rx_gpio >= 32u ||
        profile->tx_gpio == profile->rx_gpio) {
        return false;
    }

    return profile->crc32 == profile_crc32(
        (const uint8_t *)profile,
        offsetof(fun_uart_profile_t, crc32)
    );
}
