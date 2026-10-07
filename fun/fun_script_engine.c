#include "fun_script_engine.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

/* ================= 2. 词法切分与数值解析 ================= */

static int32_t resolve_val(script_vm_t *vm, const char *tok) {
    if (!tok) return 0;
    if (tok[0] == '$') {
        if (tok[1] >= '0' && tok[1] <= '7') return vm->regs[tok[1] - '0'];
        if (tok[1] == '?') return vm->last_ret;
    }
    return (int32_t)strtol(tok, NULL, 0);
}

static int tokenize_line(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '\0' || *p == '#') break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"' && *p != '\r' && *p != '\n') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '#' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* ================= 3. 统一条件求值器 (支持内联原语) ================= */

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

static void skip_if_block(script_vm_t *vm, bool stop_at_else) {
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
            bool is_goto = false;
            for (int k = 1; k < argc; k++) {
                if (strcmp(argv[k], "goto") == 0) { is_goto = true; break; }
            }
            if (!is_goto) depth++;
        } else if (strcmp(argv[0], "else") == 0) {
            if (depth == 0 && stop_at_else) return;
        } else if (strcmp(argv[0], "endif") == 0) {
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

        if (strcmp(argv[0], "repeat") == 0 || strcmp(argv[0], "while") == 0 || strcmp(argv[0], "loop") == 0) {
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
    if (argc == 0 || argv[0][0] == ':') return 0;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        script_vm_print_help(vm);
        if (out_msg && out_len > 0) out_msg[0] = '\0';
        return 0;
    }

    // 变量赋值与运算: $0 = ...
    if (argv[0][0] == '$' && argc >= 3 && strcmp(argv[1], "=") == 0) {
        uint8_t r_idx = argv[0][1] - '0';
        if (r_idx < 8) {
            if (argc == 5) {
                int32_t a = resolve_val(vm, argv[2]);
                int32_t b = resolve_val(vm, argv[4]);
                if (strcmp(argv[3], "+") == 0)      vm->regs[r_idx] = a + b;
                else if (strcmp(argv[3], "-") == 0) vm->regs[r_idx] = a - b;
                else if (strcmp(argv[3], "*") == 0) vm->regs[r_idx] = a * b;
                else if (strcmp(argv[3], "/") == 0 && b != 0) vm->regs[r_idx] = a / b;
            } else if (argc == 3) {
                vm->regs[r_idx] = resolve_val(vm, argv[2]);
            } else {
                const script_prim_entry_t *prim = find_primitive(argv[2]);
                if (prim) {
                    vm->regs[r_idx] = prim->handler(vm, argc - 2, (const char**)&argv[2]);
                } else {
                    if (out_msg && out_len > 0) {
                        snprintf(out_msg, out_len, "[ERR] 未知原语: %s", argv[2]);
                    }
                    return -1;
                }
            }
            if (out_msg && out_len > 0) {
                snprintf(out_msg, out_len, "$%u = %ld", r_idx, (long)vm->regs[r_idx]);
            }
            return 0;
        }
    }

    // 原语查找与执行
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

    if (out_msg && out_len > 0) {
        snprintf(out_msg, out_len, "[ERR] 未知指令: %s", argv[0]);
    }
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
    memset(vm->regs, 0, sizeof(vm->regs));
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

// 脚本文件多行解释器主循环
void script_vm_run(script_vm_t *vm) {
    if (!vm || !vm->src) return;
    vm->is_running = true;
    vm->loop_sp = 0;
    char line[SCRIPT_LINE_MAX];

    while (vm->src[vm->pc] != '\0' && !vm->abort_req) {
        uint32_t line_start_pc = vm->pc; // 记录本行起始位置，供 while 循环重估条件回溯
        uint32_t i = 0;
        while (vm->src[vm->pc] != '\0' && vm->src[vm->pc] != '\n' && i < SCRIPT_LINE_MAX - 1) {
            if (vm->src[vm->pc] != '\r') line[i++] = vm->src[vm->pc];
            vm->pc++;
        }
        line[i] = '\0';
        if (vm->src[vm->pc] == '\n') vm->pc++;

        char *argv[SCRIPT_MAX_ARGS];
        int argc = tokenize_line(line, argv, SCRIPT_MAX_ARGS);
        if (argc == 0 || argv[0][0] == ':') continue;

        // ---------------- 1. 结构化循环控制: repeat / while / loop ----------------
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
            // 检查当前栈顶是否已是本 while 循环
            if (vm->loop_sp > 0 && vm->loop_stack[vm->loop_sp - 1].type == LOOP_TYPE_WHILE &&
                vm->loop_stack[vm->loop_sp - 1].start_pc == line_start_pc) {
                if (!cond) {
                    vm->loop_sp--;
                    skip_loop_block(vm);
                }
            } else {
                // 初次进入 while
                if (cond) {
                    if (vm->loop_sp < SCRIPT_MAX_LOOPS) {
                        vm->loop_stack[vm->loop_sp].start_pc = line_start_pc;
                        vm->loop_stack[vm->loop_sp].type = LOOP_TYPE_WHILE;
                        vm->loop_stack[vm->loop_sp].remaining = 0;
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
                vm->loop_stack[vm->loop_sp].remaining = 0;
                vm->loop_sp++;
            }
            continue;
        }

        if (strcmp(argv[0], "end") == 0) {
            if (vm->loop_sp > 0) {
                script_loop_frame_t *frame = &vm->loop_stack[vm->loop_sp - 1];
                if (frame->type == LOOP_TYPE_REPEAT) {
                    frame->remaining--;
                    if (frame->remaining > 0) {
                        vm->pc = frame->start_pc;
                    } else {
                        vm->loop_sp--;
                    }
                } else if (frame->type == LOOP_TYPE_WHILE) {
                    vm->pc = frame->start_pc; // 跳回 while 语句行重新评估条件
                } else if (frame->type == LOOP_TYPE_LOOP) {
                    vm->pc = frame->start_pc; // 跳回循环体第一行
                }
            }
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
                if (frame->type == LOOP_TYPE_REPEAT) {
                    frame->remaining--;
                    if (frame->remaining > 0) {
                        vm->pc = frame->start_pc;
                    } else {
                        vm->loop_sp--;
                        skip_loop_block(vm);
                    }
                } else if (frame->type == LOOP_TYPE_WHILE) {
                    vm->pc = frame->start_pc;
                } else if (frame->type == LOOP_TYPE_LOOP) {
                    vm->pc = frame->start_pc;
                }
            }
            continue;
        }

        // ---------------- 2. 条件分支控制: if ... else ... endif ----------------
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
                // 条件真：执行分支
            } else {
                // 条件假：跳过至 else 或 endif
                skip_if_block(vm, true);
            }
            continue;
        }

        if (strcmp(argv[0], "else") == 0) {
            skip_if_block(vm, false);
            continue;
        }

        if (strcmp(argv[0], "endif") == 0) {
            continue;
        }

        // ---------------- 3. 经典跳转兼容: goto ----------------
        if (strcmp(argv[0], "goto") == 0 && argc >= 2) {
            jump_to_label(vm, argv[1]);
            continue;
        }

        // ---------------- 4. 变量运算与原语直接派发 ----------------
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
SCRIPT_PRIMITIVE("delay", prim_delay, 1, "delay <毫秒>");

static int32_t prim_print(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 2 || !vm || !vm->log_cb) return 0;
    char buf[128] = {0};
    for (int i = 1; i < argc; i++) {
        const char *val_str = argv[i];
        char num_buf[16];

        if (argv[i][0] == '$') {
            if (argv[i][1] >= '0' && argv[i][1] <= '7') {
                snprintf(num_buf, sizeof(num_buf), "%ld", (long)vm->regs[argv[i][1] - '0']);
                val_str = num_buf;
            } else if (argv[i][1] == '?') {
                snprintf(num_buf, sizeof(num_buf), "%ld", (long)vm->last_ret);
                val_str = num_buf;
            }
        }

        strncat(buf, val_str, sizeof(buf) - strlen(buf) - 2);
        if (i < argc - 1) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
    }
    vm->log_cb(buf, 0xFFFF, vm->user_data);
    return 0;
}
SCRIPT_PRIMITIVE("print", prim_print, 1, "print <内容/$0>");

static int32_t prim_pin_mode(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    bool is_out = (strcmp(argv[2], "out") == 0 || strcmp(argv[2], "OUT") == 0);
    gpio_init(pin);
    gpio_set_dir(pin, is_out ? GPIO_OUT : GPIO_IN);
    return 0;
}
SCRIPT_PRIMITIVE("pin.mode", prim_pin_mode, 2, "pin.mode <引脚> <in/out>");

static int32_t prim_pin_write(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    int32_t val = resolve_val(vm, argv[2]);
    gpio_put(pin, val != 0);
    return 0;
}
SCRIPT_PRIMITIVE("pin.write", prim_pin_write, 2, "pin.write <引脚> <0/1/$0>");

static int32_t prim_pin_read(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    return gpio_get(pin) ? 1 : 0;
}
SCRIPT_PRIMITIVE("pin.read", prim_pin_read, 1, "pin.read <引脚>");

static int32_t prim_help(script_vm_t *vm, int argc, const char *argv[]) {
    (void)argc; (void)argv;
    script_vm_print_help(vm);
    return 0;
}
SCRIPT_PRIMITIVE("help", prim_help, 0, "打印支持的原语清单");