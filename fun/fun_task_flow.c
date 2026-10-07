/*
 * @Author: 轩
 * @Date: 2026-10-07 15:44:34
 * @LastEditTime: 2026-10-07 15:44:42
 * @FilePath: \pico\fun\fun_task_flow.c
 */
#include "fun_task_flow.h"
#include <stdio.h>
#include <string.h>

void flow_step_set_default_args(flow_card_step_t *st, const char *prim_name) {
    if (!st || !prim_name) return;
    strncpy(st->prim_name, prim_name, sizeof(st->prim_name) - 1);
    st->prim_name[sizeof(st->prim_name) - 1] = '\0';
    memset(st->args, 0, sizeof(st->args));

    // 根据原语预填初值
    if (strcmp(prim_name, "pin.write") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "1");
    } else if (strcmp(prim_name, "delay") == 0) {
        strcpy(st->args[0], "200");
    } else if (strcmp(prim_name, "pin.mode") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "out");
    } else if (strcmp(prim_name, "pwm.set") == 0) {
        strcpy(st->args[0], "25");
        strcpy(st->args[1], "50");
    } else if (strcmp(prim_name, "ir.send") == 0) {
        strcpy(st->args[0], "0:/ir/p.ir");
    } else if (strcmp(prim_name, "print") == 0) {
        strcpy(st->args[0], "\"触发\"");
    }
}

int flow_export_to_script(const flow_card_seq_t *seq, char *out_buf, size_t max_len) {
    if (!seq || !out_buf || max_len == 0) return 0;
    out_buf[0] = '\0';

    snprintf(out_buf, max_len, "# 动作向导生成脚本\n");

    const char *indent = "";
    if (seq->repeat_times > 1) {
        char rep_hdr[32];
        snprintf(rep_hdr, sizeof(rep_hdr), "repeat %u\n", seq->repeat_times);
        strncat(out_buf, rep_hdr, max_len - strlen(out_buf) - 1);
        indent = "    ";
    }

    char line[96];
    for (uint8_t i = 0; i < seq->step_count; i++) {
        const flow_card_step_t *st = &seq->steps[i];
        if (st->prim_name[0] == '\0') continue;

        if (st->args[1][0] != '\0') {
            snprintf(line, sizeof(line), "%s%s %s %s\n", indent, st->prim_name, st->args[0], st->args[1]);
        } else if (st->args[0][0] != '\0') {
            snprintf(line, sizeof(line), "%s%s %s\n", indent, st->prim_name, st->args[0]);
        } else {
            snprintf(line, sizeof(line), "%s%s\n", indent, st->prim_name);
        }
        strncat(out_buf, line, max_len - strlen(out_buf) - 1);
    }

    if (seq->repeat_times > 1) {
        strncat(out_buf, "end\n", max_len - strlen(out_buf) - 1);
    }
    strncat(out_buf, "print \"=== 向导流程执行完毕 ===\"\n", max_len - strlen(out_buf) - 1);
    return (int)strlen(out_buf);
}