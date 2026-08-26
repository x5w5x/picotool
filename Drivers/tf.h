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

#define TF_CT_NONE        0x00
#define TF_CT_SD1         0x01  /* V1 Standard Capacity (SDSC) */
#define TF_CT_SD2_SC      0x02  /* V2 Standard Capacity (SDSC) */
#define TF_CT_SD2_HC      0x04  /* V2 High Capacity (SDHC/SDXC) */

/* 初始化与状态 API */
int      TF_Init(void);
uint8_t  TF_IsReady(void);
int      TF_ReadCID(uint8_t buf[16]);
int      TF_ReadCSD(uint8_t buf[16]);
int      TF_GetSectors(uint32_t *sectors);

/* 块读写 API (LBA: 逻辑块地址, count: 块数量) */
int      TF_ReadBlocks(uint32_t lba, uint8_t *buf, uint32_t count);
int      TF_WriteBlocks(uint32_t lba, const uint8_t *buf, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* TF_H */