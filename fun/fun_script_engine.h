/*
 * @Author: 轩
 * @Date: 2026-10-06 14:09:35
 * @LastEditTime: 2026-10-06 14:09:47
 * @FilePath: \pico\fun\fun_script_engine.h
 */
#ifndef FUN_SCRIPT_ENGINE_H
#define FUN_SCRIPT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SCRIPT_MAX_LABELS     32
#define SCRIPT_MAX_ARGS       8
#define SCRIPT_LINE_MAX       128

typedef struct script_vm script_vm_t;

// 原语函数签名：返回整型结果自动写入 $? 寄存器
typedef int32_t (*script_primitive_fn)(script_vm_t *vm, int argc, const char *argv[]);

typedef struct {
    const char          *name;
    script_primitive_fn  handler;
} script_primitive_t;

typedef struct {
    char     name[16];
    uint32_t char_offset; // 标签在脚本内存中的绝对字符偏移
} script_label_t;

struct script_vm {
    const char         *src;           // 脚本内存指针
    uint32_t            pc;            // 当前字符游标
    int32_t             regs[8];       // $0 ~ $7 通用寄存器
    int32_t             last_ret;      // $? 隐式返回寄存器
    
    script_label_t      labels[SCRIPT_MAX_LABELS];
    uint8_t             label_count;
    
    volatile bool       abort_req;     // Core0 发来的中止标志
    volatile bool       is_running;    // 当前运行状态
    
    void (*log_cb)(const char *text, uint16_t color, void *user_data); // 终端日志输出
    void               *user_data;
};

void script_vm_init(script_vm_t *vm);
void script_vm_set_log(script_vm_t *vm, void (*log_cb)(const char*, uint16_t, void*), void *user_data);
void script_vm_load(script_vm_t *vm, const char *script_text);
void script_vm_run(script_vm_t *vm);
void script_vm_abort(script_vm_t *vm);

#endif // FUN_SCRIPT_ENGINE_H