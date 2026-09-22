#include "fun_pio_swd.h"
#include "swd.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include <string.h>

#define PIO_SWD_PROGRAM_NAME "swd_read"
#define PIO_SWD_WRITE_PROGRAM_NAME "swd_write"

static uint8_t make_request(bool ap, bool read, uint8_t address)
{
    uint8_t request = 0x81u;
    if (ap) request |= 1u << 1;
    if (read) request |= 1u << 2;
    request |= (address & 0x0cu) << 1;
    uint8_t parity = 0;
    for (uint8_t i = 1; i <= 4; ++i) parity ^= (request >> i) & 1u;
    if (parity) request |= 1u << 5;
    return request;
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16; value ^= value >> 8;
    value ^= value >> 4; value ^= value >> 2; value ^= value >> 1;
    return value & 1u;
}

static bool wait_rx_word(PIO pio, uint8_t sm, uint32_t *word,
                         uint32_t timeout_us);
static bool wait_sm_at(PIO pio, uint8_t sm, uint offset,
                       uint32_t timeout_us);
static bool ensure_write_sm(fun_pio_swd_t *swd);
static void restart_read_sm(fun_pio_swd_t *swd);
static bool pio_read_register(fun_pio_swd_t *swd, bool ap,
                              uint8_t address, uint32_t *value,
                              uint8_t *ack);
static bool pio_write_register(fun_pio_swd_t *swd, bool ap,
                               uint8_t address, uint32_t value,
                               uint8_t *ack);
static bool pio_write_session_begin(fun_pio_swd_t *swd);
static bool pio_write_transaction_active(fun_pio_swd_t *swd, bool ap,
                                         uint8_t address, uint32_t value,
                                         uint8_t *ack);
static void pio_write_session_end(fun_pio_swd_t *swd);

static void restore_read_sm(fun_pio_swd_t *swd)
{
    pio_gpio_init(swd->pio, swd->swclk_gpio);
    pio_gpio_init(swd->pio, swd->swdio_gpio);
    gpio_pull_up(swd->swdio_gpio);
    restart_read_sm(swd);
}

void fun_pio_swd_init(fun_pio_swd_t *swd)
{
    if (swd) memset(swd, 0, sizeof(*swd));
}

static void line_reset_gpio(uint8_t clk, uint8_t io)
{
    /* 先预装SIO输出锁存器和方向，再切功能，避免切换瞬间产生低脉冲。 */
    gpio_put(clk, 1);
    gpio_put(io, 1);
    gpio_set_dir(clk, GPIO_OUT);
    gpio_set_dir(io, GPIO_OUT);
    gpio_set_function(clk, GPIO_FUNC_SIO);
    gpio_set_function(io, GPIO_FUNC_SIO);
    for (uint16_t i = 0; i < 64; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    /* JTAG-to-SWD 0xE79E, LSB first. */
    for (uint8_t i = 0; i < 16; ++i) {
        gpio_put(io, (0xE79Eu >> i) & 1u);
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    gpio_put(io, 1);
    for (uint16_t i = 0; i < 64; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    /* 与已验证的软件版本一致：切换序列后再发送16个低电平周期。 */
    gpio_put(io, 0);
    for (uint8_t i = 0; i < 16; ++i) {
        sleep_us(1); gpio_put(clk, 0); sleep_us(1); gpio_put(clk, 1);
    }
    gpio_put(io, 1);
}

static void line_reset_repeated(uint8_t clk, uint8_t io)
{
    /* 与已经验证的软件SWD初始化一致，多次复位可恢复中断在半截事务的目标。 */
    for (uint8_t attempt = 0; attempt < 5u; ++attempt) {
        line_reset_gpio(clk, io);
        sleep_ms(2);
    }
}

static void restart_read_sm(fun_pio_swd_t *swd)
{
    float div = (float)clock_get_hz(clk_sys) /
                ((float)swd->clock_hz * 2.0f);
    if (div < 1.0f) div = 1.0f;

    pio_sm_config cfg =
        swd_read_program_get_default_config(swd->offset);
    sm_config_set_out_pins(&cfg, swd->swdio_gpio, 1);
    sm_config_set_set_pins(&cfg, swd->swdio_gpio, 1);
    sm_config_set_in_pins(&cfg, swd->swdio_gpio);
    sm_config_set_sideset_pins(&cfg, swd->swclk_gpio);
    sm_config_set_out_shift(&cfg, true, false, 32);
    sm_config_set_in_shift(&cfg, true, false, 32);
    sm_config_set_clkdiv(&cfg, div);

    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_clear_fifos(swd->pio, swd->sm);
    /* pio_sm_init会重置PC、移位计数器、FIFO状态和时钟分频状态。 */
    pio_sm_init(swd->pio, swd->sm, swd->offset, &cfg);
    pio_sm_set_pindirs_with_mask(swd->pio, swd->sm,
                                 (1u << swd->swclk_gpio) |
                                 (1u << swd->swdio_gpio),
                                 (1u << swd->swclk_gpio) |
                                 (1u << swd->swdio_gpio));
    pio_sm_set_pins_with_mask(swd->pio, swd->sm,
                              (1u << swd->swclk_gpio),
                              (1u << swd->swclk_gpio) |
                              (1u << swd->swdio_gpio));
    pio_sm_set_enabled(swd->pio, swd->sm, true);
}

bool fun_pio_swd_recover(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return false;
    pio_sm_set_enabled(swd->pio, swd->sm, false);
    swd->mem_select_valid = false;
    swd->mem_csw_valid = false;
    swd->mem_tar_valid = false;

    /* 使用实例自己的引脚恢复线路，避免固定GP2/GP3的软件SWD干扰。 */
    line_reset_repeated(swd->swclk_gpio, swd->swdio_gpio);

    pio_gpio_init(swd->pio, swd->swclk_gpio);
    pio_gpio_init(swd->pio, swd->swdio_gpio);
    gpio_pull_up(swd->swdio_gpio);
    restart_read_sm(swd);
    return true;
}

bool fun_pio_swd_open_auto(fun_pio_swd_t *swd, pio_manager_t *manager,
                           uint8_t clk, uint8_t io, uint32_t hz)
{
    return fun_pio_swd_open_session(swd, manager, clk, io, hz, true);
}

bool fun_pio_swd_open_session(fun_pio_swd_t *swd, pio_manager_t *manager,
                              uint8_t clk, uint8_t io, uint32_t hz,
                              bool line_reset)
{
    if (!swd || !manager || swd->opened || clk >= 30 || io >= 30 ||
        clk == io || hz == 0) return false;
    for (uint8_t block = 0; block < PIO_BLOCK_COUNT; ++block) {
        int sm_id = pio_manager_claim_sm_from(manager, block, PIO_SWD_PROGRAM_NAME);
        if (sm_id < 0) continue;
        uint8_t sm = (uint8_t)(sm_id % PIO_SM_COUNT);
        int offset = pio_manager_load_program(manager, block, PIO_SWD_PROGRAM_NAME,
                                              &swd_read_program);
        if (offset < 0) { pio_manager_release_sm(manager, block, sm); continue; }
        PIO pio = pio_manager_get_hardware(manager, block);
        /* 每个请求/ACK/数据位由低、高两个PIO指令周期组成。 */
        float div = (float)clock_get_hz(clk_sys) / ((float)hz * 2.0f);
        if (div < 1.0f) div = 1.0f;
        pio_sm_config cfg = swd_read_program_get_default_config((uint)offset);
        sm_config_set_out_pins(&cfg, io, 1);
        sm_config_set_set_pins(&cfg, io, 1);
        sm_config_set_in_pins(&cfg, io);
        sm_config_set_sideset_pins(&cfg, clk);
        sm_config_set_out_shift(&cfg, true, false, 32);
        sm_config_set_in_shift(&cfg, true, false, 32);
        sm_config_set_clkdiv(&cfg, div);
        if (line_reset) line_reset_repeated(clk, io);
        pio_gpio_init(pio, clk); pio_gpio_init(pio, io);
        gpio_pull_up(io);
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_clear_fifos(pio, sm);
        pio_sm_init(pio, sm, (uint)offset, &cfg);
        pio_sm_set_pindirs_with_mask(pio, sm, (1u << clk) | (1u << io),
                                     (1u << clk) | (1u << io));
        /* 已验证的事务空闲状态：SWCLK高、SWDIO低。 */
        pio_sm_set_pins_with_mask(pio, sm, (1u << clk),
                                  (1u << clk) | (1u << io));
        pio_sm_set_enabled(pio, sm, true);
        swd->opened = true; swd->manager = manager; swd->pio = pio;
        swd->pio_index = block; swd->sm = sm; swd->offset = (uint)offset;
        swd->swclk_gpio = clk; swd->swdio_gpio = io; swd->clock_hz = hz;
        return true;
    }
    return false;
}

void fun_pio_swd_close(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return;
    if (swd->write_opened) {
        pio_sm_set_enabled(swd->write_pio, swd->write_sm, false);
        pio_sm_clear_fifos(swd->write_pio, swd->write_sm);
        pio_manager_release_program(swd->manager, swd->write_pio_index,
                                    PIO_SWD_WRITE_PROGRAM_NAME);
        pio_manager_release_sm(swd->manager, swd->write_pio_index,
                               swd->write_sm);
    }
    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_manager_release_program(swd->manager, swd->pio_index, PIO_SWD_PROGRAM_NAME);
    pio_manager_release_sm(swd->manager, swd->pio_index, swd->sm);
    gpio_set_function(swd->swclk_gpio, GPIO_FUNC_SIO);
    gpio_set_function(swd->swdio_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(swd->swclk_gpio, GPIO_IN);
    gpio_set_dir(swd->swdio_gpio, GPIO_IN);
    fun_pio_swd_init(swd);
}

static bool ensure_write_sm(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return false;
    if (swd->write_opened) return true;

    for (uint8_t block = 0; block < PIO_BLOCK_COUNT; ++block) {
        if (block == swd->pio_index) continue;
        int sm_id = pio_manager_claim_sm_from(
            swd->manager, block, PIO_SWD_WRITE_PROGRAM_NAME);
        if (sm_id < 0) continue;
        uint8_t sm = (uint8_t)(sm_id % PIO_SM_COUNT);
        int offset = pio_manager_load_program(
            swd->manager, block, PIO_SWD_WRITE_PROGRAM_NAME,
            &swd_write_program);
        if (offset < 0) {
            pio_manager_release_sm(swd->manager, block, sm);
            continue;
        }
        swd->write_pio = pio_manager_get_hardware(swd->manager, block);
        swd->write_pio_index = block;
        swd->write_sm = sm;
        swd->write_offset = (uint)offset;
        swd->write_opened = true;
        return true;
    }
    return false;
}

bool fun_pio_swd_set_clock(fun_pio_swd_t *swd, uint32_t clock_hz)
{
    if (!swd || !swd->opened || clock_hz == 0u) return false;
    float div = (float)clock_get_hz(clk_sys) / ((float)clock_hz * 2.0f);
    if (div < 1.0f) div = 1.0f;
    pio_sm_set_clkdiv(swd->pio, swd->sm, div);
    swd->clock_hz = clock_hz;
    return true;
}

bool fun_pio_swd_read_dp_idcode(fun_pio_swd_t *swd, uint32_t *idcode,
                                uint8_t *ack)
{
    return fun_pio_swd_read_dp_register(swd, 0x00u, idcode, ack);
}

bool fun_pio_swd_read_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                  uint32_t *value, uint8_t *ack)
{
    return pio_read_register(swd, false, address, value, ack);
}

bool fun_pio_swd_read_register(fun_pio_swd_t *swd, bool ap, uint8_t address,
                               uint32_t *value, uint8_t *ack)
{
    return pio_read_register(swd, ap, address, value, ack);
}

static bool pio_read_register(fun_pio_swd_t *swd, bool ap,
                              uint8_t address, uint32_t *value,
                              uint8_t *ack)
{
    if (!swd || !swd->opened || !value || (address & 3u)) return false;
    *value = 0u;
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_sm_put_blocking(swd->pio, swd->sm,
                        make_request(ap, true, address));
    uint32_t word = 0u;
    if (!wait_rx_word(swd->pio, swd->sm, &word, 5000u)) {
        (void)fun_pio_swd_recover(swd);
        if (ack) *ack = 7u;
        return false;
    }
    uint8_t response = (uint8_t)(word >> 29);
    /* 告诉PIO是否存在数据阶段；WAIT/FAULT时必须直接收尾。 */
    pio_sm_put_blocking(swd->pio, swd->sm, response == 1u ? 1u : 0u);
    if (response != 1u) {
        if (!wait_sm_at(swd->pio, swd->sm, swd->offset, 5000u)) {
            (void)fun_pio_swd_recover(swd);
            response = 7u;
        }
        if (ack) *ack = response;
        return false;
    }
    uint32_t raw_value = 0u, parity_word = 0u;
    if (!wait_rx_word(swd->pio, swd->sm, &raw_value, 5000u) ||
        !wait_rx_word(swd->pio, swd->sm, &parity_word, 5000u)) {
        (void)fun_pio_swd_recover(swd);
        if (ack) *ack = 7u;
        return false;
    }
    /* 等待12个空闲周期真正完成并回到pull，再允许上层结束本次探测。 */
    if (!wait_sm_at(swd->pio, swd->sm, swd->offset, 5000u)) {
        (void)fun_pio_swd_recover(swd);
        if (ack) *ack = 7u;
        return false;
    }
    if (response == 1u &&
        ((uint8_t)(parity_word >> 31) != parity32(raw_value)))
        response = 4u;
    if (ack) *ack = response;
    if (response != 1u) return false;
    *value = raw_value;
    return true;
}

static bool wait_rx_word(PIO pio, uint8_t sm, uint32_t *word,
                         uint32_t timeout_us)
{
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (pio_sm_is_rx_fifo_empty(pio, sm)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            return false;
        tight_loop_contents();
    }
    *word = pio_sm_get(pio, sm);
    return true;
}

static bool wait_sm_at(PIO pio, uint8_t sm, uint offset,
                       uint32_t timeout_us)
{
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (pio_sm_get_pc(pio, sm) != offset) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            return false;
        tight_loop_contents();
    }
    return true;
}

uint8_t fun_pio_swd_transfer(fun_pio_swd_t *swd, uint8_t request,
                             uint32_t *data)
{
    if (!swd || !swd->opened || !data) return 7u;

    bool read = (request & (1u << 2)) != 0u;
    uint8_t active_sm = swd->sm;
    uint active_offset = swd->offset;

    pio_sm_set_enabled(swd->pio, active_sm, false);
    pio_sm_clear_fifos(swd->pio, active_sm);
    pio_sm_restart(swd->pio, active_sm);
    pio_sm_exec(swd->pio, active_sm, pio_encode_jmp(active_offset));
    pio_sm_put(swd->pio, active_sm, request);
    pio_sm_set_enabled(swd->pio, active_sm, true);

    uint32_t fifo_word = 0u;
    if (!wait_rx_word(swd->pio, active_sm, &fifo_word, 5000u)) {
        (void)fun_pio_swd_recover(swd);
        return 7u;
    }
    uint8_t response = (uint8_t)(fifo_word >> 29);
    if (response != 1u) {
        pio_sm_set_enabled(swd->pio, active_sm, false);
        pio_sm_clear_fifos(swd->pio, active_sm);
        pio_sm_restart(swd->pio, active_sm);
        pio_sm_exec(swd->pio, active_sm, pio_encode_jmp(active_offset));
        pio_sm_set_enabled(swd->pio, active_sm, true);
        return response;
    }
    pio_sm_put_blocking(swd->pio, active_sm, read ? 0u : 1u);

    {
        if (read) {
            uint32_t raw_value = 0u;
            uint32_t parity_word = 0u;
            if (!wait_rx_word(swd->pio, active_sm, &raw_value, 5000u) ||
                !wait_rx_word(swd->pio, active_sm, &parity_word, 5000u)) {
                (void)fun_pio_swd_recover(swd);
                return 7u;
            }
            uint8_t received_parity = (uint8_t)(parity_word >> 31);
            *data = raw_value;
            if (received_parity != parity32(raw_value)) response = 4u;
        } else {
            pio_sm_put_blocking(swd->pio, active_sm, *data);
            pio_sm_put_blocking(swd->pio, active_sm, parity32(*data));
        }
    }

    while (!pio_sm_is_tx_fifo_empty(swd->pio, active_sm)) tight_loop_contents();
    if (!wait_sm_at(swd->pio, active_sm, active_offset, 5000u)) {
        (void)fun_pio_swd_recover(swd);
        return 7u;
    }
    pio_sm_set_enabled(swd->pio, active_sm, false);

    /* 默认恢复读状态机并停在pull等待，便于下一次事务。 */
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_sm_restart(swd->pio, swd->sm);
    pio_sm_exec(swd->pio, swd->sm, pio_encode_jmp(swd->offset));
    pio_sm_set_enabled(swd->pio, swd->sm, true);
    return response;
}

bool fun_pio_swd_write_dp_register(fun_pio_swd_t *swd, uint8_t address,
                                   uint32_t value, uint8_t *ack)
{
    return pio_write_register(swd, false, address, value, ack);
}

bool fun_pio_swd_write_register(fun_pio_swd_t *swd, bool ap, uint8_t address,
                                uint32_t value, uint8_t *ack)
{
    return pio_write_register(swd, ap, address, value, ack);
}

static bool pio_write_register(fun_pio_swd_t *swd, bool ap,
                               uint8_t address, uint32_t value,
                               uint8_t *ack)
{
    if (!swd || !swd->opened || (address & 3u)) return false;
    if (!pio_write_session_begin(swd)) {
        if (ack) *ack = 7u;
        return false;
    }
    bool ok = pio_write_transaction_active(swd, ap, address, value, ack);
    pio_write_session_end(swd);
    return ok;
}

/* Switch the pins to the write SM once. A block transfer can then issue many
 * complete SWD write transactions without reinitialising either PIO block. */
static bool pio_write_session_begin(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened || !ensure_write_sm(swd)) return false;

    float div = (float)clock_get_hz(clk_sys) /
                ((float)swd->clock_hz * 2.0f);
    if (div < 1.0f) div = 1.0f;
    pio_sm_config cfg =
        swd_write_program_get_default_config(swd->write_offset);
    sm_config_set_out_pins(&cfg, swd->swdio_gpio, 1);
    sm_config_set_set_pins(&cfg, swd->swdio_gpio, 1);
    sm_config_set_in_pins(&cfg, swd->swdio_gpio);
    sm_config_set_sideset_pins(&cfg, swd->swclk_gpio);
    sm_config_set_out_shift(&cfg, true, false, 32);
    sm_config_set_in_shift(&cfg, true, false, 32);
    sm_config_set_clkdiv(&cfg, div);

    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_set_enabled(swd->write_pio, swd->write_sm, false);
    pio_sm_clear_fifos(swd->write_pio, swd->write_sm);
    pio_sm_init(swd->write_pio, swd->write_sm,
                swd->write_offset, &cfg);
    pio_sm_set_pins_with_mask(
        swd->write_pio, swd->write_sm,
        (1u << swd->swclk_gpio) | (1u << swd->swdio_gpio),
        (1u << swd->swclk_gpio) | (1u << swd->swdio_gpio));
    pio_sm_set_pindirs_with_mask(
        swd->write_pio, swd->write_sm,
        (1u << swd->swclk_gpio) | (1u << swd->swdio_gpio),
        (1u << swd->swclk_gpio) | (1u << swd->swdio_gpio));
    pio_gpio_init(swd->write_pio, swd->swclk_gpio);
    pio_gpio_init(swd->write_pio, swd->swdio_gpio);
    gpio_pull_up(swd->swdio_gpio);
    pio_sm_set_enabled(swd->write_pio, swd->write_sm, true);
    return true;
}

static bool pio_write_transaction_active(fun_pio_swd_t *swd, bool ap,
                                         uint8_t address, uint32_t value,
                                         uint8_t *ack)
{
    if (!swd || !swd->write_opened || (address & 3u)) return false;
    pio_sm_put_blocking(swd->write_pio, swd->write_sm,
                        make_request(ap, false, address));

    uint32_t ack_word = 0u;
    if (!wait_rx_word(swd->write_pio, swd->write_sm,
                      &ack_word, 5000u)) {
        if (ack) *ack = 7u;
        return false;
    }
    uint8_t response = (uint8_t)(ack_word >> 29);
    if (response == 1u) {
        pio_sm_put_blocking(swd->write_pio, swd->write_sm, value);
        pio_sm_put_blocking(swd->write_pio, swd->write_sm,
                            parity32(value));
        if (!wait_sm_at(swd->write_pio, swd->write_sm,
                        swd->write_offset, 5000u)) {
            response = 7u;
        }
    }
    if (ack) *ack = response;
    swd->last_write_ack = response;
    return response == 1u;
}

static void pio_write_session_end(fun_pio_swd_t *swd)
{
    if (!swd || !swd->write_opened) return;
    pio_sm_set_enabled(swd->write_pio, swd->write_sm, false);
    restore_read_sm(swd);
}

static uint8_t transfer_retry(fun_pio_swd_t *swd, uint8_t request,
                              uint32_t *data)
{
    uint8_t ack = 0u;
    for (uint16_t retry = 0; retry < 200u; ++retry) {
        ack = fun_pio_swd_transfer(swd, request, data);
        if (ack == 1u) return ack;
        if (ack == 2u) continue;
        if (ack == 4u) {
            uint32_t abort_value = 0x1Eu;
            (void)fun_pio_swd_transfer(
                swd, make_request(false, false, 0x00u), &abort_value);
            continue;
        }
        break;
    }
    return ack;
}

bool fun_pio_swd_power_up(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return false;
    swd->power_stage = 0u;
    uint8_t ack = 0u;
    if (!fun_pio_swd_write_dp_register(swd, 0x00u, 0x1Eu, &ack)) {
        swd->power_stage = 1u;
        return false;
    }
    if (!fun_pio_swd_write_dp_register(
            swd, 0x04u, (1u << 28) | (1u << 30), &ack)) {
        swd->power_stage = 2u;
        return false;
    }
    swd->power_stage = 3u;
    for (uint16_t retry = 0; retry < 500u; ++retry) {
        uint32_t value = 0u;
        if (fun_pio_swd_read_dp_register(swd, 0x04u, &value, &ack) &&
            (value & (1u << 29)) && (value & (1u << 31))) {
            swd->power_stage = 4u;
            return true;
        }
        /* ACK OK但电源确认位尚未置位时也必须继续轮询。 */
        if (ack != 1u && ack != 2u) break;
        tight_loop_contents();
    }
    swd->power_stage = 5u;
    return false;
}

bool fun_pio_swd_read_memory32(fun_pio_swd_t *swd, uint32_t address,
                               uint32_t *value)
{
    if (!swd || !swd->opened || !value) return false;
    uint8_t ack = 0u;
    uint32_t posted = 0u;

    /* SELECT is shared by all MEM-AP bank-0 accesses; avoid rewriting it. */
    if (!swd->mem_select_valid) {
        if (!pio_write_register(swd, false, 0x08u, 0u, &ack)) return false;
        swd->mem_select_valid = true;
    }
    /* AHB-AP CSW: 32-bit transfer, auto increment disabled. */
    if (!swd->mem_csw_valid || swd->mem_csw != 0x23000002u) {
        if (!pio_write_register(swd, true, 0x00u, 0x23000002u, &ack))
            return false;
        swd->mem_csw = 0x23000002u;
        swd->mem_csw_valid = true;
    }
    /* TAR selects the target memory address. */
    if (!swd->mem_tar_valid || swd->mem_tar != address) {
        if (!pio_write_register(swd, true, 0x04u, address, &ack)) return false;
        swd->mem_tar = address;
        swd->mem_tar_valid = true;
    }
    /* AP reads are posted: discard this immediate result. */
    if (!pio_read_register(swd, true, 0x0Cu, &posted, &ack)) return false;
    /* The actual memory value is returned through DP RDBUFF. */
    bool ok = pio_read_register(swd, false, 0x0Cu, value, &ack);
    /* No increment is enabled, so TAR still addresses the same word. */
    return ok;
}

static bool pio_write_memory(fun_pio_swd_t *swd, uint32_t address,
                             uint32_t value, uint32_t csw)
{
    if (!swd || !swd->opened) return false;
    uint8_t ack = 0u;
    if (!swd->mem_select_valid) {
        if (!pio_write_register(swd, false, 0x08u, 0u, &ack)) return false;
        swd->mem_select_valid = true;
    }
    if (!swd->mem_csw_valid || swd->mem_csw != csw) {
        if (!pio_write_register(swd, true, 0x00u, csw, &ack)) return false;
        swd->mem_csw = csw;
        swd->mem_csw_valid = true;
    }
    if (!swd->mem_tar_valid || swd->mem_tar != address) {
        if (!pio_write_register(swd, true, 0x04u, address, &ack)) return false;
        swd->mem_tar = address;
        swd->mem_tar_valid = true;
    }
    return pio_write_register(swd, true, 0x0Cu, value, &ack);
}

bool fun_pio_swd_write_memory32(fun_pio_swd_t *swd, uint32_t address,
                                uint32_t value)
{
    if ((address & 3u) != 0u) return false;
    return pio_write_memory(swd, address, value, 0x23000002u);
}

bool fun_pio_swd_write_memory16(fun_pio_swd_t *swd, uint32_t address,
                                uint16_t value)
{
    if ((address & 1u) != 0u) return false;
    uint32_t drw = (address & 2u) ? ((uint32_t)value << 16)
                                  : (uint32_t)value;
    return pio_write_memory(swd, address, drw, 0x23000001u);
}

bool fun_pio_swd_write_memory_block32(fun_pio_swd_t *swd, uint32_t address,
                                      const uint8_t *data, uint32_t length)
{
    if (!swd || !swd->opened || (!data && length != 0u) ||
        (address & 3u) != 0u) return false;

    uint8_t ack = 0u;
    if (!swd->mem_select_valid) {
        if (!pio_write_register(swd, false, 0x08u, 0u, &ack)) return false;
        swd->mem_select_valid = true;
    }

    /* SIZE=word, AddrInc=single. */
    const uint32_t csw = 0x23000012u;
    if (!swd->mem_csw_valid || swd->mem_csw != csw) {
        if (!pio_write_register(swd, true, 0x00u, csw, &ack)) return false;
        swd->mem_csw = csw;
        swd->mem_csw_valid = true;
    }

    /* Program TAR once before taking the long-lived write session. */
    if (!pio_write_register(swd, true, 0x04u, address, &ack)) return false;
    if (!pio_write_session_begin(swd)) return false;

    bool ok = true;
    uint32_t offset = 0u;
    while (offset < length) {
        uint32_t current = address + offset;
        /* AHB-AP auto increment wraps at a 1 KiB boundary. */
        if (offset != 0u && (current & 0x3FFu) == 0u) {
            if (!pio_write_transaction_active(swd, true, 0x04u,
                                              current, &ack)) {
                ok = false;
                break;
            }
        }

        uint32_t word = 0xFFFFFFFFu;
        uint32_t remain = length - offset;
        uint32_t take = remain < 4u ? remain : 4u;
        word = 0u;
        for (uint32_t i = 0u; i < take; ++i)
            word |= (uint32_t)data[offset + i] << (8u * i);
        if (take < 4u) {
            for (uint32_t i = take; i < 4u; ++i)
                word |= 0xFFu << (8u * i);
        }
        if (!pio_write_transaction_active(swd, true, 0x0Cu,
                                          word, &ack)) {
            ok = false;
            break;
        }
        offset += take;
    }
    pio_write_session_end(swd);

    /* Auto-increment makes the cached TAR ambiguous at the 1 KiB wrap. */
    swd->mem_tar_valid = false;
    return ok;
}

#if 0 /* 旧固定读取实现，保留在本次迁移中便于对照。 */
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_sm_put_blocking(swd->pio, swd->sm, make_request(false, true, address));
    uint8_t response = (uint8_t)(pio_sm_get_blocking(swd->pio, swd->sm) >> 29);
    uint32_t raw_value = pio_sm_get_blocking(swd->pio, swd->sm);
    uint8_t received_parity = (uint8_t)(pio_sm_get_blocking(swd->pio, swd->sm) >> 31);
    if (ack) *ack = response;
    *value = raw_value;
    return response == 1u && received_parity == parity32(raw_value);
#endif


bool fun_pio_swd_is_halted(fun_pio_swd_t *swd,bool *halted)
{
    if(!swd||!swd->opened||!halted) return false;
    uint32_t dhcsr = 0;
    if(!fun_pio_swd_read_memory32(swd,SWD_DHCSR,&dhcsr)) return false;
    *halted = (dhcsr & DHCSR_S_HALT) !=0;
    return true;
}
bool fun_pio_swd_halt_target(fun_pio_swd_t *swd,uint32_t timeout_ms)
{
    if(!swd||!swd->opened) return false;
    uint32_t cmd = DHCSR_DBGKEY|DHCSR_C_DEBUGEN|DHCSR_C_HALT;
    if(!fun_pio_swd_write_memory32(swd,SWD_DHCSR,cmd)) return false;
    while(timeout_ms-- >0){
        bool halted =0;
        if(fun_pio_swd_is_halted(swd,&halted)&&halted) return true;
        sleep_ms(1);
    }
    return false;
}

bool fun_pio_swd_run_target(fun_pio_swd_t *swd)
{
    if(!swd||!swd->opened) return false;
    uint32_t cmd= DHCSR_DBGKEY|DHCSR_C_DEBUGEN;
    return fun_pio_swd_write_memory32(swd,SWD_DHCSR,cmd);
}
bool fun_pio_swd_step_target(fun_pio_swd_t *swd,uint32_t timeout_ms)
{
    if(!swd || !swd->opened) return false;
    uint32_t cmd = DHCSR_DBGKEY|DHCSR_C_DEBUGEN | DHCSR_C_STEP;
    if(!fun_pio_swd_write_memory32(swd,SWD_DHCSR,cmd)) return false;
    sleep_us(10); //测试看看是否保留
    while(timeout_ms-- >0){
        bool halted =0;
        if(fun_pio_swd_is_halted(swd,&halted)&&halted){
            fun_pio_swd_write_memory32(swd, SWD_DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
            return true;
        }
        sleep_ms(1);
    }
    return false;
}

bool fun_pio_swd_reset_target(fun_pio_swd_t *swd)
{
    if(!swd || !swd->opened) return false;
    fun_pio_swd_write_memory32(swd, SWD_DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN);
    if (!fun_pio_swd_write_memory32(swd, SWD_AIRCR, 0x05FA0004u)) {
        return false;
    }
    return true;
}

static bool wait_dcrdr_ready(fun_pio_swd_t *swd, uint32_t timeout_us)
{
    while (timeout_us-- > 0) {
        uint32_t dhcsr = 0;
        if (!fun_pio_swd_read_memory32(swd, SWD_DHCSR, &dhcsr)) return false;
        if (dhcsr & DHCSR_S_REGRDY) return true;
        sleep_us(1);
    }
    return false;
}

bool fun_pio_swd_read_core_reg(fun_pio_swd_t *swd, uint8_t reg_id, uint32_t *value)
{
    if (!swd || !swd->opened || !value) return false;
    if (!fun_pio_swd_write_memory32(swd, SWD_DCRSR, reg_id & 0x1Fu)) return false;
    if (!wait_dcrdr_ready(swd, 2000)) return false;
    return fun_pio_swd_read_memory32(swd, SWD_DCRDR, value);
}

bool fun_pio_swd_write_core_reg(fun_pio_swd_t *swd, uint8_t reg_id, uint32_t value)
{
    if (!swd || !swd->opened) return false;
    if (!fun_pio_swd_write_memory32(swd, SWD_DCRDR, value)) return false;
    if (!fun_pio_swd_write_memory32(swd, SWD_DCRSR, (reg_id & 0x1Fu) | (1u << 16))) return false;
    return wait_dcrdr_ready(swd, 2000);
}

bool fun_pio_swd_read_memory_block32(fun_pio_swd_t *swd, uint32_t address,
                                     uint32_t *data, uint32_t word_count)
{
    if (!swd || !swd->opened || !data || word_count == 0 || (address & 3u)) return false;

    uint8_t ack = 0;
    if (!swd->mem_select_valid) {
        if (!pio_write_register(swd, false, 0x08u, 0u, &ack)) return false;
        swd->mem_select_valid = true;
    }
    const uint32_t csw = 0x23000012u;
    if (!swd->mem_csw_valid || swd->mem_csw != csw) {
        if (!pio_write_register(swd, true, 0x00u, csw, &ack)) return false;
        swd->mem_csw = csw;
        swd->mem_csw_valid = true;
    }
    if (!pio_write_register(swd, true, 0x04u, address, &ack)) return false;
    uint32_t dummy = 0;
    if (!pio_read_register(swd, true, 0x0Cu, &dummy, &ack)) return false;
    for (uint32_t i = 0; i < word_count - 1; i++) {
        uint32_t cur_addr = address + (i * 4);
        if (i > 0 && (cur_addr & 0x3FFu) == 0u) {
            if (!pio_write_register(swd, true, 0x04u, cur_addr, &ack)) return false;
            if (!pio_read_register(swd, true, 0x0Cu, &dummy, &ack)) return false;
        }
        if (!pio_read_register(swd, true, 0x0Cu, &data[i], &ack)) return false;
    }
    if (!pio_read_register(swd, false, 0x0Cu, &data[word_count - 1], &ack)) return false;

    swd->mem_tar_valid = false;
    return true;
}

bool fun_pio_swd_fpb_init(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return false;
    uint32_t demcr = 0;
    if (fun_pio_swd_read_memory32(swd, SWD_DEMCR, &demcr)) {
        if (!(demcr & (1u << 24))) {
            fun_pio_swd_write_memory32(swd, SWD_DEMCR, demcr | (1u << 24));
        }
    }
    return fun_pio_swd_write_memory32(swd, FP_CTRL, FP_CTRL_KEY | FP_CTRL_ENABLE);
}
bool fun_pio_swd_set_breakpoint(fun_pio_swd_t *swd, uint32_t flash_addr)
{
    if (!swd || !swd->opened) return false;
    uint32_t comp = (flash_addr & 0x1FFFFFFCu) | 1u;
    if (flash_addr & 2u) 
        comp |= (2u << 30); 
    else 
        comp |= (1u << 30); 
    return fun_pio_swd_write_memory32(swd, FP_COMP0, comp);
}

bool fun_pio_swd_clear_breakpoint(fun_pio_swd_t *swd)
{
    if (!swd || !swd->opened) return false;
    return fun_pio_swd_write_memory32(swd, FP_COMP0, 0u);
}
static bool read_thumb_instruction(fun_pio_swd_t *swd, uint32_t pc,
                                   uint16_t *hw1, uint16_t *hw2)
{
    uint32_t aligned_pc = pc & ~3u;
    uint32_t val1 = 0, val2 = 0;
    if (!fun_pio_swd_read_memory32(swd, aligned_pc, &val1)) return false;
    if ((pc & 2u) == 0) {
        *hw1 = (uint16_t)(val1 & 0xFFFFu);
        *hw2 = (uint16_t)(val1 >> 16);
    } else {
        *hw1 = (uint16_t)(val1 >> 16);
        if (!fun_pio_swd_read_memory32(swd, aligned_pc + 4u, &val2)) return false;
        *hw2 = (uint16_t)(val2 & 0xFFFFu);
    }
    return true;
}
static bool is_call_instruction(uint16_t hw1, uint16_t hw2, uint8_t *insn_len)
{
    if ((hw1 & 0xFF87) == 0x4780 && (hw1 & 0x0078) != 0x0078) {
        *insn_len = 2;
        return true;
    }
    if ((hw1 & 0xF800) == 0xF000) {
        *insn_len = 4;
        if ((hw2 & 0xD000) == 0xD000 || (hw2 & 0xD000) == 0xC000) {
            return true;
        }
    }
    if ((hw1 & 0xF800) >= 0xE800) {
        *insn_len = 4;
        return false;
    }
    *insn_len = 2;
    return false;
}
bool fun_pio_swd_step_over(fun_pio_swd_t *swd, uint32_t timeout_ms)
{
    if (!swd || !swd->opened) return false;
    uint32_t pc = 0;
    if (!fun_pio_swd_read_core_reg(swd, 15, &pc)) return false;
    uint16_t hw1 = 0, hw2 = 0;
    if (!read_thumb_instruction(swd, pc & ~1u, &hw1, &hw2)) return false;
    uint8_t insn_len = 2;
    bool is_call = is_call_instruction(hw1, hw2, &insn_len);
    if (!is_call) 
        return fun_pio_swd_step_target(swd, timeout_ms);
    uint32_t next_pc = (pc & ~1u) + insn_len;
    fun_pio_swd_fpb_init(swd);
    fun_pio_swd_set_breakpoint(swd, next_pc);
    fun_pio_swd_run_target(swd);
    bool hit_breakpoint = false;
    while (timeout_ms-- > 0) {
        bool halted = false;
        if (fun_pio_swd_is_halted(swd, &halted) && halted) {
            hit_breakpoint = true;
            break;
        }
        sleep_ms(1);
    }
    fun_pio_swd_clear_breakpoint(swd);
    if (!hit_breakpoint) 
        fun_pio_swd_halt_target(swd, 100);
    else 
        fun_pio_swd_write_memory32(swd, SWD_DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);

    return hit_breakpoint;
}

// bool fun_pio_swd_step_out(fun_pio_swd_t *swd, uint32_t timeout_ms)
// {
//     if (!swd || !swd->opened) return false;
//     uint32_t lr = 0;
//     if (!fun_pio_swd_read_core_reg(swd, 14, &lr)) return false;
//     if (lr == 0 || lr == 0xFFFFFFFFu || (lr & 0xF0000000u) == 0xF0000000u) 
//         return false;
//     uint32_t return_pc = lr & ~1u;
//     fun_pio_swd_fpb_init(swd);
//     fun_pio_swd_set_breakpoint(swd, return_pc);
//     fun_pio_swd_run_target(swd);
//     bool hit_breakpoint = false;
//     while (timeout_ms-- > 0) {
//         bool halted = false;
//         if (fun_pio_swd_is_halted(swd, &halted) && halted) {
//             hit_breakpoint = true;
//             break;
//         }
//         sleep_ms(1);
//     }
//     fun_pio_swd_clear_breakpoint(swd);
//     if (!hit_breakpoint) 
//         fun_pio_swd_halt_target(swd, 100);
//     else 
//         fun_pio_swd_write_memory32(swd, SWD_DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
//     return hit_breakpoint;
// }
bool fun_pio_swd_step_out(fun_pio_swd_t *swd, uint32_t timeout_ms)
{
    if (!swd || !swd->opened) return false;

    // 1. 读取当前 PC、SP 与硬件 LR
    uint32_t pc = 0, sp = 0, lr = 0;
    if (!fun_pio_swd_read_core_reg(swd, 15, &pc)) return false;
    if (!fun_pio_swd_read_core_reg(swd, 13, &sp)) return false;
    if (!fun_pio_swd_read_core_reg(swd, 14, &lr)) return false;

    // 中断返回码 (0xFxxxxxxx) 拦截
    if ((lr & 0xF0000000u) == 0xF0000000u) return false;

    // 2. 顶层拦截：如果在 main 函数范围 (0x08000510 ~ 0x08000566)，没有更上层，直接全速运行 (同 Keil)
    if (pc >= 0x08000510u && pc <= 0x08000566u) {
        fun_pio_swd_run_target(swd);
        return true; 
    }

    uint32_t return_pc = lr & ~1u;

    // 3. 脏数据拦截：
    // 如果 return_pc == pc (自身)，或者 return_pc 落入已知的延时子函数区 (0x0800022C ~ 0x08000270)，
    // 说明 LR 已经被冲刷，必须从栈顶寻找真正的返回地址
    if (return_pc == (pc & ~1u) || (return_pc >= 0x0800022Cu && return_pc <= 0x08000270u)) {
        uint32_t stack_buf[8];
        uint32_t real_lr = 0;
        if (fun_pio_swd_read_memory_block32(swd, sp & ~3u, stack_buf, 8)) {
            for (int i = 0; i < 8; i++) {
                uint32_t candidate = stack_buf[i];
                // 寻找位于 main 函数区间 (0x080005xx) 的有效返回地址
                if ((candidate & 0xFF000000u) == 0x08000000u && (candidate & 1u)) {
                    uint32_t target = candidate & ~1u;
                    if (target >= 0x08000510u && target <= 0x08000566u) {
                        real_lr = candidate;
                        break;
                    }
                }
            }
        }

        if (real_lr != 0) {
            return_pc = real_lr & ~1u; // 成功找到 main 函数内的返回点 (0x08000552)
        } else {
            // 堆栈里也找不到更外层时，直接全速运行
            fun_pio_swd_run_target(swd);
            return true;
        }
    }

    // 4. 设置硬件断点并放行
    fun_pio_swd_fpb_init(swd);
    fun_pio_swd_set_breakpoint(swd, return_pc);
    fun_pio_swd_run_target(swd);

    // 5. 等待命中
    bool hit_breakpoint = false;
    while (timeout_ms-- > 0) {
        bool halted = false;
        if (fun_pio_swd_is_halted(swd, &halted) && halted) {
            hit_breakpoint = true;
            break;
        }
        sleep_ms(1);
    }

    // 6. 清除断点与保底
    fun_pio_swd_clear_breakpoint(swd);
    if (!hit_breakpoint) {
        fun_pio_swd_halt_target(swd, 100);
    } else {
        fun_pio_swd_write_memory32(swd, SWD_DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
    }

    return hit_breakpoint;
}