/*
 * @Author: 轩
 * @Date: 2026-10-07 15:44:08
 * @LastEditTime: 2026-10-07 15:44:15
 * @FilePath: \pico\fun\fun_task_flow.h
 */
#ifndef FUN_TASK_FLOW_H
#define FUN_TASK_FLOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FLOW_MAX_STEPS   16
#define FLOW_MAX_ARGS    2

typedef struct {
    char prim_name[16];           // 绑定的原语名 (如 "pin.write")
    char args[FLOW_MAX_ARGS][16]; // 参数槽位 (如 args[0]="25", args[1]="1")
} flow_card_step_t;

typedef struct {
    flow_card_step_t steps[FLOW_MAX_STEPS];
    uint8_t          step_count;
    uint8_t          repeat_times; // 1 = 单次执行, >1 = repeat N ... end
} flow_card_seq_t;

// 动态将卡片数据序列化为 DSL 文本
int flow_export_to_script(const flow_card_seq_t *seq, char *out_buf, size_t max_len);

// 当原语切换时，重置并赋予一套合理的默认参数
void flow_step_set_default_args(flow_card_step_t *st, const char *prim_name);

#endif // FUN_TASK_FLOW_H