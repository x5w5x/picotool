/*
 * @Author: 轩
 * @Date: 2026-10-06 14:09:35
 * @LastEditTime: 2026-10-07 14:46:38
 * @FilePath: \pico\fun\fun_script_engine.h
 */

#ifndef FUN_SCRIPT_ENGINE_H
#define FUN_SCRIPT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SCRIPT_MAX_ARGS      8
#define SCRIPT_LINE_MAX      128
#define SCRIPT_MAX_LABELS    16
#define SCRIPT_MAX_DYN_PRIMS 16
#define SCRIPT_MAX_LOOPS     8

typedef enum {
    LOOP_TYPE_REPEAT,
    LOOP_TYPE_WHILE,
    LOOP_TYPE_LOOP
} script_loop_type_t;

typedef struct {
    char     name[16];
    uint32_t char_offset;
} script_label_t;

typedef struct {
    uint32_t           start_pc;   // 循环语句起点 (while语句行首 / repeat循环体首行)
    script_loop_type_t type;       // 循环类型
    int32_t            remaining;  // repeat 剩余计数
} script_loop_frame_t;

typedef struct script_vm script_vm_t;

typedef int32_t (*prim_func_t)(script_vm_t *vm, int argc, const char *argv[]);

typedef struct {
    const char   *name;
    prim_func_t   handler;
    uint8_t       min_args;
    const char   *help;
} script_prim_entry_t;

#define SCRIPT_PRIMITIVE(name_str, func_ptr, min_arg_cnt, help_str) \
    static const script_prim_entry_t __prim_##func_ptr \
    __attribute__((used, retain, section("script_prims"), aligned(4))) = { \
        .name     = name_str, \
        .handler  = func_ptr, \
        .min_args = min_arg_cnt, \
        .help     = help_str \
    }

struct script_vm {
    const char          *src;
    uint32_t             pc;
    int32_t              regs[8];
    int32_t              last_ret;
    bool                 is_running;
    volatile bool        abort_req;
    void               (*log_cb)(const char *msg, uint16_t color, void *user_data);
    void                *user_data;
    
    script_label_t       labels[SCRIPT_MAX_LABELS];
    uint8_t              label_count;

    // 结构化多模式循环栈 (支持 repeat / while / loop 及 break / continue)
    script_loop_frame_t  loop_stack[SCRIPT_MAX_LOOPS];
    uint8_t              loop_sp;
};

void     script_vm_init(script_vm_t *vm);
void     script_vm_set_log(script_vm_t *vm, void (*log_cb)(const char*, uint16_t, void*), void *user_data);
void     script_vm_load(script_vm_t *vm, const char *script_text);
void     script_vm_run(script_vm_t *vm);
void     script_vm_abort(script_vm_t *vm);

bool     script_register_dyn_primitive(const char *name, prim_func_t handler, 
                                       uint8_t min_args, const char *help, void *owner_token);
void     script_unregister_dyn_primitives(void *owner_token);

int32_t  script_vm_exec_line(script_vm_t *vm, char *line, char *out_msg, size_t out_len);
void     script_vm_print_help(script_vm_t *vm);
uint8_t                    script_prim_get_count(void);
const script_prim_entry_t *script_prim_get_by_index(uint8_t index);
#endif // FUN_SCRIPT_ENGINE_H