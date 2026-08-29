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

#ifdef __cplusplus
}
#endif

#endif // APP_LOADER_H