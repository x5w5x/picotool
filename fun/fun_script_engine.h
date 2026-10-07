/*
 * @Author: 轩
 * @Date: 2026-10-06 14:09:35
 * @LastEditTime: 2026-10-07 12:27:08
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

typedef struct {
    char     name[16];
    uint32_t char_offset;
} script_label_t;

typedef struct script_vm script_vm_t;

// ⚡ 统一原语函数指针签名 (保留 vm 上下文以访问寄存器、日志与取消标志)
typedef int32_t (*prim_func_t)(script_vm_t *vm, int argc, const char *argv[]);

// 原语元数据结构体
typedef struct {
    const char   *name;       // 指令名称，如 "delay", "ir.send"
    prim_func_t   handler;    // 处理函数
    uint8_t       min_args;   // 最少参数个数 (不含指令名本身)
    const char   *help;       // 帮助提示文本
} script_prim_entry_t;

// ⚡ GNU ld 自动段收集宏：任何 .c 文件可直接就地声明，链接期自动聚合成表
#define SCRIPT_PRIMITIVE(name_str, func_ptr, min_arg_cnt, help_str) \
    static const script_prim_entry_t __prim_##func_ptr \
    __attribute__((used, retain, section("script_prims"), aligned(4))) = { \
        .name     = name_str, \
        .handler  = func_ptr, \
        .min_args = min_arg_cnt, \
        .help     = help_str \
    }

struct script_vm {
    const char    *src;
    uint32_t       pc;
    int32_t        regs[8];             // $0 ~ $7 通用寄存器
    int32_t        last_ret;            // $? 上次执行结果
    bool           is_running;
    volatile bool  abort_req;           // 外部强制中止标志
    void         (*log_cb)(const char *msg, uint16_t color, void *user_data);
    void          *user_data;
    script_label_t labels[SCRIPT_MAX_LABELS];
    uint8_t        label_count;
};

// 虚拟机生命周期与运行控制
void     script_vm_init(script_vm_t *vm);
void     script_vm_set_log(script_vm_t *vm, void (*log_cb)(const char*, uint16_t, void*), void *user_data);
void     script_vm_load(script_vm_t *vm, const char *script_text);
void     script_vm_run(script_vm_t *vm);
void     script_vm_abort(script_vm_t *vm);

// ⚡ 交互式 REPL 接口与帮助列举
int32_t  script_vm_exec_line(script_vm_t *vm, char *line, char *out_msg, size_t out_len);
void     script_vm_print_help(script_vm_t *vm);

#endif // FUN_SCRIPT_ENGINE_H