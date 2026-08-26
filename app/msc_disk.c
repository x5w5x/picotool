/*
 * @FilePath: \pico\app\msc_disk.c
 * @Description: TinyUSB MSC (Mass Storage Class) ↔ Drivers/tf.c 桥接层
 */
#include "tusb.h"
#include "tf.h"
#include "pico/time.h"
#include <string.h>
#include <stdbool.h>

static bool s_msc_ready = false;

// ⚡ 拖拽传输监听状态
static volatile uint64_t s_last_msc_write_us = 0;
static volatile bool     s_msc_has_written   = false;

bool msc_disk_check_write_completed(uint32_t quiet_ms) {
    if (!s_msc_has_written) return false;
    uint64_t now = time_us_64();
    if ((now - s_last_msc_write_us) > ((uint64_t)quiet_ms * 1000ULL)) {
        s_msc_has_written = false;
        return true;
    }
    return false;
}

void msc_disk_reset_write_state(void) {
    s_msc_has_written = false;
    s_last_msc_write_us = 0;
}

// 1. 电脑端查询设备信息 (Inquiry)
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "PicoHub ";
    const char pid[] = "TF Card Reader  ";
    const char rev[] = "1.0 ";

    memcpy(vendor_id,   vid, 8);
    memcpy(product_id,  pid, 16);
    memcpy(product_rev, rev, 4);
}

// 2. 检查单元就绪状态 (Test Unit Ready)
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return s_msc_ready && TF_IsReady();
}

// 3. 获取容量与扇区大小
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    uint32_t sectors = 0;
    if (TF_GetSectors(&sectors) == TF_OK && sectors > 0) {
        *block_count = sectors;
        *block_size  = TF_BLOCK_SIZE;
    } else {
        *block_count = 0;
        *block_size  = 512;
    }
}

// 4. 电脑从 TF 卡读取扇区 (Read10)
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;

    if (!s_msc_ready || !TF_IsReady()) {
        return -1;
    }

    uint32_t count = bufsize / TF_BLOCK_SIZE;
    if (TF_ReadBlocks(lba, (uint8_t *)buffer, count) == TF_OK) {
        return (int32_t)bufsize;
    }

    return -1;
}

// 5. 电脑向 TF 卡写入扇区 (Write10)
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;

    if (!s_msc_ready || !TF_IsReady()) {
        return -1;
    }

    uint32_t count = bufsize / TF_BLOCK_SIZE;
    if (TF_WriteBlocks(lba, (const uint8_t *)buffer, count) == TF_OK) {
        s_last_msc_write_us = time_us_64();
        s_msc_has_written   = true;
        return (int32_t)bufsize;
    }

    return -1;
}

// 6. SCSI 指令未处理透传
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    return -1;
}

// 7. App 外部控制开关接口
void msc_set_enabled(bool enabled) {
    s_msc_ready = enabled;
    if (enabled && !TF_IsReady()) {
        TF_Init();
    }
}