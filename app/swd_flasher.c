#include "swd_flasher.h"
#include "algo_def.h"
#include "app_swd_probe.h"
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

#define PIN_SWCLK       2
#define PIN_SWDIO       3

static flasher_status_t s_flasher_status;
static FIL              s_bin_file;
static bool             s_bin_open = false;
static char             s_bin_path_cache[256];
static uint8_t          s_page_buffer[1024];
static uint64_t         s_flash_start_us = 0;

#define FIRMWARE_BUF_SIZE  (64 * 1024)
static uint8_t          s_firmware_buf[FIRMWARE_BUF_SIZE];
static uint32_t         s_firmware_size   = 0;
static uint32_t         s_firmware_offset = 0;
static bool             s_firmware_is_hex = false;

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
    memset(&s_flasher_status, 0, sizeof(flasher_status_t));
    s_flasher_status.stage = FLASHER_IDLE;
}

bool swd_flasher_start(const char *bin_path, const char *algo_path) {
    (void)algo_path;
    if (s_flasher_status.stage != FLASHER_IDLE &&
        s_flasher_status.stage != FLASHER_STAGE_SUCCESS &&
        s_flasher_status.stage != FLASHER_STAGE_ERROR) {
        return false;
    }

    if (s_bin_open) { f_close(&s_bin_file); s_bin_open = false; }

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
    s_flasher_status.stage        = FLASHER_STAGE_CONNECT;
    strcpy(s_flasher_status.chip_name, "STM32F10x");
    strcpy(s_flasher_status.status_msg, s_firmware_is_hex ? "正在连接 (HEX已就绪)..." : "正在连接 (BIN已就绪)...");
    s_flash_start_us = time_us_64();
    return true;
}

void swd_flasher_abort(void) {
    if (s_bin_open) {
        f_close(&s_bin_file);
        s_bin_open = false;
    }
    Target_Write(0xE000ED0C, 0x05FA0004);
    Target_Write(DHCSR, DHCSR_DBGKEY | 0x0000);
    gpio_set_dir(PIN_SWCLK, GPIO_IN);
    gpio_set_dir(PIN_SWDIO, GPIO_IN);
    s_flasher_status.stage = FLASHER_IDLE;
    strcpy(s_flasher_status.status_msg, "已中止烧录");
}

void swd_flasher_step(void) {
    if (s_flasher_status.stage == FLASHER_IDLE || 
        s_flasher_status.stage == FLASHER_STAGE_SUCCESS || 
        s_flasher_status.stage == FLASHER_STAGE_ERROR) {
        return;
    }

    switch (s_flasher_status.stage) {
        case FLASHER_STAGE_CONNECT: {
            if (!target_swd_init()) {
                snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "SWD 初始化失败 (无 ACK)");
                s_flasher_status.stage = FLASHER_STAGE_ERROR;
                break;
            }

            Target_Write(DHCSR, DHCSR_DBGKEY | 0x0003); // HALT
            sleep_ms(5);

            s_flasher_status.stage = FLASHER_STAGE_INIT_ALGO;
            strcpy(s_flasher_status.status_msg, "正在解锁 Flash 控制器...");
            break;
        }

        case FLASHER_STAGE_INIT_ALGO: {
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

        case FLASHER_STAGE_ERASE: {
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
            strcpy(s_flasher_status.status_msg, "正在烧录固件...");
            break;
        }

        case FLASHER_STAGE_PROGRAM: {
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

            uint64_t elapsed_us = time_us_64() - s_flash_start_us;
            if (elapsed_us > 1000) {
                s_flasher_status.speed_kbs = (uint32_t)((uint64_t)s_firmware_offset * 1000000ULL / elapsed_us / 1024);
            }
            break;
        }

        case FLASHER_STAGE_UNINIT: {
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

            // ⚡ 释放 SWD 引脚为高阻态
            gpio_set_dir(PIN_SWCLK, GPIO_IN);
            gpio_set_dir(PIN_SWDIO, GPIO_IN);

            s_flasher_status.cost_time_ms = (uint32_t)((time_us_64() - s_flash_start_us) / 1000);
            s_flasher_status.progress_percent = 100;
            s_flasher_status.stage = FLASHER_STAGE_SUCCESS;
            snprintf(s_flasher_status.status_msg, sizeof(s_flasher_status.status_msg), "烧录成功! SP:0x%lX PC:0x%lX", check_sp, check_pc);
            break;
        }

        default:
            break;
    }
}

const flasher_status_t *swd_flasher_get_status(void) {
    return &s_flasher_status;
}