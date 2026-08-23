/*
 * @Author: 轩
 * @Date: 2026-07-23 12:22:14
 * @LastEditTime: 2026-07-24 17:09:37
 * @FilePath: \minirtos\OS_Core\shell.c
 */

#include "shell.h"
#include "SEGGER_RTT.h"
#include <string.h>
#include <stdio.h>
#include "stm32f10x.h"
extern void os_system_info(void);
extern void vfs_list_devices(void);
extern int vfs_open(const char *name);
extern int vfs_read(int fd, uint32_t pos, void *buffer, uint32_t size);
extern int vfs_write(int fd, uint32_t pos, const void *buffer, uint32_t size);
extern int vfs_ioctl(int fd, int cmd, void *args);
extern int vfs_close(int fd);
extern void vm_load_apps(uint32_t base_addr);
extern void vm_unload_apps(void);
#define APP_START_ADDR     (0x08000000 + 24 * 1024) 
#define APP_MAX_SIZE       (8 * 1024) 
#define APP_SLOT_SIZE (1*1024)
extern uint32_t g_slot_index;
#define SHELL_BUF_SIZE   64   // 行输入缓冲区最大字节数
#define SHELL_MAX_ARGS   8    // 单条命令最多支持的参数个数 (argc)

typedef void (*shell_func_t)(int argc, char *argv[]);
//Shell 命令结构体
typedef struct {
    const char *name;                       // 命令名称 (如 "ps")
    void (*func)(int argc, char *argv[]);   // 回调函数指针 (支持传参)
    const char *desc;                       // 命令描述 (用于 help)
} shell_cmd_t;

static char s_rx_buf[SHELL_BUF_SIZE];
static int  s_buf_idx = 0;

static void cmd_help(int argc, char *argv[]);

static void cmd_ps(int argc, char *argv[]) {
    (void)argc; (void)argv;
    os_system_info();
}

static void cmd_reboot(int argc, char *argv[]) {
    (void)argc; (void)argv;
    SEGGER_RTT_printf(0, "\r\n[SYS] System Rebooting...\r\n");
    NVIC_SystemReset();
}

static void cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    // RTT ANSI 转义清屏并复位光标到左上角
    SEGGER_RTT_WriteString(0, "\033[2J\033[1;1H");
}
static void cmd_lsdev(int argc, char *argv[]) {
    (void)argc; (void)argv;
    vfs_list_devices();
}
static void cmd_dev(int argc, char *argv[])
{
    if (argc < 3) {
        SEGGER_RTT_printf(0, "\r\nUsage:\r\n");
        SEGGER_RTT_printf(0, "  dev read  <dev_name>\r\n");
        SEGGER_RTT_printf(0, "  dev write <dev_name> <val/string>\r\n");
        SEGGER_RTT_printf(0, "  dev ioctl <dev_name> <cmd> [arg]\r\n");
        return;
    }

    const char *op       = argv[1];
    const char *dev_name = argv[2];

    // 1. 尝试打开外设 (匹配 vfs_open 单参数)
    int fd = vfs_open(dev_name);
    if (fd < 0) {
        SEGGER_RTT_printf(0, "\r\n[DEV] Error: Device '%s' not found!\r\n", dev_name);
        return;
    }

    // 2. 读外设 (dev read <dev_name>)
    if (strcmp(op, "read") == 0) {
        uint32_t val = 0;
        // 【精准修正 2】：4 参数匹配 (fd, pos, buffer, size)，pos 传 0
        int ret = vfs_read(fd, 0, &val, sizeof(val));
        if (ret >= 0) {
            SEGGER_RTT_printf(0, "\r\n[DEV] Read '%s' -> Val: %u (0x%X)\r\n", dev_name, val, val);
        } else {
            SEGGER_RTT_printf(0, "\r\n[DEV] Read '%s' failed! Code: %d\r\n", dev_name, ret);
        }
    }
    // 3. 写外设 (dev write <dev_name> <val/string>)
    else if (strcmp(op, "write") == 0) {
        if (argc < 4) {
            SEGGER_RTT_printf(0, "\r\nUsage: dev write <dev_name> <val/string>\r\n");
            vfs_close(fd);
            return;
        }

        // 如果是对串口发送，直接发送文本字符串
        if (strcmp(dev_name, "uart") == 0 || strcmp(dev_name, "sys_uart") == 0) {
            const char *str = argv[3];
            // 【精准修正 3】：pos 传 0，直接发送字符串
            int ret = vfs_write(fd, 0, str, strlen(str));
            SEGGER_RTT_printf(0, "\r\n[DEV] Sent string '%s' to %s -> Ret:%d\r\n", str, dev_name, ret);
        } 
        // 普通硬件写 32 位整型数值
        else {
            uint32_t val = strtoul(argv[3], NULL, 0);
            // 【精准修正 4】：pos 传 0，写 4 字节数据
            int ret = vfs_write(fd, 0, &val, sizeof(val));
            if (ret >= 0) {
                SEGGER_RTT_printf(0, "\r\n[DEV] Write '%s' <- %u (0x%X) OK\r\n", dev_name, val, val);
            } else {
                SEGGER_RTT_printf(0, "\r\n[DEV] Write '%s' failed! Code: %d\r\n", dev_name, ret);
            }
        }
    }
    // 4. 设备 IOCTL 控制 (dev ioctl <dev_name> <cmd> [arg])
    else if (strcmp(op, "ioctl") == 0) {
        if (argc < 4) {
            SEGGER_RTT_printf(0, "\r\nUsage: dev ioctl <dev_name> <cmd> [arg]\r\n");
            vfs_close(fd);
            return;
        }
        int cmd = (int)strtoul(argv[3], NULL, 0);
        uint32_t arg_val = (argc >= 5) ? strtoul(argv[4], NULL, 0) : 0;

        int ret = vfs_ioctl(fd, cmd, (void *)(uintptr_t)arg_val);
        SEGGER_RTT_printf(0, "\r\n[DEV] ioctl '%s' cmd:%d arg:0x%X -> Ret:%d\r\n", dev_name, cmd, arg_val, ret);
    }
    else {
        SEGGER_RTT_printf(0, "\r\n[DEV] Unknown op '%s'. Use read/write/ioctl.\r\n", op);
    }

    // 5. 操作完毕，关闭设备
    vfs_close(fd);
}
static void cmd_app(int argc, char *argv[]) {
    if (argc < 2) {
        SEGGER_RTT_printf(0, "\r\nUsage:\r\n");
        SEGGER_RTT_printf(0, "  app load            - 从当前最新槽位重新加载动态 App\r\n");
        SEGGER_RTT_printf(0, "  app unload          - 动态卸载所有扩展 App\r\n");
        return;
    }

    const char *action = argv[1];

    if (strcmp(action, "load") == 0) {
        SEGGER_RTT_printf(0, "\r\n[APP] 正在从槽位 %d 加载动态 App...\r\n", g_slot_index);
        // 计算当前最新槽位的物理基地址并加载
        uint32_t target_addr = APP_START_ADDR + g_slot_index * APP_SLOT_SIZE;
        vm_load_apps(target_addr);
        SEGGER_RTT_printf(0, "[APP] 动态 App 加载完毕并已挂载。\r\n");
    } 
    else if (strcmp(action, "unload") == 0) {
        SEGGER_RTT_printf(0, "\r\n[APP] 正在卸载动态 App...\r\n");
        vm_unload_apps(); // 执行你源码中的清空与关闭句柄逻辑
        SEGGER_RTT_printf(0, "[APP] 卸载完成，沙箱已隔离。\r\n");
    } 
    else {
        SEGGER_RTT_printf(0, "\r\n[APP] 未知操作 '%s'。请使用 'app load' 或 'app unload。\r\n", action);
    }
}
static const shell_cmd_t s_cmd_table[] = {
    {"help",   cmd_help,   "Show available commands list"},
    {"ps",     cmd_ps,     "Show RTOS tasks status and CPU stack info"},
    {"lsdev",  cmd_lsdev,  "List all registered VFS devices"},
    {"dev",    cmd_dev,    "Read/Write/Ioctl VFS hardware devices"}, 
    {"app",    cmd_app,    "Load or unload VM bytecode apps dynamically"}, // <-- 新增的 App 动态管理命令
    {"clear",  cmd_clear,  "Clear RTT terminal screen"},
    {"reboot", cmd_reboot, "Software reset MCU"},
};

#define CMD_COUNT (sizeof(s_cmd_table) / sizeof(s_cmd_table[0]))

static void cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    SEGGER_RTT_printf(0, "\r\nAvailable Commands:\r\n");
    for (size_t i = 0; i < CMD_COUNT; i++) {
        SEGGER_RTT_printf(0, "  %-10s - %s\r\n", s_cmd_table[i].name, s_cmd_table[i].desc);
    }
    SEGGER_RTT_printf(0, "\r\n");
}

static void shell_prompt(void) {
    SEGGER_RTT_printf(0, "minirtos# ");
}

static void shell_execute(char *cmd_line) {
    char *argv[SHELL_MAX_ARGS];
    int   argc = 0;

    // 1. 空格切分参数 (strtok 逐词切分)
    char *token = strtok(cmd_line, " ");
    while (token != NULL && argc < SHELL_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    if (argc == 0) {
        return;
    }

    // 2. 查表查找匹配的命令并触发回调
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strcmp(argv[0], s_cmd_table[i].name) == 0) {
            s_cmd_table[i].func(argc, argv); // 触发回调，传入参数列表
            return;
        }
    }

    // 3. 未找到命令处理
    SEGGER_RTT_printf(0, "\r\nCommand not found: '%s'. Type 'help' for options.\r\n", argv[0]);
}

void shell_init(void) {
    s_buf_idx = 0;
    memset(s_rx_buf, 0, sizeof(s_rx_buf));

    SEGGER_RTT_printf(0, "\r\n");
    SEGGER_RTT_printf(0, "========================================\r\n");
    SEGGER_RTT_printf(0, "     MiniRTOS RTT Shell System          \r\n");
    SEGGER_RTT_printf(0, "========================================\r\n");
    shell_prompt();
}

void shell_poll_rtt(void) {
    // 循环读取 RTT Down-Buffer 中的所有字节
    while (SEGGER_RTT_HasKey()) {
        int ch = SEGGER_RTT_GetKey();
        if (ch < 0) {
            break;
        }

        // 1. 处理回车/换行 (CR / LF)
        if (ch == '\r' || ch == '\n') {
            if (s_buf_idx > 0) {
                s_rx_buf[s_buf_idx] = '\0'; // 字符串封口
                SEGGER_RTT_printf(0, "\r\n");
                shell_execute(s_rx_buf);    // 执行解析
                s_buf_idx = 0;             // 重置索引
            } else {
                SEGGER_RTT_printf(0, "\r\n");
            }
            shell_prompt();
        }
        // 2. 处理退格删除 (Backspace: 0x08 / Delete: 0x7F)
        else if (ch == '\b' || ch == 0x7F) {
            if (s_buf_idx > 0) {
                s_buf_idx--;
                SEGGER_RTT_printf(0, "\b \b"); // 光标左移、输出空格覆盖、再左移
            }
        }
        // 3. 正常字符写入缓冲区
        else if (s_buf_idx < (SHELL_BUF_SIZE - 1)) {
            // 过滤掉不可见控制字符
            if (ch >= 32 && ch <= 126) {
                s_rx_buf[s_buf_idx++] = (char)ch;
                SEGGER_RTT_printf(0, "%c", (char)ch); // 终端回显
            }
        }
    }
}