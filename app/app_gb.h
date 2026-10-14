#ifndef APP_GB_H
#define APP_GB_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Game Boy 模拟器静态 App (基于 Peanut-GB)。
 * 会话中直接从 TF 卡读取 .gb / .gbc,自行按 bank 缓存 ROM,
 * 逐行渲染并直接推 LCD,不走 ui_tile 切片管线。 */
app_t *app_gb_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif
