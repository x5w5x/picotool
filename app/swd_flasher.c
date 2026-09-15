#include "swd_flasher.h"
#include "algo_def.h"
#include "fun_swd.h"
#include "fun_pio_swd.h"
#include "pio_manager.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#define STM32F1_FLASH_KEYR    0x40022004
#define STM32F1_FLASH_SR      0x4002200C
#define STM32F1_FLASH_CR      0x40022010
#define STM32F1_FLASH_AR      0x40022014
#define FLASH_KEY1            0x45670123
#define FLASH_KEY2            0xCDEF89AB
#define FLASH_CR_PG           (1U << 0)
#define FLASH_CR_MER          (1U << 2)
#define FLASH_CR_STRT         (1U << 6)
#define FLASH_CR_LOCK         (1U << 7)
#define FLASH_SR_BSY          (1U << 0)
#define FLASH_SR_PGERR        (1U << 2)
#define FLASH_SR_WRPRTERR     (1U << 4)

#define DHCSR           0xE000EDF0
#define DHCSR_DBGKEY    0xA05F0000
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT    (1u << 1)
#define DHCSR_S_REGRDY  (1u << 16)
#define DHCSR_S_HALT    (1u << 17)
#define DCRSR           0xE000EDF4
#define DCRDR           0xE000EDF8
#define DFSR            0xE000ED30
#define DEMCR           0xE000EDFC
#define DEMCR_FAULT_CATCH 0x000007F0u

#define PIN_SWCLK       2
#define PIN_SWDIO       3

static flasher_status_t s_flasher_status;
static FIL              s_bin_file;
static bool             s_bin_open = false;
static char             s_bin_path_cache[256];
static uint8_t          s_page_buffer[1024];
static uint64_t         s_flash_start_us = 0;
static uint64_t         s_program_start_us = 0;

#define FIRMWARE_BUF_SIZE  (32 * 1024)
static uint8_t          s_firmware_buf[FIRMWARE_BUF_SIZE];
static uint32_t         s_firmware_size   = 0;
static uint32_t         s_firmware_offset = 0;
static bool             s_firmware_is_hex = false;
static fun_pio_swd_t    s_flasher_pio_swd;
static bool             s_flasher_use_pio = false;
static bool             s_target_io_ok = true;
static uint8_t          s_pio_connect_stage = 0;
static uint8_t          s_pio_connect_ack = 0;
static uint8_t          s_pio_power_stage = 0;

#define ALGO_CODE_MAX_SIZE  2048u
static algo_header_t    s_algo_header;
static uint8_t          s_algo_code[ALGO_CODE_MAX_SIZE];
static uint32_t         s_algo_stub_addr = 0u;
static uint32_t         s_algo_stack_addr = 0u;
static uint8_t          s_target_call_stage = 0u;
static uint32_t         s_target_call_pc = 0u;

/* Offline flashing is forced to PIO. Software SWD remains in the project,
 * but must never be selected silently: otherwise a 10 MHz request can look
 * successful while the bus is actually running at the ~3 MHz GPIO limit. */
static void target_swd_close(void) {
    if (s_flasher_pio_swd.opened) {
        fun_pio_swd_close(&s_flasher_pio_swd);
    }
    s_flasher_use_pio = false;
}

static bool target_swd_init(void) {
    s_target_io_ok = false;
    s_pio_connect_stage = 0;
    s_pio_connect_ack = 0;
    s_pio_power_stage = 0;
    target_swd_close();
    fun_pio_swd_init(&s_flasher_pio_swd);
    if (!fun_pio_swd_open_auto(&s_flasher_pio_swd,
                               pio_manager_get_default(),
                               PIN_SWCLK, PIN_SWDIO, 10000000u)) {
        s_pio_connect_stage = 1; /* PIO program/SM allocation failed. */
        return false;
    }

    /* Keep exactly the same order as the proven PIO probe path. */
    uint32_t idcode = 0u;
    uint8_t ack = 0u;
    if (!fun_pio_swd_read_dp_idcode(&s_flasher_pio_swd,
                                    &idcode, &ack) ||
        ack != 1u || idcode == 0u || idcode == 0xFFFFFFFFu) {
        s_pio_connect_stage = 2;
        s_pio_connect_ack = ack;
        target_swd_close();
        return false;
    }
    if (!fun_pio_swd_power_up(&s_flasher_pio_swd)) {
        s_pio_connect_stage = 3;
        s_pio_connect_ack = s_flasher_pio_swd.last_write_ack;
        s_pio_power_stage = s_flasher_pio_swd.power_stage;
        target_swd_close();
        return false;
    }

    s_flasher_use_pio = true;
    s_target_io_ok = true;
    return true;
}

static uint32_t target_read32(uint32_t address) {
    if (!s_flasher_use_pio) return fun_swd_read(address);
    uint32_t value = 0xFFFFFFFFu;
    if (!fun_pio_swd_read_memory32(&s_flasher_pio_swd, address, &value)) {
        s_target_io_ok = false;
    }
    return value;
}

static void target_write32(uint32_t address, uint32_t value) {
    if (s_flasher_use_pio) {
        if (!fun_pio_swd_write_memory32(&s_flasher_pio_swd, address, value)) {
            s_target_io_ok = false;
        }
    } else {
        fun_swd_write(address, value);
    }
}

static bool target_write_block32(uint32_t address, const uint8_t *data,
                                 uint32_t length) {
    if (!s_flasher_use_pio) return false;
    if (!fun_pio_swd_write_memory_block32(&s_flasher_pio_swd,
                                           address, data, length)) {
        s_target_io_ok = false;
        return false;
    }
    return true;
}

#define Target_Read       target_read32
#define Target_Write      target_write32

static bool algo_offset_valid(uint32_t offset) {
    return offset != 0xFFFFFFFFu &&
           (offset & ~1u) < s_algo_header.code_size;
}

static bool load_algo_file(const char *path) {
    if (!path) return false;
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;

    UINT read_count = 0u;
    bool ok = f_read(&file, &s_algo_header, sizeof(s_algo_header),
                     &read_count) == FR_OK &&
              read_count == sizeof(s_algo_header);
    if (ok) {
        uint64_t ram_end = (uint64_t)s_algo_header.ram_base +
                           s_algo_header.ram_size;
        uint64_t buffer_end = (uint64_t)s_algo_header.buffer_addr +
                              s_algo_header.buffer_size;
        ok = s_algo_header.magic == ALGO_MAGIC &&
             s_algo_header.code_size > 0u &&
             s_algo_header.code_size <= ALGO_CODE_MAX_SIZE &&
             s_algo_header.ram_size >= 256u &&
             s_algo_header.buffer_size > 0u &&
             s_algo_header.buffer_addr >= s_algo_header.ram_base &&
             buffer_end <= ram_end;
    }
    if (ok) {
        ok = algo_offset_valid(s_algo_header.init_offset) &&
             algo_offset_valid(s_algo_header.uninit_offset) &&
             algo_offset_valid(s_algo_header.erase_chip_offset) &&
             algo_offset_valid(s_algo_header.program_page_offset);
    }
    if (ok) {
        read_count = 0u;
        ok = f_read(&file, s_algo_code, s_algo_header.code_size,
                    &read_count) == FR_OK &&
             read_count == s_algo_header.code_size;
    }
    f_close(&file);
    if (!ok) return false;

    /* Older flm2algo.py revisions accidentally stored FlashDevice::toProg
     * (100 ms) as buffer_size and the first sector size as ram_size. Keep
     * already-copied TF card algorithms usable and normalize the standard
     * STM32F1 Flash layout here. */
    if (s_algo_header.flash_base == 0x08000000u &&
        s_algo_header.buffer_size == 100u) {
        s_algo_header.ram_size = 4096u;
        s_algo_header.buffer_addr = s_algo_header.ram_base + 2048u;
        s_algo_header.buffer_size = 1024u;
    }

    /* Some compact custom algorithms describe the whole target SRAM and put
     * buffer_addr at ram_base. Loading a page there would overwrite the
     * algorithm itself. ProgramPage receives the buffer pointer in R2, so it
     * is safe to relocate that buffer behind a 4 KiB code/stack workspace. */
    uint32_t code_end = s_algo_header.ram_base + s_algo_header.code_size;
    if (s_algo_header.buffer_addr < code_end + 256u) {
        uint32_t relocated = s_algo_header.ram_base + 0x1000u;
        uint64_t relocated_end = (uint64_t)relocated +
                                 s_algo_header.buffer_size;
        uint64_t ram_end = (uint64_t)s_algo_header.ram_base +
                           s_algo_header.ram_size;
        if (relocated_end > ram_end) return false;
        s_algo_header.buffer_addr = relocated;
    }

    s_algo_stub_addr = (s_algo_header.ram_base +
                        s_algo_header.code_size + 3u) & ~3u;
    s_algo_stack_addr = s_algo_header.buffer_addr & ~7u;
    /* Reserve enough room between code/return stub and the descending stack. */
    return s_algo_stub_addr + 4u + 128u <= s_algo_stack_addr;
}

static bool target_wait_reg_ready(uint32_t timeout_us) {
    while (timeout_us-- > 0u) {
        uint32_t dhcsr = Target_Read(DHCSR);
        if (!s_target_io_ok) return false;
        if (dhcsr & DHCSR_S_REGRDY) return true;
        sleep_us(1);
    }
    return false;
}

static bool target_core_write_reg(uint8_t reg, uint32_t value) {
    Target_Write(DCRDR, value);
    Target_Write(DCRSR, (uint32_t)reg | (1u << 16));
    return s_target_io_ok && target_wait_reg_ready(2000u);
}

static bool target_core_read_reg(uint8_t reg, uint32_t *value) {
    if (!value) return false;
    Target_Write(DCRSR, reg);
    if (!s_target_io_ok || !target_wait_reg_ready(2000u)) return false;
    *value = Target_Read(DCRDR);
    return s_target_io_ok;
}

static bool target_halt(uint32_t timeout_ms) {
    Target_Write(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
    while (timeout_ms-- > 0u) {
        if (Target_Read(DHCSR) & DHCSR_S_HALT) return s_target_io_ok;
        if (!s_target_io_ok) return false;
        sleep_ms(1);
    }
    return false;
}

static bool target_call(uint32_t offset, uint32_t r0, uint32_t r1,
                        uint32_t r2, uint32_t r3, uint32_t timeout_ms,
                        uint32_t *return_value) {
    s_target_call_stage = 1u;
    s_target_call_pc = 0u;
    if (!algo_offset_valid(offset) || !target_halt(20u)) return false;

    Target_Write(DFSR, 0x1Fu);
    /* Halt on HardFault/BusFault/etc. Without vector catch a bad algorithm
     * address can enter lockup forever and looks exactly like a timeout. */
    Target_Write(DEMCR, DEMCR_FAULT_CATCH);
    if (!s_target_io_ok) return false;
    s_target_call_stage = 2u;
    if (!target_core_write_reg(0u, r0) ||
        !target_core_write_reg(1u, r1) ||
        !target_core_write_reg(2u, r2) ||
        !target_core_write_reg(3u, r3) ||
        !target_core_write_reg(13u, s_algo_stack_addr) ||
        !target_core_write_reg(14u, s_algo_stub_addr | 1u) ||
        !target_core_write_reg(15u, (s_algo_header.ram_base +
                                    (offset & ~1u)) | 1u) ||
        !target_core_write_reg(16u, 0x01000000u)) return false;

    s_target_call_stage = 3u;
    Target_Write(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN);
    if (!s_target_io_ok) return false;
    /* Let the AP write reach the core before looking at S_HALT. Without this
     * guard the first DHCSR read can still report the pre-resume halt. */
    sleep_us(20u);
    bool halted = false;
    s_target_call_stage = 4u;
    while (timeout_ms-- > 0u) {
        uint32_t dhcsr = Target_Read(DHCSR);
        if (!s_target_io_ok) return false;
        if (dhcsr & DHCSR_S_HALT) {
            uint32_t pc = 0u;
            if (!target_core_read_reg(15u, &pc)) return false;
            s_target_call_pc = pc;
            /* Cortex-M implementations may report BKPT, BKPT+2, or a
             * prefetched PC. After the ordered resume and delay, any halt is
             * a completed BKPT or a target fault; R0 distinguishes success. */
            halted = true;
            break;
        }
        sleep_ms(1);
    }
    if (!halted) {
        if (target_halt(20u)) {
            (void)target_core_read_reg(15u, &s_target_call_pc);
        }
        return false;
    }
    s_target_call_stage = 5u;
    if (!target_core_read_reg(0u, return_value)) return false;
    s_target_call_stage = 6u;
    return true;
}

/* ================= Intel HEX 解析 ================= */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t s_hex_base = 0x08000000;

static bool hex_parse_line(const char *line) {
    if (line[0] != ':') return false;
    const char *p = line + 1;

    int b0 = hex_nibble(p[0]), b1 = hex_nibble(p[1]);
    int c0 = hex_nibble(p[2]), c1 = hex_nibble(p[3]);
    int d0 = hex_nibble(p[4]), d1 = hex_nibble(p[5]);
    int e0 = hex_nibble(p[6]), e1 = hex_nibble(p[7]);
    if (b0 < 0 || b1 < 0 || c0 < 0 || c1 < 0 || d0 < 0 || d1 < 0 || e0 < 0 || e1 < 0) return false;

    uint8_t  byte_count = (uint8_t)((b0 << 4) | b1);
    uint16_t addr16     = (uint16_t)((c0 << 12) | (c1 << 8) | (d0 << 4) | d1);
    uint8_t  rectype    = (uint8_t)((e0 << 4) | e1);
    p += 8;

    switch (rectype) {
        case 0x00: {
            uint32_t absolute_addr = s_hex_base + addr16;
            for (int i = 0; i < byte_count; i++) {
                int hi = hex_nibble(p[2 * i]);
                int lo = hex_nibble(p[2 * i + 1]);
                if (hi < 0 || lo < 0) return false;
                uint8_t val = (uint8_t)((hi << 4) | lo);

                uint32_t ofs = (absolute_addr >= 0x08000000) ? 
                               (absolute_addr - 0x08000000 + i) : (absolute_addr + i);

                if (ofs < FIRMWARE_BUF_SIZE) {
                    s_firmware_buf[ofs] = val;
                    if (ofs + 1 > s_firmware_size) {
                        s_firmware_size = ofs + 1;
                    }
                }
            }
            return true;
        }

        case 0x01:
            return false;

        case 0x02: {
            int hi = hex_nibble(p[0]), lo = hex_nibble(p[1]);
            int hi2 = hex_nibble(p[2]), lo2 = hex_nibble(p[3]);
            if (hi < 0 || lo < 0 || hi2 < 0 || lo2 < 0) return false;
            uint16_t seg = (uint16_t)((hi << 12) | (lo << 8) | (hi2 << 4) | lo2);
            s_hex_base = ((uint32_t)seg) << 4;
            return true;
        }

        case 0x04: {
            int hi = hex_nibble(p[0]), lo = hex_nibble(p[1]);
            int hi2 = hex_nibble(p[2]), lo2 = hex_nibble(p[3]);
            if (hi < 0 || lo < 0 || hi2 < 0 || lo2 < 0) return false;
            uint16_t up = (uint16_t)((hi << 12) | (lo << 8) | (hi2 << 4) | lo2);
            s_hex_base = ((uint32_t)up) << 16;
            return true;
        }

        default:
            return true;
    }
}

static bool parse_intel_hex_file(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;

    memset(s_firmware_buf, 0xFF, sizeof(s_firmware_buf));
    s_firmware_size = 0;
    s_hex_base = 0x08000000;

    char line[600];
    UINT br = 0;
    uint16_t line_len = 0;
    bool eof_seen = false;
    bool ok_any = false;

    while (!eof_seen) {
        uint8_t ch;
        if (f_read(&f, &ch, 1, &br) != FR_OK || br == 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[line_len] = '\0';
            if (line_len > 0 && line[0] == ':') {
                if (!hex_parse_line(line)) eof_seen = true;
                ok_any = true;
            }
            line_len = 0;
            continue;
        }
        if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)ch;
        }
    }
    if (!eof_seen && line_len > 0 && line[0] == ':') {
        line[line_len] = '\0';
        hex_parse_line(line);
        ok_any = true;
    }
    f_close(&f);

    if (!ok_any || s_firmware_size == 0) return false;
    if (s_firmware_size & 1u) s_firmware_size++;
    return true;
}

static bool path_is_hex(const char *path) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return (strcmp(dot, ".hex") == 0 || strcmp(dot, ".HEX") == 0);
}

static bool load_bin_file(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;

    memset(s_firmware_buf, 0xFF, sizeof(s_firmware_buf));
    s_firmware_size = 0;

    UINT br = 0;
    FRESULT fr = f_read(&f, s_firmware_buf, sizeof(s_firmware_buf), &br);
    f_close(&f);
    if (fr != FR_OK) return false;
    s_firmware_size = br;
    if (s_firmware_size & 1u) s_firmware_size++;
    return true;
}

/* ================= 烧录状态机 ================= */

void swd_flasher_init(void) {
    fun_pio_swd_init(&s_flasher_pio_swd);
    s_flasher_use_pio = false;
    memset(&s_flasher_status, 0, sizeof(flasher_status_t));
    s_flasher_status.stage = FLASHER_IDLE;
}

bool swd_flasher_start(const char *bin_path, const char *algo_path) {
    if (s_flasher_status.stage != FLASHER_IDLE &&
        s_flasher_status.stage != FLASHER_STAGE_SUCCESS &&
        s_flasher_status.stage != FLASHER_STAGE_ERROR) {
        return false;
    }

    if (s_bin_open) { f_close(&s_bin_file); s_bin_open = false; }

    if (!load_algo_file(algo_path)) {
        memset(&s_flasher_status, 0, sizeof(s_flasher_status));
        s_flasher_status.stage = FLASHER_STAGE_ERROR;
        strcpy(s_flasher_status.status_msg, "Invalid ALGO file");
        return false;
    }

    s_firmware_is_hex = path_is_hex(bin_path);
    bool firmware_ok = false;
    if (s_firmware_is_hex) {
        firmware_ok = parse_intel_hex_file(bin_path);
        if (!firmware_ok) {
            s_flasher_status.stage = FLASHER_STAGE_ERROR;
            strcpy(s_flasher_status.status_msg, "HEX 文件解析失败");
            return false;
        }
    } else {
        firmware_ok = load_bin_file(bin_path);
        if (!firmware_ok) {
            s_flasher_status.stage = FLASHER_STAGE_ERROR;
            strcpy(s_flasher_status.status_msg, "BIN 文件读取失败");
            return false;
        }
    }

    strncpy(s_bin_path_cache, bin_path, sizeof(s_bin_path_cache) - 1);
    s_firmware_offset             = 0;
    s_flasher_status.total_bytes  = s_firmware_size;
    s_flasher_status.burned_bytes = 0;
    s_flasher_status.progress_percent = 0;
    s_flasher_status.using_pio = false;
    s_flasher_status.using_ram_algo = false;
    s_target_io_ok = true;
    s_flasher_status.stage        = FLASHER_STAGE_CONNECT;
    memcpy(s_flasher_status.chip_name, s_algo_header.chip_name,
           sizeof(s_flasher_status.chip_name));
    s_flasher_status.chip_name[sizeof(s_flasher_status.chip_name) - 1u] = '\0';
    strcpy(s_flasher_status.status_msg, s_firmware_is_hex ? "正在连接 (HEX已就绪)..." : "正在连接 (BIN已就绪)...");
    s_flash_start_us = time_us_64();
    s_program_start_us = 0;
    return true;
}

void swd_flasher_abort(void) {
    if (s_bin_open) {
        f_close(&s_bin_file);
        s_bin_open = false;
    }
    bool active = s_flasher_status.stage != FLASHER_IDLE &&
                  s_flasher_status.stage != FLASHER_STAGE_SUCCESS &&
                  s_flasher_status.stage != FLASHER_STAGE_ERROR;
    if (active && (s_flasher_use_pio || s_flasher_pio_swd.opened)) {
        Target_Write(0xE000ED0C, 0x05FA0004);
        Target_Write(DHCSR, DHCSR_DBGKEY | 0x0000);
    }
    target_swd_close();
    gpio_set_function(PIN_SWCLK, GPIO_FUNC_SIO);
    gpio_set_function(PIN_SWDIO, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_SWCLK, GPIO_IN);
    gpio_set_dir(PIN_SWDIO, GPIO_IN);
    s_flasher_status.stage = FLASHER_IDLE;
    strcpy(s_flasher_status.status_msg, "已中止烧录");
}

void swd_flasher_release_bus(void) {
    target_swd_close();
    gpio_set_function(PIN_SWCLK, GPIO_FUNC_SIO);
    gpio_set_function(PIN_SWDIO, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_SWCLK, GPIO_IN);
    gpio_set_dir(PIN_SWDIO, GPIO_IN);
}

void swd_flasher_step(void) {
    if (s_flasher_status.stage == FLASHER_IDLE || 
        s_flasher_status.stage == FLASHER_STAGE_SUCCESS || 
        s_flasher_status.stage == FLASHER_STAGE_ERROR) {
        if (s_flasher_status.stage == FLASHER_STAGE_ERROR &&
            s_flasher_pio_swd.opened) {
            target_swd_close();
        }
        return;
    }

    /* Total time covers connect, erase, program, verify and reset. */
    s_flasher_status.cost_time_ms =
        (uint32_t)((time_us_64() - s_flash_start_us) / 1000u);

    switch (s_flasher_status.stage) {
        case FLASHER_STAGE_CONNECT: {
            if (!target_swd_init()) {
                if (s_pio_connect_stage == 1u) {
                    snprintf(s_flasher_status.status_msg,
                             sizeof(s_flasher_status.status_msg),
                             "PIO资源不足(SM/指令空间)");
                } else if (s_pio_connect_stage == 2u) {
                    snprintf(s_flasher_status.status_msg,
                             sizeof(s_flasher_status.status_msg),
                             "PIO读DP ID失败 ACK=%u", s_pio_connect_ack);
                } else {
                    snprintf(s_flasher_status.status_msg,
                             sizeof(s_flasher_status.status_msg),
                             "PIO上电失败 stage=%u ACK=%u",
                             s_pio_power_stage,
                             s_pio_connect_ack);
                }
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }

            s_flasher_status.using_pio = s_flasher_use_pio;

            Target_Write(DHCSR, DHCSR_DBGKEY | 0x0003); // HALT
            sleep_ms(5);

            s_flasher_status.stage = FLASHER_STAGE_LOAD_ALGO;
            strcpy(s_flasher_status.status_msg, "PIO 10MHz: 正在解锁Flash...");
            break;
        }

        case FLASHER_STAGE_LOAD_ALGO: {
            static const uint8_t stub[4] = {0x00u, 0xBEu, 0xFEu, 0xE7u};
            if (!target_write_block32(s_algo_header.ram_base, s_algo_code,
                                      s_algo_header.code_size) ||
                !target_write_block32(s_algo_stub_addr, stub, sizeof(stub))) {
                strcpy(s_flasher_status.status_msg, "ALGO upload failed");
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            uint32_t expected = (uint32_t)s_algo_code[0] |
                                ((uint32_t)s_algo_code[1] << 8) |
                                ((uint32_t)s_algo_code[2] << 16) |
                                ((uint32_t)s_algo_code[3] << 24);
            uint32_t actual = Target_Read(s_algo_header.ram_base);
            if (!s_target_io_ok || actual != expected) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "ALGO verify %08lX/%08lX",
                         (unsigned long)actual, (unsigned long)expected);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            s_flasher_status.using_ram_algo = true;
            s_flasher_status.stage = FLASHER_STAGE_INIT_ALGO;
            strcpy(s_flasher_status.status_msg, "Running ALGO Init...");
            break;
        }

        case FLASHER_STAGE_INIT_ALGO: {
            uint32_t result = 0xFFFFFFFFu;
            if (!target_call(s_algo_header.init_offset,
                             s_algo_header.flash_base, 0u, 1u, 0u,
                             1000u, &result) || result != 0u) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "Init fail S%u R%lu PC%08lX",
                         s_target_call_stage, (unsigned long)result,
                         (unsigned long)s_target_call_pc);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            s_flasher_status.stage = FLASHER_STAGE_ERASE;
            strcpy(s_flasher_status.status_msg, "Running EraseChip...");
            break;
#if 0
            Target_Write(STM32F1_FLASH_KEYR, FLASH_KEY1);
            Target_Write(STM32F1_FLASH_KEYR, FLASH_KEY2);
            sleep_ms(2);

            uint32_t cr = Target_Read(STM32F1_FLASH_CR);
            if (cr & FLASH_CR_LOCK) {
                snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "Flash 解锁失败 CR=0x%lX", cr);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                return;
            }

            s_flasher_status.stage = FLASHER_STAGE_ERASE;
            strcpy(s_flasher_status.status_msg, "正在擦除片上 Flash...");
            break;
        }

#endif
        }

        case FLASHER_STAGE_ERASE: {
            uint32_t result = 0xFFFFFFFFu;
            if (!target_call(s_algo_header.erase_chip_offset,
                             0u, 0u, 0u, 0u, 10000u, &result) ||
                result != 0u) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "Erase failed ret=%lu", (unsigned long)result);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            s_flasher_status.stage = FLASHER_STAGE_PROGRAM;
            s_program_start_us = time_us_64();
            strcpy(s_flasher_status.status_msg, "RAM algorithm programming...");
            break;
#if 0
            Target_Write(STM32F1_FLASH_CR, FLASH_CR_MER);
            Target_Write(STM32F1_FLASH_CR, FLASH_CR_MER | FLASH_CR_STRT);

            int timeout = 5000;
            while (timeout-- > 0) {
                if ((Target_Read(STM32F1_FLASH_SR) & FLASH_SR_BSY) == 0) break;
                sleep_ms(1);
            }
            uint32_t sr = Target_Read(STM32F1_FLASH_SR);
            Target_Write(STM32F1_FLASH_CR, 0x00);

            if (timeout <= 0 || (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR))) {
                snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "擦除失败 SR=0x%lX", sr);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                return;
            }

            s_flasher_status.stage = FLASHER_STAGE_PROGRAM;
            s_program_start_us = time_us_64();
            strcpy(s_flasher_status.status_msg, "正在烧录固件...");
            break;
        }

#endif
        }

        case FLASHER_STAGE_PROGRAM: {
            if (s_firmware_offset >= s_firmware_size) {
                s_flasher_status.stage = FLASHER_STAGE_UNINIT;
                strcpy(s_flasher_status.status_msg, "Running ALGO UnInit...");
                break;
            }
            uint32_t count = s_firmware_size - s_firmware_offset;
            if (count > s_algo_header.buffer_size)
                count = s_algo_header.buffer_size;
            uint32_t flash_addr = s_algo_header.flash_base + s_firmware_offset;
            if (!target_write_block32(s_algo_header.buffer_addr,
                                      &s_firmware_buf[s_firmware_offset],
                                      count)) {
                strcpy(s_flasher_status.status_msg, "Page buffer upload failed");
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            uint32_t result = 0xFFFFFFFFu;
            if (!target_call(s_algo_header.program_page_offset,
                             flash_addr, count, s_algo_header.buffer_addr, 0u,
                             2000u, &result) || result != 0u) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "Program %08lX ret=%lu",
                         (unsigned long)flash_addr, (unsigned long)result);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            s_firmware_offset += count;
            s_flasher_status.burned_bytes = s_firmware_offset;
            s_flasher_status.progress_percent = (uint8_t)(
                (uint64_t)s_firmware_offset * 100u /
                s_flasher_status.total_bytes);
            uint64_t elapsed = time_us_64() - s_program_start_us;
            if (elapsed > 0u)
                s_flasher_status.speed_kbs = (uint32_t)(
                    (uint64_t)s_firmware_offset * 1000000u / elapsed / 1024u);
            break;
#if 0
            const uint32_t chunk = 1024;
            if (s_firmware_offset >= s_firmware_size) {
                s_flasher_status.stage = FLASHER_STAGE_UNINIT;
                strcpy(s_flasher_status.status_msg, "固件写入完毕，正在校验与复位...");
                return;
            }

            uint32_t br = s_firmware_size - s_firmware_offset;
            if (br > chunk) br = chunk;
            memcpy(s_page_buffer, &s_firmware_buf[s_firmware_offset], br);

            uint32_t flash_addr = 0x08000000 + s_firmware_offset;

            // 16 位半字编程模式
            Target_Write(STM32F1_FLASH_CR, FLASH_CR_PG);

            for (uint32_t i = 0; i < br; i += 2) {
                uint16_t hw = (uint16_t)s_page_buffer[i] | ((uint16_t)s_page_buffer[i + 1] << 8);
                Target_WriteHalf(flash_addr + i, hw);
                if (!s_target_io_ok) {
                    snprintf(s_flasher_status.status_msg,
                             sizeof(s_flasher_status.status_msg),
                             "SWD write failed @0x%lX", flash_addr + i);
                    s_flasher_status.stage = FLASHER_STAGE_ERROR;
                    target_swd_close();
                    return;
                }

                int busy_wait = 200;
                while (busy_wait-- > 0) {
                    if ((Target_Read(STM32F1_FLASH_SR) & FLASH_SR_BSY) == 0) break;
                    sleep_us(10);
                }
            }

            Target_Write(STM32F1_FLASH_CR, 0x00);

            uint32_t sr = Target_Read(STM32F1_FLASH_SR);
            if (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
                snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "编程错误 SR=0x%lX @0x%lX", sr, flash_addr);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                return;
            }

            s_firmware_offset += br;
            s_flasher_status.burned_bytes = s_firmware_offset;
            if (s_flasher_status.total_bytes > 0) {
                s_flasher_status.progress_percent = (uint8_t)((uint64_t)s_firmware_offset * 100 / s_flasher_status.total_bytes);
            }

            uint64_t program_us = time_us_64() - s_program_start_us;
            if (s_program_start_us != 0u && program_us > 0u) {
                s_flasher_status.speed_kbs =
                    (uint32_t)((uint64_t)s_firmware_offset * 1000000ULL /
                               program_us / 1024ULL);
            }
            break;
        }

#endif
        }

        case FLASHER_STAGE_UNINIT: {
            uint32_t result = 0xFFFFFFFFu;
            if (!target_call(s_algo_header.uninit_offset,
                             1u, 0u, 0u, 0u, 1000u, &result) ||
                result != 0u) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "UnInit failed ret=%lu", (unsigned long)result);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            uint32_t check_sp = Target_Read(s_algo_header.flash_base);
            uint32_t check_pc = Target_Read(s_algo_header.flash_base + 4u);
            if (!s_target_io_ok || check_sp == 0u ||
                check_sp == 0xFFFFFFFFu || check_pc == 0xFFFFFFFFu) {
                snprintf(s_flasher_status.status_msg,
                         sizeof(s_flasher_status.status_msg),
                         "Vector verify SP=%08lX PC=%08lX",
                         (unsigned long)check_sp, (unsigned long)check_pc);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }
            Target_Write(0xE000EDFC, 0u);
            Target_Write(DHCSR, DHCSR_DBGKEY);
            Target_Write(0xE000ED0C, 0x05FA0004u);
            sleep_ms(10);
            target_swd_close();
            gpio_set_function(PIN_SWCLK, GPIO_FUNC_SIO);
            gpio_set_function(PIN_SWDIO, GPIO_FUNC_SIO);
            gpio_set_dir(PIN_SWCLK, GPIO_IN);
            gpio_set_dir(PIN_SWDIO, GPIO_IN);
            s_flasher_status.cost_time_ms =
                (uint32_t)((time_us_64() - s_flash_start_us) / 1000u);
            s_flasher_status.progress_percent = 100u;
            s_flasher_status.stage = FLASHER_STAGE_SUCCESS;
            snprintf(s_flasher_status.status_msg,
                     sizeof(s_flasher_status.status_msg),
                     "PIO RAM ALGO OK SP=%08lX", (unsigned long)check_sp);
            break;
#if 0
            uint32_t check_sp = Target_Read(0x08000000);
            uint32_t check_pc = Target_Read(0x08000004);

            if (check_sp == 0xFFFFFFFF || check_pc == 0xFFFFFFFF || check_sp == 0x00000000) {
                snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), 
                    "校验失败: 向量全空 SP=0x%lX PC=0x%lX", check_sp, check_pc);
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                return;
            }

            Target_Write(STM32F1_FLASH_CR, FLASH_CR_LOCK);

            // 彻底释放调试态
            Target_Write(0xE000EDFC, 0x00000000);        // DEMCR = 0
            Target_Write(DHCSR, DHCSR_DBGKEY | 0x0000); // C_DEBUGEN = 0
            sleep_ms(5);
            Target_Write(0xE000ED0C, 0x05FA0004);        // SYSRESETREQ (软复位)
            sleep_ms(10);

            target_swd_close();
            gpio_set_function(PIN_SWCLK, GPIO_FUNC_SIO);
            gpio_set_function(PIN_SWDIO, GPIO_FUNC_SIO);

            // ⚡ 释放 SWD 引脚为高阻态
            gpio_set_dir(PIN_SWCLK, GPIO_IN);
            gpio_set_dir(PIN_SWDIO, GPIO_IN);

            s_flasher_status.cost_time_ms = (uint32_t)((time_us_64() - s_flash_start_us) / 1000);
            s_flasher_status.progress_percent = 100;
            s_flasher_status.stage = FLASHER_STAGE_SUCCESS;
            snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "烧录成功! SP:0x%lX PC:0x%lX", check_sp, check_pc);
            break;
        }

#endif
        }

        default:
            break;
    }
}

const flasher_status_t *swd_flasher_get_status(void) {
    return &s_flasher_status;
}
