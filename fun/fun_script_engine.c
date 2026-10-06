#include "fun_script_engine.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================= 1. 基础硬件与控制原语 ================= */

static int32_t prim_delay(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 2) return -1;
    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 0);
    while (ms > 0 && !vm->abort_req) {
        uint32_t step = (ms > 10) ? 10 : ms;
        sleep_ms(step);
        ms -= step;
    }
    return 0;
}

// ⚡ 修复 1：输出时动态解析 $0~$7 与 $? 变量值
static int32_t prim_print(script_vm_t *vm, int argc, const char *argv[]) {
    if (argc < 2 || !vm->log_cb) return 0;
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

static int32_t prim_pin_mode(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    bool is_out = (strcmp(argv[2], "out") == 0 || strcmp(argv[2], "OUT") == 0);
    gpio_init(pin);
    gpio_set_dir(pin, is_out ? GPIO_OUT : GPIO_IN);
    return 0;
}

static int32_t prim_pin_write(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 3) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    uint val = (uint)strtoul(argv[2], NULL, 0);
    gpio_put(pin, val != 0);
    return 0;
}

static int32_t prim_pin_read(script_vm_t *vm, int argc, const char *argv[]) {
    (void)vm;
    if (argc < 2) return -1;
    uint pin = (uint)strtoul(argv[1], NULL, 0);
    return gpio_get(pin) ? 1 : 0;
}

static const script_primitive_t k_primitives[] = {
    { "delay",      prim_delay },
    { "print",      prim_print },
    { "pin.mode",   prim_pin_mode },
    { "pin.write",  prim_pin_write },
    { "pin.read",   prim_pin_read },
};
#define PRIMITIVE_COUNT (sizeof(k_primitives) / sizeof(k_primitives[0]))

/* ================= 2. 词法切分与数值解析 ================= */

static int32_t resolve_val(script_vm_t *vm, const char *tok) {
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
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '#') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* ================= 3. 标签索引与跳转 ================= */

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

/* ================= 4. 解释器运行主入口 ================= */

void script_vm_init(script_vm_t *vm) {
    memset(vm, 0, sizeof(*vm));
}

void script_vm_set_log(script_vm_t *vm, void (*log_cb)(const char*, uint16_t, void*), void *user_data) {
    vm->log_cb = log_cb;
    vm->user_data = user_data;
}

void script_vm_load(script_vm_t *vm, const char *script_text) {
    vm->src = script_text;
    vm->pc = 0;
    vm->abort_req = false;
    memset(vm->regs, 0, sizeof(vm->regs));
    vm->last_ret = 0;
    if (script_text != NULL) {
        prescan_labels(vm);
    }
}

void script_vm_abort(script_vm_t *vm) {
    vm->abort_req = true;
}

void script_vm_run(script_vm_t *vm) {
    if (!vm || !vm->src) return;
    vm->is_running = true;
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
        if (argc == 0 || argv[0][0] == ':') continue;

        // ⚡ 修复 2：统一收敛赋值分支，优先匹配 argc == 5 算术表达式
        if (argv[0][0] == '$' && argc >= 3 && strcmp(argv[1], "=") == 0) {
            uint8_t r_idx = argv[0][1] - '0';
            if (r_idx < 8) {
                if (argc == 5) {
                    // 运算赋值: $0 = $0 - 1 或 $0 = $1 + 2
                    int32_t a = resolve_val(vm, argv[2]);
                    int32_t b = resolve_val(vm, argv[4]);
                    if (strcmp(argv[3], "+") == 0) vm->regs[r_idx] = a + b;
                    else if (strcmp(argv[3], "-") == 0) vm->regs[r_idx] = a - b;
                } else if (argc == 3) {
                    // 常数或寄存器直接赋值: $0 = 10 或 $0 = $1
                    vm->regs[r_idx] = resolve_val(vm, argv[2]);
                } else {
                    // 原语返回值赋值: $0 = pin.read 4
                    for (size_t p = 0; p < PRIMITIVE_COUNT; p++) {
                        if (strcmp(k_primitives[p].name, argv[2]) == 0) {
                            vm->regs[r_idx] = k_primitives[p].handler(vm, argc - 2, (const char**)&argv[2]);
                            break;
                        }
                    }
                }
            }
            continue;
        }

        // 无条件跳转: goto LABEL
        if (strcmp(argv[0], "goto") == 0 && argc >= 2) {
            jump_to_label(vm, argv[1]);
            continue;
        }

        // 条件跳转: if $0 > 0 goto LABEL
        if (strcmp(argv[0], "if") == 0 && argc >= 6 && strcmp(argv[4], "goto") == 0) {
            int32_t v1 = resolve_val(vm, argv[1]);
            const char *op = argv[2];
            int32_t v2 = resolve_val(vm, argv[3]);
            bool cond = false;

            if (strcmp(op, "==") == 0)      cond = (v1 == v2);
            else if (strcmp(op, "!=") == 0) cond = (v1 != v2);
            else if (strcmp(op, ">") == 0)  cond = (v1 > v2);
            else if (strcmp(op, "<") == 0)  cond = (v1 < v2);
            else if (strcmp(op, ">=") == 0) cond = (v1 >= v2);
            else if (strcmp(op, "<=") == 0) cond = (v1 <= v2);

            if (cond) {
                jump_to_label(vm, argv[5]);
            }
            continue;
        }

        // 普通原语调用
        bool matched = false;
        for (size_t p = 0; p < PRIMITIVE_COUNT; p++) {
            if (strcmp(k_primitives[p].name, argv[0]) == 0) {
                vm->last_ret = k_primitives[p].handler(vm, argc, (const char**)argv);
                matched = true;
                break;
            }
        }

        if (!matched && vm->log_cb) {
            char err[48];
            snprintf(err, sizeof(err), "[ERR] 未知原语: %s", argv[0]);
            vm->log_cb(err, 0xF800, vm->user_data);
        }
    }

    vm->is_running = false;
    if (vm->log_cb) {
        if (vm->abort_req) vm->log_cb("[SYS] 脚本已被人工中止", 0xFD20, vm->user_data);
        else vm->log_cb("[SYS] 脚本执行完毕", 0x07E0, vm->user_data);
    }
}