/*
 * 电平触发逻辑分析仪 App
 * CH0~CH5 对应 GP2~GP7，PIO 采样 + DMA 搬运，最多 4096 点
 */
#ifndef APP_LA_H
#define APP_LA_H

#include "app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

app_t *app_la_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // APP_LA_H
