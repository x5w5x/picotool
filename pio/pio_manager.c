/*
 * @Author: 轩
 * @Date: 2026-09-06 12:40:44
 * @LastEditTime: 2026-09-06 13:06:29
 * @FilePath: \pico\pio\pio_manager.c
 */
#include "pio_manager.h"

#include <stddef.h>
#include <string.h>

void pio_manager_init(pio_manager_t *manager)
{
    if (manager == NULL)
        return;

    /* 建立软件资源表与 SDK 中两个 PIO 实例的对应关系。 */
    manager->block[0].pio = pio0;
    manager->block[1].pio = pio1;

    for (uint8_t pio_index = 0;
         pio_index < PIO_COUNT;
         ++pio_index) {
        /* 状态机属于 PIO 硬件资源，每个 PIO 有 4 个 SM。 */
        for (uint8_t sm = 0;
             sm < PIO_SM_COUNT;
             ++sm) {
            manager->block[pio_index].sm[sm].used = false;
            manager->block[pio_index].sm[sm].label = NULL;
        }

        /* 指令 RAM 属于整个 PIO，不属于某一个状态机。 */
        manager->block[pio_index].program_count = 0;
        manager->block[pio_index].instruction_used = 0;
        for (uint8_t program_index = 0;
             program_index < PIO_PROGRAM_MAX_COUNT;
             ++program_index) {
            pio_program_state_t *program =
                &manager->block[pio_index].programs[program_index];
            program->loaded = false;
            program->name = NULL;
            program->offset = 0;
            program->length = 0;
            program->ref_count = 0;
            program->program = NULL;
        }
    }
}
int pio_manager_claim_sm(pio_manager_t *manager,
                         const char *owner_name)
{
    if (manager == NULL) {
        return -1;
    }

    /* 从 PIO0 到 PIO1 查找第一个可用状态机。 */
    for (uint8_t pio_index = 0;
         pio_index < PIO_COUNT;
         ++pio_index) {
        PIO pio = manager->block[pio_index].pio;

        for (uint8_t sm = 0;
             sm < PIO_SM_COUNT;
             ++sm) {
            pio_sm_state_t *state =
                &manager->block[pio_index].sm[sm];

            /* 同时检查管理器记录和 SDK 状态，避免重复占用。 */
            if (!state->used && !pio_sm_is_claimed(pio, sm)) {
                pio_sm_claim(pio, sm);

                state->used = true;
                state->label = owner_name;

                return pio_index * PIO_SM_COUNT + sm;
            }
        }
    }

    return -1;
}
bool pio_manager_release_sm(pio_manager_t *manager,
                            uint8_t pio_index,
                            uint8_t sm)
{
    if (manager == NULL) {
        return false;
    }

    if (pio_index >= PIO_COUNT ||
        sm >= PIO_SM_COUNT) {
        return false;
    }

    PIO pio = manager->block[pio_index].pio;

    if (manager->block[pio_index].sm[sm].used) {
        pio_sm_unclaim(pio, sm);

        manager->block[pio_index].sm[sm].used = false;
        manager->block[pio_index].sm[sm].label = NULL;

        return true;
    }

    return false;
}

bool pio_manager_is_claimed(const pio_manager_t *manager,uint8_t pio_index,uint8_t sm)
{
    if (manager == NULL) {
        return false;
    }

    if (pio_index >= PIO_COUNT ||
        sm >= PIO_SM_COUNT) {
        return false;
    }

    return manager->block[pio_index].sm[sm].used;
}

PIO pio_manager_get_pio(const pio_manager_t *manager,
                        uint8_t pio_index)
{
    if (manager == NULL) {
        return NULL;
    }

    if (pio_index >= PIO_COUNT) {
        return NULL;
    }

    return manager->block[pio_index].pio;
}

int pio_manager_find_program(const pio_manager_t *manager,
                             uint8_t pio_index,
                             const char *program_name)
{
    if (manager == NULL || program_name == NULL || pio_index >= PIO_COUNT) {
        return -1;
    }

    const pio_state_t *block = &manager->block[pio_index];
    for (uint8_t i = 0; i < block->program_count; ++i) {
        const pio_program_state_t *program = &block->programs[i];
        if (program->loaded && program->name != NULL &&
            strcmp(program->name, program_name) == 0) {
            return i;
        }
    }

    return -1;
}

bool pio_manager_has_instruction_space(const pio_manager_t *manager,
                                       uint8_t pio_index,
                                       uint8_t program_length)
{
    if (manager == NULL || pio_index >= PIO_COUNT) {
        return false;
    }

    return program_length <= PIO_INSTRUCTION_COUNT &&
           manager->block[pio_index].instruction_used + program_length <=
               PIO_INSTRUCTION_COUNT;
}

int pio_manager_load_program(pio_manager_t *manager,
                             uint8_t pio_index,
                             const char *program_name,
                             const struct pio_program *program)
{
    if (manager == NULL || program_name == NULL || program == NULL ||
        pio_index >= PIO_COUNT) {
        return -1;
    }

    /* 同一 PIO 上的同名程序只加载一次，后续只增加引用计数。 */
    int existing = pio_manager_find_program(manager, pio_index, program_name);
    if (existing >= 0) {
        manager->block[pio_index].programs[existing].ref_count++;
        return manager->block[pio_index].programs[existing].offset;
    }

    pio_state_t *block = &manager->block[pio_index];

    /* 加载前同时检查管理器统计和 SDK 的实际指令空间。 */
    if (block->program_count >= PIO_PROGRAM_MAX_COUNT ||
        !pio_manager_has_instruction_space(manager, pio_index,
                                            (uint8_t)program->length) ||
        !pio_can_add_program(block->pio, program)) {
        return -1;
    }

    uint offset = pio_add_program(block->pio, program);
    pio_program_state_t *state =
        &block->programs[block->program_count];
    state->loaded = true;
    state->name = program_name;
    state->offset = (uint8_t)offset;
    state->length = (uint8_t)program->length;
    state->ref_count = 1;
    state->program = program;

    block->program_count++;
    block->instruction_used += (uint8_t)program->length;
    return (int)offset;
}

bool pio_manager_release_program(pio_manager_t *manager,
                                 uint8_t pio_index,
                                 const char *program_name)
{
    if (manager == NULL || program_name == NULL ||
        pio_index >= PIO_COUNT) {
        return false;
    }

    int index = pio_manager_find_program(
        manager, pio_index, program_name);
    if (index < 0) {
        return false;
    }

    pio_state_t *block = &manager->block[pio_index];
    pio_program_state_t *state = &block->programs[index];

    if (state->ref_count > 1) {
        state->ref_count--;
        return true;
    }

    /* 最后一个使用者释放后，才从 PIO 指令 RAM 删除程序。 */
    if (state->program != NULL) {
        pio_remove_program(block->pio,
                           state->program,
                           state->offset);
    }

    if (block->instruction_used >= state->length) {
        block->instruction_used -= state->length;
    } else {
        block->instruction_used = 0;
    }

    state->loaded = false;
    state->name = NULL;
    state->offset = 0;
    state->length = 0;
    state->ref_count = 0;
    state->program = NULL;

    return true;
}

int pio_manager_claim_sm_from(pio_manager_t *manager,uint8_t pio_index,const char *owner_name)
{
    if (manager == NULL) {
        return -1;
    }
    if (pio_index >= PIO_COUNT) {
        return -1;
    }
    PIO pio = manager->block[pio_index].pio;
    for (uint8_t sm = 0;
         sm < PIO_SM_COUNT;
         ++sm) {
        pio_sm_state_t *state =
            &manager->block[pio_index].sm[sm];

        if (!state->used && !pio_sm_is_claimed(pio, sm)) {
            pio_sm_claim(pio, sm);
            state->used = true;
            state->label = owner_name;
            return pio_index * PIO_SM_COUNT + sm;
        }
    }
    return -1;
}
