#ifndef APP_FONTBURN_H
#define APP_FONTBURN_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 字库烧录工具:把 TF 卡上的 gb2312_*.bin 写进 W25Q64,
 * 带 64KB 块擦除、分片写入和逐字节读回校验。 */
app_t *app_fontburn_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif
