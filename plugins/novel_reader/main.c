/* =============================================================
 * plugin 入口点 — 二进制 offset 0 (见 plugin.ld .entry)
 * ============================================================= */

#include "app_api.h"
#include "plugin_novel_reader.h"

__attribute__((section(".entry"), used))
const app_plugin_t *app_entry(const pico_host_api_t *api) {
    (void)api;
    return novel_reader_get_descriptor();
}