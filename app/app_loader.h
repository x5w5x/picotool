/*
 * @Author: 轩
 * @Date: 2026-09-05 20:16:55
 * @LastEditTime: 2026-09-23 15:22:41
 * @FilePath: \pico\app\app_loader.h
 */
#ifndef APP_LOADER_H
#define APP_LOADER_H

#include "app_base.h"
#include "app_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void   app_loader_init(void);
app_t *app_loader_get_instance(void);

bool   app_loader_load_papp(const char *filepath);
void   app_loader_unload_current(void);
bool   app_loader_is_running(void);
// 供其他静态 App 在闲置期间借用 48KB 内存池
uint8_t *app_loader_get_ram_pool(void);
uint32_t app_loader_get_ram_pool_size(void);
#ifdef __cplusplus
}
#endif

#endif // APP_LOADER_H