#include "app_api.h"
const app_plugin_t *dino_runner_get_descriptor(void);
__attribute__((section(".entry"), used))
const app_plugin_t *app_entry(const pico_host_api_t *api) {
    (void)api;
    return dino_runner_get_descriptor();
}
