#include "fun_script_engine.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "fun_esp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "fun_i2c.h"
extern const script_prim_entry_t __start_script_prims[];
extern const script_prim_entry_t __stop_script_prims[];

typedef struct {
    script_prim_entry_t entry;
    void               *owner_token;
    bool                is_used;
} script_dyn_slot_t;

static script_dyn_slot_t s_dyn_slots[SCRIPT_MAX_DYN_PRIMS];

/* ================= 1. 动态与静态原语调度 ================= */

bool script_register_dyn_primitive(const char *name, prim_func_t handler, 
                                   uint8_t min_args, const char *help, void *owner_token) {
    if (!name || !handler || !owner_token) return false;

    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used && strcmp(s_dyn_slots[i].entry.name, name) == 0) {
            s_dyn_slots[i].entry.handler = handler;
            s_dyn_slots[i].entry.min_args = min_args;
            s_dyn_slots[i].entry.help = help;
            s_dyn_slots[i].owner_token = owner_token;
            return true;
        }
    }

    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (!s_dyn_slots[i].is_used) {
            s_dyn_slots[i].entry.name = name;
            s_dyn_slots[i].entry.handler = handler;
            s_dyn_slots[i].entry.min_args = min_args;
            s_dyn_slots[i].entry.help = help;
            s_dyn_slots[i].owner_token = owner_token;
            s_dyn_slots[i].is_used = true;
            return true;
        }
    }
    return false;
}

void script_unregister_dyn_primitives(void *owner_token) {
    if (!owner_token) return;
    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used && s_dyn_slots[i].owner_token == owner_token) {
            memset(&s_dyn_slots[i], 0, sizeof(script_dyn_slot_t));
        }
    }
}

static const script_prim_entry_t *find_primitive(const char *name) {
    if (!name) return NULL;

    const script_prim_entry_t *p = __start_script_prims;
    while (p < __stop_script_prims) {
        if (p->name && strcmp(p->name, name) == 0) return p;
        p++;
    }

    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used && strcmp(s_dyn_slots[i].entry.name, name) == 0) {
            return &s_dyn_slots[i].entry;
        }
    }
    return NULL;
}

uint8_t script_prim_get_count(void) {
    uint8_t static_cnt = (uint8_t)(__stop_script_prims - __start_script_prims);
    uint8_t dyn_cnt = 0;
    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used) dyn_cnt++;
    }
    return static_cnt + dyn_cnt;
}

const script_prim_entry_t *script_prim_get_by_index(uint8_t index) {
    uint8_t static_cnt = (uint8_t)(__stop_script_prims - __start_script_prims);
    if (index < static_cnt) {
        return &__start_script_prims[index];
    }
    uint8_t dyn_idx = index - static_cnt;
    uint8_t curr = 0;
    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used) {
            if (curr == dyn_idx) {
                return &s_dyn_slots[i].entry;
            }
            curr++;
        }
    }
    return NULL;
}

void script_vm_print_help(script_vm_t *vm) {
    if (!vm || !vm->log_cb) return;
    vm->log_cb("=== 原生固件指令 ===", 0x03DF, vm->user_data);
    const script_prim_entry_t *p = __start_script_prims;
    while (p < __stop_script_prims) {
        if (p->name) {
            char buf[64];
            snprintf(buf, sizeof(buf), " %-12s - %s", p->name, p->help ? p->help : "");
            vm->log_cb(buf, 0xDEFB, vm->user_data);
        }
        p++;
    }

    bool has_dyn = false;
    for (int i = 0; i < SCRIPT_MAX_DYN_PRIMS; i++) {
        if (s_dyn_slots[i].is_used) {
            if (!has_dyn) {
                vm->log_cb("=== PApp 扩展指令 ===", 0x07E0, vm->user_data);
                has_dyn = true;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), " %-12s - %s", s_dyn_slots[i].entry.name,
                     s_dyn_slots[i].entry.help ? s_dyn_slots[i].entry.help : "");
            vm->log_cb(buf, 0x5E7F, vm->user_data);
        }
    }
}

/* ================= 2. Pythonic 分词与命名符号解析 ================= */

static inline bool is_python_delim(char c) {
    return (c == ' ' || c == '\t' || c == ',' || c == '(' || c == ')' || c == ':');
}

static int32_t resolve_val(script_vm_t *vm, const char *tok) {
    if (!tok || !vm) return 0;
    if (tok[0] == '$') {
        if (tok[1] >= '0' && tok[1] <= '7') return vm->regs[tok[1] - '0'];
        if (tok[1] == '?') return vm->last_ret;
    }
    for (uint8_t i = 0; i < vm->var_count; i++) {
        if (strcmp(vm->vars[i].name, tok) == 0) {
            return vm->vars[i].val;
        }
    }
    return (int32_t)strtol(tok, NULL, 0);
}

static void set_var_val(script_vm_t *vm, const char *name, int32_t val) {
    if (!vm || !name) return;
    if (name[0] == '$' && name[1] >= '0' && name[1] <= '7') {
        vm->regs[name[1] - '0'] = val;
        return;
    }
    for (uint8_t i = 0; i < vm->var_count; i++) {
        if (strcmp(vm->vars[i].name, name) == 0) {
            vm->vars[i].val = val;
            return;
        }
    }
    if (vm->var_count < SCRIPT_MAX_VARS) {
        strncpy(vm->vars[vm->var_count].name, name, 11);
        vm->vars[vm->var_count].name[11] = '\0';
        vm->vars[vm->var_count].val = val;
        vm->var_count++;
    }
}

static int tokenize_line(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == ':') return 0;

    while (*p && argc < max_args) {
        while (*p && is_python_delim(*p)) p++;
        if (*p == '\0' || *p == '#') break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"' && *p != '\r' && *p != '\n') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !is_python_delim(*p) && *p != '#' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* ================= 3. 条件求值器 ================= */

static bool eval_condition(script_vm_t *vm, int argc, char *argv[]) {
    int op_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "==") == 0 || strcmp(argv[i], "!=") == 0 ||
            strcmp(argv[i], ">=") == 0 || strcmp(argv[i], "<=") == 0 ||
            strcmp(argv[i], ">")  == 0 || strcmp(argv[i], "<")  == 0) {
            op_idx = i;
            break;
        }
    }
    if (op_idx == -1 || op_idx + 1 >= argc) return false;

    int32_t v1 = 0;
    if (op_idx == 2) {
        v1 = resolve_val(vm, argv[1]);
    } else if (op_idx > 2) {
        const script_prim_entry_t *prim = find_primitive(argv[1]);
        if (prim) {
            v1 = prim->handler(vm, op_idx - 1, (const char**)&argv[1]);
        }
    }

    const char *op = argv[op_idx];
    int32_t v2 = resolve_val(vm, argv[op_idx + 1]);

    if (strcmp(op, "==") == 0) return (v1 == v2);
    if (strcmp(op, "!=") == 0) return (v1 != v2);
    if (strcmp(op, ">")  == 0) return (v1 > v2);
    if (strcmp(op, "<")  == 0) return (v1 < v2);
    if (strcmp(op, ">=") == 0) return (v1 >= v2);
    if (strcmp(op, "<=") == 0) return (v1 <= v2);
    return false;
}

/* ================= 4. 块语法快速跳跃扫描器 ================= */

static void skip_if_block(script_vm_t *vm, bool stop_at_branch) {
    int depth = 0;
    char line[SCRIPT_LINE_MAX];

    while (vm->src[vm->pc] != '\0' && !vm->abort_req) {
        uint32_t i = 0;
        while (vm->src[vm->pc] != '\0' && vm->src[vm->pc] != '\n' && i < SCRIPT_LINE_MAX - 1) {
            if (vm->src[vm->pc] != '\r') line[i++] = vm->src[vm->pc];
            vm->pc++;
        }
        line[i] = '\0';
        if (vm->src[vm->pc] == '\n') vm->pc++;

        char *argv[SCRIPT_MAX_ARGS];
        int argc = tokenize_line(line, argv, SCRIPT_MAX_ARGS);
        if (argc == 0) continue;

        if (strcmp(argv[0], "if") == 0) {
            depth++;
        } else if (strcmp(argv[0], "elif") == 0 || strcmp(argv[0], "else") == 0) {
            if (depth == 0 && stop_at_branch) return;
        } else if (strcmp(argv[0], "end") == 0 || strcmp(argv[0], "endif") == 0) {
            if (depth == 0) return;
            depth--;
        }
    }
}

static void skip_loop_block(script_vm_t *vm) {
    int depth = 0;
    char line[SCRIPT_LINE_MAX];

    while (vm->src[vm->pc] != '\0' && !vm->abort_req) {
        uint32_t i = 0;
        while (vm->src[vm->pc] != '\0' && vm->src[vm->pc] != '\n' && i < SCRIPT_LINE_MAX - 1) {
            if (vm->src[vm->pc] != '\r') line[i++] = vm->src[vm->pc];
            vm->pc++;
        }
        line[i] = '\0';
        if (vm->src[vm->pc] == '\n') vm->pc++;

        char *argv[2];
        int argc = tokenize_line(line, argv, 2);
        if (argc == 0) continue;

        if (strcmp(argv[0], "for") == 0 || strcmp(argv[0], "while") == 0 ||
            strcmp(argv[0], "repeat") == 0 || strcmp(argv[0], "loop") == 0) {
            depth++;
        } else if (strcmp(argv[0], "end") == 0) {
            if (depth == 0) return;
            depth--;
        }
    }
}

/* ================= 5. 经典标签预扫描与跳转 ================= */

static void prescan_labels(script_vm_t *vm) {
    vm->label_count = 0;
    const char *p = vm->src;
    uint32_t offset = 0;

    while (*p) {
        uint32_t line_start = offset;
        while (*p == ' ' || *p == '\t') { p++; offset++; }

        if (*p == ':') {
            p++; offset++;
            char name[16];
            int idx = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && idx < 15) {
                name[idx++] = *p++;
                offset++;
            }
            name[idx] = '\0';
            if (vm->label_count < SCRIPT_MAX_LABELS) {
                strncpy(vm->labels[vm->label_count].name, name, 15);
                vm->labels[vm->label_count].name[15] = '\0';
                vm->labels[vm->label_count].char_offset = line_start;
                vm->label_count++;
            }
        }
        while (*p && *p != '\n') { p++; offset++; }
        if (*p == '\n') { p++; offset++; }
    }
}

static void jump_to_label(script_vm_t *vm, const char *label) {
    for (uint8_t i = 0; i < vm->label_count; i++) {
        if (strcmp(vm->labels[i].name, label) == 0) {
            vm->pc = vm->labels[i].char_offset;
            return;
        }
    }
    if (vm->log_cb) {
        char err[48];
        snprintf(err, sizeof(err), "[ERR] 未找到标签 :%s", label);
        vm->log_cb(err, 0xF800, vm->user_data);
    }
    vm->abort_req = true;
}

/* ================= 6. 指令与表达式统一执行器 ================= */

static int32_t exec_tokens(script_vm_t *vm, int argc, char *argv[], char *out_msg, size_t out_len) {
    if (argc == 0) return 0;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        script_vm_print_help(vm);
        if (out_msg && out_len > 0) out_msg[0] = '\0';
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "=") == 0) {
        const char *var_name = argv[0];
        if (argc == 5) {
            int32_t a = resolve_val(vm, argv[2]);
            int32_t b = resolve_val(vm, argv[4]);
            int32_t res = 0;
            if (strcmp(argv[3], "+") == 0)      res = a + b;
            else if (strcmp(argv[3], "-") == 0) res = a - b;
            else if (strcmp(argv[3], "*") == 0) res = a * b;
            else if (strcmp(argv[3], "/") == 0 && b != 0) res = a / b;
            set_var_val(vm, var_name, res);
        } else if (argc == 3) {
            set_var_val(vm, var_name, resolve_val(vm, argv[2]));
        } else {
            const script_prim_entry_t *prim = find_primitive(argv[2]);
            if (prim) {
                int32_t res = prim->handler(vm, argc - 2, (const char**)&argv[2]);
                set_var_val(vm, var_name, res);
            } else {
                if (out_msg && out_len > 0) snprintf(out_msg, out_len, "[ERR] 未知原语: %s", argv[2]);
                return -1;
            }
        }
        if (out_msg && out_len > 0) {
            snprintf(out_msg, out_len, "%s = %ld", var_name, (long)resolve_val(vm, var_name));
        }
        return 0;
    }

    const script_prim_entry_t *prim = find_primitive(argv[0]);
    if (prim) {
        if ((argc - 1) < prim->min_args) {
            if (out_msg && out_len > 0) {
                snprintf(out_msg, out_len, "[ERR] 参数不足! 用法: %s", prim->help ? prim->help : "");
            }
            return -2;
        }
        vm->last_ret = prim->handler(vm, argc, (const char**)argv);
        if (out_msg && out_len > 0) {
            snprintf(out_msg, out_len, "[OK] 返回值: %ld", (long)vm->last_ret);
        }
        return vm->last_ret;
    }

    if (out_msg && out_len > 0) snprintf(out_msg, out_len, "[ERR] 未知指令: %s", argv[0]);
    return -1;
}

/* ================= 7. 虚拟机生命周期接口 ================= */

void script_vm_init(script_vm_t *vm) {
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
}

void script_vm_set_log(script_vm_t *vm, void (*log_cb)(const char*, uint16_t, void*), void *user_data) {
    if (!vm) return;
    vm->log_cb = log_cb;
    vm->user_data = user_data;
}

void script_vm_load(script_vm_t *vm, const char *script_text) {
    if (!vm) return;
    vm->src = script_text;
    vm->pc = 0;
    vm->abort_req = false;
    vm->loop_sp = 0;
    vm->var_count = 0;
    vm->if_matched = false;
    memset(vm->regs, 0, sizeof(vm->regs));
    memset(vm->vars, 0, sizeof(vm->vars));
    vm->last_ret = 0;
    if (script_text != NULL) {
        prescan_labels(vm);
    }
}

void script_vm_abort(script_vm_t *vm) {
    if (vm) vm->abort_req = true;
}

int32_t script_vm_exec_line(script_vm_t *vm, char *line, char *out_msg, size_t out_len) {
    static script_vm_t s_default_repl_vm;
    if (!vm) vm = &s_default_repl_vm;

    char *argv[SCRIPT_MAX_ARGS];
    int argc = tokenize_line(line, argv, SCRIPT_MAX_ARGS);
    return exec_tokens(vm, argc, argv, out_msg, out_len);
}

void script_vm_run(script_vm_t *vm) {
    if (!vm || !vm->src) return;
    vm->is_running = true;
    vm->loop_sp = 0;
    vm->if_matched = false;
    char line[SCRIPT_LINE_MAX];

    while (vm->src[vm->pc] != '\0' && !vm->abort_req) {
        uint32_t line_start_pc = vm->pc;
        uint32_t i = 0;
        while (vm->src[vm->pc] != '\0' && vm->src[vm->pc] != '\n' && i < SCRIPT_LINE_MAX - 1) {
            if (vm->src[vm->pc] != '\r') line[i++] = vm->src[vm->pc];
            vm->pc++;
        }
        line[i] = '\0';
        if (vm->src[vm->pc] == '\n') vm->pc++;

        char *argv[SCRIPT_MAX_ARGS];
        int argc = tokenize_line(line, argv, SCRIPT_MAX_ARGS);
        if (argc == 0) continue;

        // 1. 结构化循环控制
        if (strcmp(argv[0], "for") == 0 && argc >= 4) {
            const char *var_name = argv[1];
            int32_t start_val = 0;
            int32_t stop_val = 0;
            int32_t step_val = 1;

            if (strcmp(argv[2], "in") == 0 && strcmp(argv[3], "range") == 0) {
                if (argc == 5) {
                    start_val = 0;
                    stop_val = resolve_val(vm, argv[4]);
                } else if (argc >= 6) {
                    start_val = resolve_val(vm, argv[4]);
                    stop_val = resolve_val(vm, argv[5]);
                    if (argc >= 7) step_val = resolve_val(vm, argv[6]);
                }
            } else if (strcmp(argv[2], "in") == 0) {
                start_val = 0;
                stop_val = resolve_val(vm, argv[3]);
            }

            if ((step_val > 0 && start_val >= stop_val) || (step_val < 0 && start_val <= stop_val)) {
                skip_loop_block(vm);
            } else {
                set_var_val(vm, var_name, start_val);
                if (vm->loop_sp < SCRIPT_MAX_LOOPS) {
                    script_loop_frame_t *f = &vm->loop_stack[vm->loop_sp];
                    f->start_pc = vm->pc;
                    f->type = LOOP_TYPE_FOR;
                    f->end_val = stop_val;
                    f->step = step_val;
                    strncpy(f->var_name, var_name, 11);
                    f->var_name[11] = '\0';
                    vm->loop_sp++;
                }
            }
            continue;
        }

        if (strcmp(argv[0], "repeat") == 0 && argc >= 2) {
            int32_t count = resolve_val(vm, argv[1]);
            if (count <= 0) {
                skip_loop_block(vm);
            } else {
                if (vm->loop_sp < SCRIPT_MAX_LOOPS) {
                    vm->loop_stack[vm->loop_sp].start_pc = vm->pc;
                    vm->loop_stack[vm->loop_sp].type = LOOP_TYPE_REPEAT;
                    vm->loop_stack[vm->loop_sp].remaining = count;
                    vm->loop_sp++;
                }
            }
            continue;
        }

        if (strcmp(argv[0], "while") == 0) {
            bool cond = eval_condition(vm, argc, argv);
            if (vm->loop_sp > 0 && vm->loop_stack[vm->loop_sp - 1].type == LOOP_TYPE_WHILE &&
                vm->loop_stack[vm->loop_sp - 1].start_pc == line_start_pc) {
                if (!cond) {
                    vm->loop_sp--;
                    skip_loop_block(vm);
                }
            } else {
                if (cond) {
                    if (vm->loop_sp < SCRIPT_MAX_LOOPS) {
                        vm->loop_stack[vm->loop_sp].start_pc = line_start_pc;
                        vm->loop_stack[vm->loop_sp].type = LOOP_TYPE_WHILE;
                        vm->loop_sp++;
                    }
                } else {
                    skip_loop_block(vm);
                }
            }
            continue;
        }

        if (strcmp(argv[0], "loop") == 0) {
            if (vm->loop_sp < SCRIPT_MAX_LOOPS) {
                vm->loop_stack[vm->loop_sp].start_pc = vm->pc;
                vm->loop_stack[vm->loop_sp].type = LOOP_TYPE_LOOP;
                vm->loop_sp++;
            }
            continue;
        }

        if (strcmp(argv[0], "end") == 0) {
            if (vm->loop_sp > 0) {
                script_loop_frame_t *frame = &vm->loop_stack[vm->loop_sp - 1];
                if (frame->type == LOOP_TYPE_FOR) {
                    int32_t cur = resolve_val(vm, frame->var_name);
                    cur += frame->step;
                    set_var_val(vm, frame->var_name, cur);

                    if ((frame->step > 0 && cur < frame->end_val) || 
                        (frame->step < 0 && cur > frame->end_val)) {
                        vm->pc = frame->start_pc;
                    } else {
                        vm->loop_sp--;
                    }
                    continue;
                } else if (frame->type == LOOP_TYPE_REPEAT) {
                    frame->remaining--;
                    if (frame->remaining > 0) vm->pc = frame->start_pc;
                    else vm->loop_sp--;
                    continue;
                } else if (frame->type == LOOP_TYPE_WHILE) {
                    vm->pc = frame->start_pc;
                    continue;
                } else if (frame->type == LOOP_TYPE_LOOP) {
                    vm->pc = frame->start_pc;
                    continue;
                }
            }
            vm->if_matched = false;
            continue;
        }

        if (strcmp(argv[0], "break") == 0) {
            if (vm->loop_sp > 0) {
                vm->loop_sp--;
                skip_loop_block(vm);
            }
            continue;
        }

        if (strcmp(argv[0], "continue") == 0) {
            if (vm->loop_sp > 0) {
                script_loop_frame_t *frame = &vm->loop_stack[vm->loop_sp - 1];
                vm->if_matched = false;

                if (frame->type == LOOP_TYPE_FOR) {
                    int32_t cur = resolve_val(vm, frame->var_name);
                    cur += frame->step;
                    set_var_val(vm, frame->var_name, cur);

                    if ((frame->step > 0 && cur < frame->end_val) || 
                        (frame->step < 0 && cur > frame->end_val)) {
                        vm->pc = frame->start_pc;
                    } else {
                        vm->loop_sp--;
                        skip_loop_block(vm);
                    }
                } else if (frame->type == LOOP_TYPE_REPEAT) {
                    frame->remaining--;
                    if (frame->remaining > 0) {
                        vm->pc = frame->start_pc;
                    } else {
                        vm->loop_sp--;
                        skip_loop_block(vm);
                    }
                } else if (frame->type == LOOP_TYPE_WHILE || frame->type == LOOP_TYPE_LOOP) {
                    vm->pc = frame->start_pc;
                }
            }
            continue;
        }

        // 2. 条件分支控制
        if (strcmp(argv[0], "if") == 0) {
            bool is_classic_goto = false;
            for (int k = 1; k < argc; k++) {
                if (strcmp(argv[k], "goto") == 0 && k + 1 < argc) {
                    if (eval_condition(vm, k, argv)) {
                        jump_to_label(vm, argv[k + 1]);
                    }
                    is_classic_goto = true;
                    break;
                }
            }
            if (is_classic_goto) continue;

            if (eval_condition(vm, argc, argv)) {
                vm->if_matched = true;
            } else {
                vm->if_matched = false;
                skip_if_block(vm, true);
            }
            continue;
        }

        if (strcmp(argv[0], "elif") == 0) {
            if (vm->if_matched) {
                skip_if_block(vm, false);
            } else {
                if (eval_condition(vm, argc, argv)) {
                    vm->if_matched = true;
                } else {
                    skip_if_block(vm, true);
                }
            }
            continue;
        }

        if (strcmp(argv[0], "else") == 0) {
            if (vm->if_matched) {
                skip_if_block(vm, false);
            }
            continue;
        }

        if (strcmp(argv[0], "endif") == 0) {
            vm->if_matched = false;
            continue;
        }

        if (strcmp(argv[0], "goto") == 0 && argc >= 2) {
            jump_to_label(vm, argv[1]);
            continue;
        }

        // 3. 原语与通用赋值派发
        char err_msg[64] = {0};
        int32_t ret = exec_tokens(vm, argc, argv, err_msg, sizeof(err_msg));
        if (ret == -1 && vm->log_cb && err_msg[0]) {
            vm->log_cb(err_msg, 0xF800, vm->user_data);
        }
    }

    vm->is_running = false;
    if (vm->log_cb) {
        if (vm->abort_req) vm->log_cb("[SYS] 脚本已被人工中止", 0xFD20, vm->user_data);
        else vm->log_cb("[SYS] 脚本执行完毕", 0x07E0, vm->user_data);
    }
}

/* ================= 8. 内置常驻原语注册 ================= */

static int32_t prim_delay(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 2) return -1;
    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 0);
    while (ms > 0 && (!vm || !vm->abort_req)) {
        uint32_t step = (ms > 10) ? 10 : ms;
        sleep_ms(step);
        ms -= step;
    }
    return 0;
}
SCRIPT_PRIMITIVE("delay", prim_delay, 1, "delay(毫秒)");

static int32_t prim_print(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 2 || !vm || !vm->log_cb) return 0;
    char buf[128] = {0};
    for (int i = 1; i < argc; i++) {
        const char *val_str = argv[i];
        char num_buf[16];

        for (uint8_t v = 0; v < vm->var_count; v++) {
            if (strcmp(vm->vars[v].name, val_str) == 0) {
                snprintf(num_buf, sizeof(num_buf), "%ld", (long)vm->vars[v].val);
                val_str = num_buf;
                break;
            }
        }
        if (val_str[0] == '$' && val_str[1] >= '0' && val_str[1] <= '7') {
            snprintf(num_buf, sizeof(num_buf), "%ld", (long)vm->regs[val_str[1] - '0']);
            val_str = num_buf;
        }

        strncat(buf, val_str, sizeof(buf) - strlen(buf) - 2);
        if (i < argc - 1) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
    }
    vm->log_cb(buf, 0xFFFF, vm->user_data);
    return 0;
}
SCRIPT_PRIMITIVE("print", prim_print, 1, "print(...)");

static int32_t prim_pin_mode(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    bool is_out = (strcmp(argv[2], "out") == 0 || strcmp(argv[2], "OUT") == 0);
    gpio_init(pin);
    gpio_set_dir(pin, is_out ? GPIO_OUT : GPIO_IN);
    return 0;
}
SCRIPT_PRIMITIVE("pin.mode", prim_pin_mode, 2, "pin.mode(引脚, in/out)");

static int32_t prim_pin_write(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    int32_t val = resolve_val(vm, argv[2]);
    gpio_put(pin, val != 0);
    return 0;
}
SCRIPT_PRIMITIVE("pin.write", prim_pin_write, 2, "pin.write(引脚, 电平)");

static int32_t prim_pin_read(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    return gpio_get(pin) ? 1 : 0;
}
SCRIPT_PRIMITIVE("pin.read", prim_pin_read, 1, "pin.read(引脚)");

static int32_t prim_help(script_vm_t *vm, int argc, const char *argv[]) {
    (void)argc; (void)argv;
    script_vm_print_help(vm);
    return 0;
}
SCRIPT_PRIMITIVE("help", prim_help, 0, "help()");

/* ================= 9. 扩展硬件物理测量与执行原语 ================= */

static int32_t prim_adc_volt(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return 0;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    if (pin < 26 || pin > 29) return 0;

    static bool s_adc_inited = false;
    if (!s_adc_inited) {
        adc_init();
        s_adc_inited = true;
    }
    adc_gpio_init(pin);
    adc_select_input(pin - 26);
    uint16_t raw = adc_read();
    return (int32_t)((raw * 3300u) / 4095u);
}
SCRIPT_PRIMITIVE("adc.volt", prim_adc_volt, 1, "adc.volt(引脚26-29) -> mV");

static int32_t prim_adc_read(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return 0;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    if (pin < 26 || pin > 29) return 0;

    static bool s_adc_inited = false;
    if (!s_adc_inited) {
        adc_init();
        s_adc_inited = true;
    }
    adc_gpio_init(pin);
    adc_select_input(pin - 26);
    return (int32_t)adc_read();
}
SCRIPT_PRIMITIVE("adc.read", prim_adc_read, 1, "adc.read(引脚26-29)");

static int32_t prim_pwm_set(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    int32_t duty = (int32_t)strtol(argv[2], NULL, 0);
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;

    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    uint chan  = pwm_gpio_to_channel(pin);

    pwm_set_wrap(slice, 1000);
    pwm_set_chan_level(slice, chan, (duty * 1000) / 100);
    pwm_set_enabled(slice, true);
    return 0;
}
SCRIPT_PRIMITIVE("pwm.set", prim_pwm_set, 2, "pwm.set(引脚, 占空比0-100)");

static int32_t prim_esp_send(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s\r\n", argv[1]);
    esp_send_string(cmd);
    return 1;
}
SCRIPT_PRIMITIVE("esp.send", prim_esp_send, 1, "esp.send(AT指令/文本)");


/* ================= 10. I2C 总线通用原生脚本原语 ================= */

static int32_t prim_i2c_scan(script_vm_t *vm, int argc, const char *argv[]) {
    (void)argc; (void)argv;
    uint8_t addrs[16];
    int n = fun_i2c_scan(addrs, 16);
    if (vm && vm->log_cb) {
        char buf[64];
        if (n == 0) {
            vm->log_cb("[I2C] 扫描完成: 未发现任何设备", 0xFD20, vm->user_data);
        } else {
            snprintf(buf, sizeof(buf), "[I2C] 扫描完成: 发现 %d 个从机", n);
            vm->log_cb(buf, 0x03DF, vm->user_data);
            
            // 逐个打印扫出的十六进制地址和器件名称
            for (int i = 0; i < n && i < 16; i++) {
                snprintf(buf, sizeof(buf), " -> [0x%02X] %s", addrs[i], fun_i2c_lookup_name(addrs[i]));
                vm->log_cb(buf, 0x07E0, vm->user_data);
            }
        }
    }
    return n;
}
SCRIPT_PRIMITIVE("i2c.scan", prim_i2c_scan, 0, "i2c.scan() -> 发现器件数");

static int32_t prim_i2c_read(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 3) return -1;
    uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(argv[2], NULL, 0);
    uint8_t val  = 0;
    int ret = fun_i2c_read_reg(addr, reg, &val);
    return (ret == 0) ? (int32_t)val : -1;
}
SCRIPT_PRIMITIVE("i2c.read", prim_i2c_read, 2, "i2c.read(从机地址, 寄存器) -> 值");

static int32_t prim_i2c_write(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 4) return -1;
    uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(argv[2], NULL, 0);
    uint8_t val  = (uint8_t)strtoul(argv[3], NULL, 0);
    return (fun_i2c_write_reg(addr, reg, val) == 0) ? 0 : -1;
}
SCRIPT_PRIMITIVE("i2c.write", prim_i2c_write, 3, "i2c.write(从机地址, 寄存器, 值)");

static int32_t prim_i2c_send(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 3) return -1;
    uint8_t addr = (uint8_t)strtoul(argv[1], NULL, 0);
    const char *hex_str = argv[2];
    int ret = fun_i2c_send_hex(addr, hex_str);
    return ret >= 0 ? 0 : -1;
}
SCRIPT_PRIMITIVE("i2c.send", prim_i2c_send, 2, "i2c.send(从机地址, \"HEX字符串\")");

