/*
 * @Author: 轩
 * @Date: 2026-08-26 16:43:30
 * @LastEditTime: 2026-08-26 16:43:36
 * @FilePath: \pico\app\swd_flasher.h
 */
#ifndef SWD_FLASHER_H
#define SWD_FLASHER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLASHER_IDLE = 0,
    FLASHER_STAGE_CONNECT,      // 握手连接目标芯片
    FLASHER_STAGE_LOAD_ALGO,    // 从 TF 卡加载 .algo 并写入目标 SRAM
    FLASHER_STAGE_INIT_ALGO,    // 远程执行 Init()
    FLASHER_STAGE_ERASE,        // 远程执行 EraseChip()
    FLASHER_STAGE_PROGRAM,      // 循环写入 Buffer 并执行 ProgramPage()
    FLASHER_STAGE_UNINIT,       // 远程执行 UnInit()
    FLASHER_STAGE_SUCCESS,      // 烧录成功
    FLASHER_STAGE_ERROR         // 烧录失败
} flasher_stage_t;

typedef struct {
    flasher_stage_t stage;
    uint32_t        total_bytes;
    uint32_t        burned_bytes;
    uint8_t         progress_percent; // 0 ~ 100
    uint32_t        speed_kbs;
    uint32_t        cost_time_ms;
    bool            using_pio;
    bool            using_ram_algo;
    char            status_msg[64];
    char            chip_name[16];
} flasher_status_t;

void swd_flasher_init(void);
bool swd_flasher_start(const char *bin_path, const char *algo_path);
void swd_flasher_abort(void);
void swd_flasher_release_bus(void);
void swd_flasher_step(void);
const flasher_status_t *swd_flasher_get_status(void);

#ifdef __cplusplus
}
#endif

#endif // SWD_FLASHER_H
