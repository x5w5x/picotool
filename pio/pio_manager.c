/*
 * @Author: 轩
 * @Date: 2026-09-06 12:40:44
 * @LastEditTime: 2026-09-12 14:42:13
 * @FilePath: \pico\pio\pio_manager.c
 */
#include "pio_manager.h"
#include <stddef.h>
#include <string.h>

static pio_manager_t s_default_manager;
static bool s_default_manager_initialized = false;
void pio_manager_init(pio_manager_t *manager){
    if(!manager) return;
    manager->blocks[0].hardware=pio0;
    manager->blocks[1].hardware=pio1;
    for(uint8_t block=0;block<PIO_BLOCK_COUNT;++block){
        manager->blocks[block].cmd_used=0;
        manager->blocks[block].program_count=0;
    for (uint8_t program = 0;program < PIO_PROGRAM_MAX_COUNT;++program) {
        manager->blocks[block].programs[program].loaded = false;
        manager->blocks[block].programs[program].name = NULL;
        manager->blocks[block].programs[program].offset = 0;
        manager->blocks[block].programs[program].len = 0;
        manager->blocks[block].programs[program].ref_count = 0;
        manager->blocks[block].programs[program].program = NULL;
    }   
    for(uint8_t sm=0;sm<PIO_SM_COUNT;++sm){
        manager->blocks[block].sm[sm].used=false;
        manager->blocks[block].sm[sm].owner=NULL;
    }
}
}

bool pio_manager_release_sm(pio_manager_t *manager,uint8_t pio_index,uint8_t sm){
    if(manager == NULL) return false;
    if(pio_index>=PIO_BLOCK_COUNT||sm>=PIO_SM_COUNT) return false;
    pio_manager_sm_t *state=&manager->blocks[pio_index].sm[sm];
    if(!state->used) return false;

    PIO pio=manager->blocks[pio_index].hardware;
    pio_sm_unclaim(pio,sm);
    state->used=false;
    state->owner=NULL;
    return true;
}

int pio_manager_claim_sm_from(pio_manager_t *manager,uint8_t pio_index,const char *owner){

    if(manager==NULL|| pio_index>=PIO_BLOCK_COUNT||owner==NULL) return -1;
   
    PIO pio =manager->blocks[pio_index].hardware;
    for(uint8_t sm=0;sm<PIO_SM_COUNT;++sm){
        pio_manager_sm_t *state = &manager->blocks[pio_index].sm[sm];
        if(state->used) continue;
        if(pio_sm_is_claimed(pio,sm)) continue;
        pio_sm_claim(pio,sm);
        state->used=true;
        state->owner=owner;
        return (int)(pio_index * PIO_SM_COUNT+sm);
    }
    return -1;
}

PIO pio_manager_get_hardware(const pio_manager_t *manager,uint8_t pio_index){
    if(manager==NULL) return NULL;
    if(pio_index>=PIO_BLOCK_COUNT) return NULL;
    return manager->blocks[pio_index].hardware;
}

bool pio_manager_has_cmd_space(const pio_manager_t *manager,uint8_t pio_index,uint8_t len){
    if(manager==NULL) return false;
    if(pio_index>=PIO_BLOCK_COUNT) return false;
    if(len>PIO_CMD_COUNT) return false;
    return manager->blocks[pio_index].cmd_used+len <= PIO_CMD_COUNT;
}

int pio_manager_find_program(const pio_manager_t *manager,uint8_t pio_index,const char* name){
    if(manager==NULL||name==NULL||pio_index>=PIO_BLOCK_COUNT) return -1;
    const pio_manager_block_t *block=&manager->blocks[pio_index];
    for(uint8_t index=0;index<block->program_count;++index){
        const pio_manager_program_t *program=&block->programs[index];
        if(!program->loaded||program->name==NULL)
         continue;
         if(strcmp(program->name,name)==0)
         return (int)index;
    }
    return -1;
}

int pio_manager_load_program(pio_manager_t *manager,uint8_t pio_index,const char *name,const struct pio_program *program)
{
    if (manager == NULL ||name == NULL || program == NULL || pio_index >= PIO_BLOCK_COUNT) 
        return -1;
    int existing =pio_manager_find_program(manager, pio_index, name);
    if (existing >= 0) {
        pio_manager_program_t *loaded = &manager->blocks[pio_index].programs[existing];
        if(loaded->ref_count == UINT8_MAX) return -1;

        if(loaded->program !=program) return -1;
        loaded->ref_count++;
        return (int)loaded->offset;
    }
    pio_manager_block_t *block = &manager->blocks[pio_index];
    if (block->program_count >= PIO_PROGRAM_MAX_COUNT||!pio_manager_has_cmd_space(manager,pio_index,(uint8_t)program->length)) 
        return -1;


    if (!pio_can_add_program(block->hardware, program)) 
        return -1;
    uint offset =pio_add_program(block->hardware, program);
    pio_manager_program_t *record =&block->programs[block->program_count];

    record->loaded = true;
    record->name = name;
    record->offset = offset;
    record->len = program->length;
    record->ref_count = 1;
    record->program = program;

    block->program_count++;
    block->cmd_used += (uint8_t)program->length;

    return (int)offset;
}

bool pio_manager_release_program(pio_manager_t *manager,uint8_t pio_index,const char *name){
    if (manager == NULL ||name == NULL ||pio_index >= PIO_BLOCK_COUNT) 
        return false;
    int index =pio_manager_find_program(manager, pio_index, name);
    if (index < 0) 
        return false;
    pio_manager_block_t *block = &manager->blocks[pio_index];
    pio_manager_program_t *record =&block->programs[index];
    if (record->ref_count > 1) {
        record->ref_count--;
        return true;
    }

    if (record->program != NULL) {
        pio_remove_program(block->hardware,record->program, record->offset);
    }

    if (block->cmd_used >= record->len) {
        block->cmd_used -= (uint8_t)record->len;
    } else {
        block->cmd_used = 0;
    }
    for (uint8_t i = (uint8_t)index;i + 1 < block->program_count; ++i) {
        block->programs[i] = block->programs[i + 1];
    }
    block->program_count--;
    pio_manager_program_t *last =&block->programs[block->program_count];

    last->loaded = false;
    last->name = NULL;
    last->offset = 0;
    last->len = 0;
    last->ref_count = 0;
    last->program = NULL;
    return true;

}

uint8_t pio_manager_get_used_sm_count(const pio_manager_t *manager,uint8_t pio_index)
{
    if(manager==NULL||pio_index >= PIO_BLOCK_COUNT) return 0;

    uint8_t used_count=0;
    for(uint8_t sm=0;sm<PIO_SM_COUNT;++sm){
        if(manager->blocks[pio_index].sm[sm].used)
        ++used_count;
    }
    return used_count;
}

uint8_t pio_manager_get_used_cmd_count(const pio_manager_t *manager,uint8_t pio_index)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT) return 0;

    return manager->blocks[pio_index].cmd_used;
}

bool pio_manager_is_sm_used(const pio_manager_t *manager,uint8_t pio_index,uint8_t sm)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT||sm>=PIO_SM_COUNT) return false;
     return manager->blocks[pio_index].sm[sm].used;
}

const char *pio_manager_get_sm_owner(const pio_manager_t *manager,uint8_t pio_index,uint8_t sm)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT||sm>=PIO_SM_COUNT) return NULL;
    const pio_manager_sm_t *state=&manager->blocks[pio_index].sm[sm];
    if(!state->used) return NULL;
    return state->owner;
}

uint8_t pio_manager_get_program_count(const pio_manager_t *manager,uint8_t pio_index)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT) return 0;
    return manager->blocks[pio_index].program_count;
}
const pio_manager_program_t *pio_manager_get_program_info(const pio_manager_t *manager,uint8_t pio_index,uint8_t program_index)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT) return NULL;
    const pio_manager_block_t *block = &manager->blocks[pio_index];
    if(program_index>=block->program_count)
    return NULL;
    const pio_manager_program_t *program = &block->programs[program_index];
    if(!program->loaded) return NULL;

    return program;
}

bool pio_manager_get_stats(const pio_manager_t *manager,uint8_t pio_index,pio_manager_stats_t *stats)
{
    if(manager==NULL||stats==NULL||pio_index>= PIO_BLOCK_COUNT) return false;
    stats->used_sm_count=pio_manager_get_used_sm_count(manager,pio_index);
    stats->total_sm_count=PIO_SM_COUNT;

    stats->used_cmd_count=pio_manager_get_used_cmd_count(manager,pio_index);
    stats->total_cmd_count=PIO_CMD_COUNT;

    stats->program_count=pio_manager_get_program_count(manager,pio_index);
    stats->total_program_count=PIO_PROGRAM_MAX_COUNT;

    return true;
}

bool pio_manager_can_load_program(const pio_manager_t *manager,uint8_t pio_index,const struct pio_program *program)
{
    if(manager==NULL||program==NULL||pio_index>=PIO_BLOCK_COUNT) return false;
    const pio_manager_block_t *block = &manager->blocks[pio_index];
    if(block->program_count>= PIO_PROGRAM_MAX_COUNT) return false;

    return pio_can_add_program(block->hardware,program);
}

bool pio_manager_can_use_program(const pio_manager_t *manager,uint8_t pio_index,const char * name,const struct pio_program *program)
{
    if(manager==NULL||name==NULL||program==NULL||pio_index>=PIO_BLOCK_COUNT) return false;
    int existing =pio_manager_find_program(manager,pio_index,name);
    if(existing>=0){
        const pio_manager_program_t *loded = &manager->blocks[pio_index].programs[existing];

        return loded->program == program;
    }
    return pio_manager_can_load_program(manager,pio_index,program);
}

uint8_t pio_manager_get_free_sm_count(const pio_manager_t *manager,uint8_t pio_index)
{
    if(manager==NULL||pio_index >=PIO_BLOCK_COUNT) return 0;
    uint8_t used = pio_manager_get_used_sm_count(manager,pio_index);
    return (uint8_t)(PIO_SM_COUNT-used);
}
void pio_manager_system_init(void)
{
    if (s_default_manager_initialized) {
        return;
    }

    pio_manager_init(&s_default_manager);
    s_default_manager_initialized = true;
}
pio_manager_t *pio_manager_get_default(void)
{
        pio_manager_system_init();

    return &s_default_manager;
}