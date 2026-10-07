/*
 * @Author: 轩
 * @Date: 2026-10-06 15:10:11
 * @LastEditTime: 2026-10-07 11:52:41
 * @FilePath: \pico\fun\fun_ir.h
 */
#ifndef FUN_IR_H
#define FUN_IR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 扩充至 400 脉冲，完整容纳空调 199/299 脉冲多帧重传长报文
#define IR_MAX_PULSES       400 
#define IR_DEFAULT_RX_PIN   4
#define IR_DEFAULT_TX_PIN   5

typedef enum {
    IR_PROTO_UNKNOWN = 0,
    IR_PROTO_RAW,       // 空调或特殊协议原始微秒时序
    IR_PROTO_NEC        // 标准 NEC 协议
} ir_protocol_t;

typedef struct {
    ir_protocol_t   protocol;
    uint16_t        pulses[IR_MAX_PULSES]; // 原始微秒序列 (Mark 0, Space 0, Mark 1...)
    uint16_t        pulse_count;
    
    // 若识别为 NEC 协议，提取解析字段
    uint8_t         nec_addr;
    uint8_t         nec_cmd;
    bool            nec_valid;
} ir_packet_t;

// 1. 底层驱动生命周期
bool     fun_ir_init(uint8_t rx_pin, uint8_t tx_pin);
void     fun_ir_deinit(void);

// 2. 学习接收控制 (由 Core 1 独占轮询，外部传入共享内存缓冲指针)
void     fun_ir_rx_start(uint16_t *sample_buf, ir_packet_t *pkt_out);
void     fun_ir_rx_stop(void);
bool     fun_ir_rx_poll(ir_packet_t *packet);

// 3. 38kHz PWM 载波发射重放
void     fun_ir_tx_send(const ir_packet_t *packet);

// 4. 协议解码与实时诊断探针
bool     fun_ir_decode_nec(ir_packet_t *packet);
bool     fun_ir_get_pin_level(void);
uint32_t fun_ir_get_edge_count(void);

// 5. TF 卡波形文件导入导出
bool     fun_ir_save_auto(const ir_packet_t *packet, char *out_path, size_t max_len);
bool     fun_ir_load_file(const char *filepath, ir_packet_t *packet);

#endif // FUN_IR_H