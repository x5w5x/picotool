/*
 * @Author: 轩
 * @Date: 2026-09-06 12:31:01
 * @LastEditTime: 2026-09-09 19:58:03
 * @FilePath: \pico\pio\pio_manager.h
 */
#ifndef PIO_MANAGER_H
#define PIO_MANAGER_H

/*
 * PIO 资源管理器：记录 RP2040 的两个 PIO 单元、8 个状态机以及
 * 每个 PIO 的 32 条指令 RAM。具体 UART、SPI、SWD 时序由 .pio 文件实现。
 */

#include <stdbool.h>
#include <stdint.h>
#include <hardware/pio.h>
#ifdef __cplusplus
extern "C"
{
#endif

#define PIO_COUNT 2                 /* PIO0、PIO1 */
#define PIO_SM_COUNT 4              /* 每个 PIO 的状态机数量 */
#define PIO_PROGRAM_MAX_COUNT 8     /* 单个 PIO 的程序记录上限 */
#define PIO_MANAGER_ID_TO_BLOCK(id) \
    ((uint8_t)((id) / PIO_SM_COUNT))

#define PIO_MANAGER_ID_TO_SM(id) \
    ((uint8_t)((id) % PIO_SM_COUNT))

    typedef struct
    {
        bool used;              /* 软件记录的占用状态 */
        const char *label;      /* 使用者名称，不由管理器释放 */
    } pio_sm_state_t;
    typedef struct
    {
        bool loaded;                         /* 是否已经加载 */
        const char *name;                    /* 管理器内部名称 */
        uint8_t offset;                      /* 指令 RAM 起始位置 */
        uint8_t length;                      /* 占用的指令数 */
        uint8_t ref_count;                   /* 使用该程序的状态机数 */
        const struct pio_program *program;   /* SDK 生成的程序描述 */
    } pio_program_state_t;
    typedef struct
    {
        PIO pio;                             /* 对应的 SDK PIO 实例 */
        pio_sm_state_t sm[PIO_SM_COUNT];
        pio_program_state_t programs[PIO_PROGRAM_MAX_COUNT];

        uint8_t program_count;               /* 当前记录的程序数 */
        uint8_t instruction_used;            /* 已使用指令数统计 */
    } pio_state_t;

    typedef struct
    {
        pio_state_t block[PIO_COUNT];
    } pio_manager_t;
    /* 系统启动时调用一次，初始化软件表并绑定 pio0、pio1。 */
    void pio_manager_init(pio_manager_t *manager);


    /* 返回全局 SM 编号：0~3 为 PIO0，4~7 为 PIO1；失败返回 -1。 */
    int pio_manager_claim_sm(pio_manager_t *manager, const char *owner_name);

    /* 释放状态机，并解除 SDK 的硬件 claim。 */
    bool pio_manager_release_sm(pio_manager_t *manager, uint8_t pio_index, uint8_t sm);
    bool pio_manager_is_claimed(const pio_manager_t *manager, uint8_t pio_index, uint8_t sm);
    PIO pio_manager_get_pio(const pio_manager_t *manager, uint8_t pio_index);
    int pio_manager_claim_sm_from(pio_manager_t *manager, uint8_t pio_index, const char *owner_name);
    /* 查找指定 PIO 中已经加载的同名程序。 */
    int pio_manager_find_program(const pio_manager_t *manager, uint8_t pio_index, const char *program_name);
    bool pio_manager_has_instruction_space(const pio_manager_t *manager, uint8_t pio_index, uint8_t program_length);
    /* 加载程序；同名程序只增加引用计数，不重复占用指令 RAM。 */
    int pio_manager_load_program(pio_manager_t *manager, uint8_t pio_index, const char *program_name, const struct pio_program *program);
    bool pio_manager_release_program(pio_manager_t *manager, uint8_t pio_index, const char *program_name);

#ifdef __cplusplus
}
#endif
#endif
