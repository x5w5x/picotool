/* =============================================================
 * plugin 入口点 — 二进制 offset 0 (见 plugin.ld .entry)
 *
 * 整个 plugin 拆成三件套,与 host app (app_led.c/.h) 风格对齐:
 *   - plugin_led_controller.h : 描述符单例声明 (led_controller_get_descriptor)
 *   - plugin_led_controller.c : 状态结构体 + 生命周期回调 + UI 渲染
 *   - main.c                  : 仅作为插件入口,落 .entry 段,
 *                              把 api 转发给描述符里的 init 回调
 *
 * host loader 调用流程:
 *   app_loader.c  f_read → app_entry(api)
 *                            └──► led_controller_get_descriptor()
 *                            └──► 返回 app_plugin_t* 给 host
 *   host 然后调 plugin->init(api) → plugin 拿到 api 自己存起来
 *
 * 必须用 __attribute__((used)) 否则 -fdata-sections + --gc-sections 会砍掉
 * ============================================================= */

#include "app_api.h"
#include "plugin_led_controller.h"

__attribute__((section(".entry"), used))
const app_plugin_t *app_entry(const pico_host_api_t *api) {
    (void)api;  // api 由 host 在 init() 阶段注入,入口这里不消费
    return led_controller_get_descriptor();
}
