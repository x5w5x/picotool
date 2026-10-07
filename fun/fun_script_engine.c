#include "fun_script_engine.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// 由 GNU ld 链接器根据 section("script_prims") 自动生成的边界符号
extern const script_prim_entry_t __start_script_prims[];
extern const script_prim_entry_t __stop_script_prims[];

/* ================= 1. 动态段原语查找器 ================= */

static const script_prim_entry_t *find_primitive(const char *name) {
    if (!name) return NULL;
    const script_prim_entry_t *p = __start_script_prims;
    while (p < __stop_script_prims) {
        if (p->name && strcmp(p->name, name) == 0) {
            return p;
        }
        p++;
    }
    return NULL;
}

void script_vm_print_help(script_vm_t *vm) {
    if (!vm || !vm->log_cb) return;
    vm->log_cb("=== 固件支持的原语指令 ===", 0x03DF, vm->user_data);
    const script_prim_entry_t *p = __start_script_prims;
    while (p < __stop_script_prims) {
        if (p->name) {
            char buf[64];
            snprintf(buf, sizeof(buf), " %-12s - %s", p->name, p->help ? p->help : "");
            vm->log_cb(buf, 0xDEFB, vm->user_data);
        }
        p++;
    }
}

/* ================= 2. 词法分词与变量解析 ================= */

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

/* ================= 4. 虚拟机运行与单行 REPL 交互 ================= */

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
    memset(vm->regs, 0, sizeof(vm->regs));
    vm->last_ret = 0;
    if (script_text != NULL) {
        prescan_labels(vm);
    }
}

void script_vm_abort(script_vm_t *vm) {
    if (vm) vm->abort_req = true;
}

// ⚡ 单行执行核心 (供多行解释器和终端 REPL 共同调用)
int32_t script_vm_exec_line(script_vm_t *vm, char *line, char *out_msg, size_t out_len) {
    static script_vm_t s_default_repl_vm;
    if (!vm) {
        vm = &s_default_repl_vm;
    }

    char *argv[SCRIPT_MAX_ARGS];
    int argc = tokenize_line(line, argv, SCRIPT_MAX_ARGS);
    if (argc == 0 || argv[0][0] == ':') return 0;

    // 1. 变量赋值与运算分支: $0 = ...
    if (argv[0][0] == '$' && argc >= 3 && strcmp(argv[1], "=") == 0) {
        uint8_t r_idx = argv[0][1] - '0';
        if (r_idx < 8) {
            if (argc == 5) {
                // $0 = $0 - 1
                int32_t a = resolve_val(vm, argv[2]);
                int32_t b = resolve_val(vm, argv[4]);
                if (strcmp(argv[3], "+") == 0) vm->regs[r_idx] = a + b;
                else if (strcmp(argv[3], "-") == 0) vm->regs[r_idx] = a - b;
                else if (strcmp(argv[3], "*") == 0) vm->regs[r_idx] = a * b;
                else if (strcmp(argv[3], "/") == 0 && b != 0) vm->regs[r_idx] = a / b;
            } else if (argc == 3) {
                // $0 = 10 或 $0 = $1
                vm->regs[r_idx] = resolve_val(vm, argv[2]);
            } else {
                // $0 = pin.read 4 (原语返回值赋值)
                const script_prim_entry_t *prim = find_primitive(argv[2]);
                if (prim) {
                    vm->regs[r_idx] = prim->handler(vm, argc - 2, (const char**)&argv[2]);
                }
            }
            if (out_msg && out_len > 0) {
                snprintf(out_msg, out_len, "$%u = %ld", r_idx, (long)vm->regs[r_idx]);
            }
            return 0;
        }
    }

    // 2. 无条件跳转: goto LABEL
    if (strcmp(argv[0], "goto") == 0 && argc >= 2) {
        jump_to_label(vm, argv[1]);
        return 0;
    }

    // 3. 条件跳转: if $0 > 0 goto LABEL
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
        return 0;
    }

    // 4. 动态段原语查找执行
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

        char err_msg[64] = {0};
        int32_t ret = script_vm_exec_line(vm, line, err_msg, sizeof(err_msg));
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

/* ================= 5. 原生基础硬件与控制原语 (通过宏就地注册) ================= */

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
    uint val = (uint)strtoul(argv[2], NULL, 0);
    gpio_put(pin, val != 0);
    return 0;
}
SCRIPT_PRIMITIVE("pin.write", prim_pin_write, 2, "pin.write <引脚> <0/1>");

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