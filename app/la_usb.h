/*
 * LA over USB CDC —— 逻辑分析仪上位机协议引擎
 *
 * 与 Tang Nano 9K 逻辑分析仪同一套帧协议:
 *   下行  A5 | CMD | LEN(1B) | PAYLOAD | CRC8(poly 0x07)
 *   上行  5A | CMD | LEN(2B LE) | PAYLOAD | CRC8
 *   CRC 覆盖 [CMD, LEN, PAYLOAD]，不含帧头。
 *
 * 通道: CH0~CH3 = GP4~GP7 (3.3V)。GP2/GP3(SWCLK/SWDIO) 留给 DAPLink,
 *       要观察 SWD 波形可跳线 GP2->GP4、GP3->GP5 旁听。
 * 深度: 32768 点 (8bit/点, 32KB 缓冲锚定在 0x2003C000)
 * 采样: PIO IN PINS,8 ×4 + autopush(32) + **右移** —— 1 word = 4 样本,
 *       ISR = s0|s1<<8|s2<<16|s3<<24, 小端落盘 = [s0,s1,s2,s3] 时间正序。
 *       用左移会变成 [s3,s2,s1,s0](每 4 点倒序), 波形假毛刺、解码全乱。
 *       基址 GP4 的 IN PINS,8 会带上 GP8~GP11, 上位机只用 bit0~3。
 * 版本: IDENT 返回 ver=3 (2=字节序已修正, 3=再增加 RLE)。旧固件 ver=1 需
 *       上位机自行做 4 字节反序还原。
 * 触发: PIO WAIT GPIO 指令 —— 硬件级电平触发, 精度 = 采样时钟;
 *       触发时刻即第 0 个采样点 (全 post-trigger)。
 * 连续: 双 DMA 通道 chain 乒乓, DMA_IRQ_0 记 seq/ready_mask。
 * 单帧 READ 上限 1024 字节, 上位机需分块读取。
 * RLE:  0x0A 命令使能 (ver>=3)。采样缓冲**原地**压缩, 输出恒 <= 输入:
 *       0x00~0x7F 字面量(1字节=1样本); 0x80~0xFF 游程(低7位=count-1,
 *       后跟1字节值), count 1~127。静态信号约 64x, 最坏情况 1:1 不膨胀。
 *       单次: 采完(或早停)后压缩, STATUS 的 valid = 压缩流长度。
 *       连续: 主循环每完成一个半区就地压缩, STATUS 的 st[9..10] =
 *       最新半区压缩长度 (溢出放弃时为 0); 上位机展开后还原 16384 点。
 */
#ifndef LA_USB_H
#define LA_USB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 主循环里轮询: 喂 USB CDC 字节 + 推进采集状态机 */
void la_usb_poll(void);

/* LA 引擎是否占用 plugin RAM 池 (0x20038000, 32KB) 当采样缓冲。
 * 活跃期间 app_loader 拒绝加载 plugin; 非采集中 60 秒无命令自动释放。 */
bool la_usb_pool_active(void);

#ifdef __cplusplus
}
#endif

#endif // LA_USB_H
