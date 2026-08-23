#ifndef TOUCH_H
#define TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ===================== FT6336U I2C 从机地址与寄存器 ===================== */
#define FT6336_ADDR         0x38

#define FT_REG_DEV_MODE     0x00
#define FT_REG_GEST_ID      0x01
#define FT_REG_TD_STATUS    0x02
#define FT_REG_TH_GROUP     0x80   /* 触摸灵敏度门限 */
#define FT_REG_PERIODACTIVE 0x88   /* 活跃状态采样周期 */
#define FT_REG_CHIP_ID      0xA3

/* ===================== 触摸数据结构体 (含滤波与速度) ===================== */
typedef struct {
    uint8_t  touch_num;     /* 触点数量 (0, 1, 2) */
    uint16_t x0;            /* EMA 滤波后的稳定坐标 X */
    uint16_t y0;            /* EMA 滤波后的稳定坐标 Y */
    uint16_t x1;            /* 触点 2 X 坐标 */
    uint16_t y1;            /* 触点 2 Y 坐标 */
    uint8_t  gesture;       /* 手势 ID */
    float    vy;            /* Y 轴瞬时滑动速度 (像素/毫秒，负数向上，正数向下) */
} Touch_Data_t;

/* ===================== 核心 API ===================== */
bool    Touch_Init(void);
uint8_t Touch_ReadReg(uint8_t reg);
void    Touch_WriteReg(uint8_t reg, uint8_t data);
bool    Touch_HasEvent(void);
bool    Touch_ReadData(Touch_Data_t *data);
bool    Touch_IsTouched(void);
uint8_t Touch_GetChipID(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_H */