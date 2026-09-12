/*
 * @Author: 轩
 * @Date: 2026-09-06 12:31:01
 * @LastEditTime: 2026-09-12 14:32:28
 * @FilePath: \pico\pio\pio_manager.h
 */
#ifndef PIO_MANAGER_H
#define PIO_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"


#ifdef __cplusplus
extern "C" {
#endif

#define PIO_BLOCK_COUNT 2
#define PIO_SM_COUNT 4
#define PIO_PROGRAM_MAX_COUNT 8
#define PIO_CMD_COUNT 32

typedef struct{
    bool used;
    const char *owner;    
}pio_manager_sm_t;
typedef struct{
    bool loaded;
    const char *name;
    uint offset;
    uint len;
    uint8_t ref_count;
    const struct pio_program *program;
}pio_manager_program_t;

typedef struct {
    PIO hardware;
    pio_manager_sm_t sm[PIO_SM_COUNT];
    pio_manager_program_t programs[PIO_PROGRAM_MAX_COUNT];
    uint8_t program_count;
    uint8_t cmd_used;
}pio_manager_block_t;

typedef struct {
    pio_manager_block_t blocks[PIO_BLOCK_COUNT];
}pio_manager_t;
typedef struct {
    uint8_t used_sm_count;
    uint8_t total_sm_count;

    uint8_t used_cmd_count;
    uint8_t total_cmd_count;
    uint8_t program_count;
    uint8_t total_program_count;
}pio_manager_stats_t;

void pio_manager_init(pio_manager_t *manager);
bool pio_manager_release_sm(pio_manager_t *manager,uint8_t pio_index,uint8_t sm);
int pio_manager_claim_sm_from(pio_manager_t *manager,uint8_t pio_index,const char *owner);
PIO pio_manager_get_hardware(const pio_manager_t *manager,uint8_t pio_index);
bool pio_manager_has_cmd_space(const pio_manager_t *manager,uint8_t pio_index,uint8_t len);
int pio_manager_find_program(const pio_manager_t *manager,uint8_t pio_index,const char* name);
int pio_manager_load_program(pio_manager_t *manager,uint8_t pio_index,const char *name,const struct pio_program *program);
bool pio_manager_release_program(pio_manager_t *manager,uint8_t pio_index,const char *name);
uint8_t pio_manager_get_used_sm_count(const pio_manager_t *manager,uint8_t pio_index);
uint8_t pio_manager_get_used_cmd_count(const pio_manager_t *manager,uint8_t pio_index);
bool pio_manager_is_sm_used(const pio_manager_t *manager,uint8_t pio_index,uint8_t sm);
const char *pio_manager_get_sm_owner(const pio_manager_t *manager,uint8_t pio_index,uint8_t sm);
const pio_manager_program_t *pio_manager_get_program_info(const pio_manager_t *manager,uint8_t pio_index,uint8_t program_index);
uint8_t pio_manager_get_program_count(const pio_manager_t *manager,uint8_t pio_index);
bool pio_manager_get_stats(const pio_manager_t *manager,uint8_t pio_index,pio_manager_stats_t *stats);
bool pio_manager_can_load_program(const pio_manager_t *manager,uint8_t pio_index,const struct pio_program *program);
bool pio_manager_can_use_program(const pio_manager_t *manager,uint8_t pio_index,const char * name,const struct pio_program *program);
uint8_t pio_manager_get_free_sm_count(const pio_manager_t *manager,uint8_t pio_index);
pio_manager_t *pio_manager_get_default(void);
void pio_manager_system_init(void);
#ifdef __cplusplus
}
#endif

#endif

