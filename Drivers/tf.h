/*
 * @Author: 轩
 * @Date: 2026-08-18 16:29:25
 * @LastEditTime: 2026-08-18 16:41:53
 * @FilePath: \pico\Drivers\tf.h
 */
#ifndef TF_H
#define TF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define TF_BLOCK_SIZE     512
#define TF_OK              0
#define TF_ERR_TIMEOUT    -1
#define TF_ERR_INIT       -2
#define TF_ERR_CMD        -3
#define TF_ERR_READ       -4
#define TF_ERR_WRITE      -5

/* 初始化与状态 API */
int      TF_Init(void);
uint8_t  TF_IsReady(void);
int      TF_ReadCID(uint8_t buf[16]);

/* 块读写 API (LBA: 逻辑块地址, count: 块数量) */
int      TF_ReadBlocks(uint32_t lba, uint8_t *buf, uint32_t count);
int      TF_WriteBlocks(uint32_t lba, const uint8_t *buf, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* TF_H */