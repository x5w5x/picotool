#ifndef APP_SHARED_RAM_H
#define APP_SHARED_RAM_H

#include <stdint.h>

#define STATIC_APP_RAM_SIZE (8 * 1024)

// 静态 App 共享内存池（常规 BSS 分配）
extern uint8_t g_static_app_ram[STATIC_APP_RAM_SIZE];

#endif