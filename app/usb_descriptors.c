/* =============================================================
 * @file   app/usb_descriptors.c
 * @brief  TinyUSB USB 描述符 + 双 PID 模式切换 + HID DAP 回调入口
 *
 * @section 角色
 *   - VID = 0x2E8A(Raspberry Pi)
 *   - 双 PID 切换核心:
 *     PID_DAP_CDC = 0xD003 → HID(DAP) + CDC 串口(默认)
 *     PID_MSC_CDC = 0xD004 → CDC 串口 + MSC U 盘(打开 TF 卡虚拟磁盘模式)
 *   - 切换流程:usb_switch_mode → tud_disconnect → sleep 150ms(Windows 完全卸载)
 *     → 改 s_usb_mode → tud_connect(热插拔)
 *   - tud_hid_set_report_cb 直接把 64 字节 buffer 喂给 dap_process_command
 *
 * @section 导出符号
 *   - tud_descriptor_device_cb / configuration_cb / string_cb(标准 TinyUSB 回调)
 *   - tud_hid_descriptor_report_cb / tud_hid_set_report_cb / tud_hid_get_report_cb
 *   - tud_cdc_rx_cb(空实现)
 *   - usb_switch_mode(uint8_t mode):双 PID 热切换
 *
 * @section 依赖
 *   - tusb.h / cmsis_dap.h(g_dap_enabled + dap_process_command)
 *   - pico/time
 *
 * @section 待重构 (P2)
 *   - 字符串描述符硬编码 7 项,加新接口需改两处(desc 配置 + string_desc_arr)
 *   - PID 切换需 150ms 强延时,实际 Windows 需要时间会因主机而异
 * ============================================================= */
#include "tusb.h"
#include "cmsis_dap.h"
#include "pico/time.h"
#include <string.h>

#define USB_VID          0x2E8A
#define PID_DAP_CDC      0xD003  // 默认模式 PID (DAP + 串口，无盘符)
#define PID_MSC_CDC      0xD004  // U盘模式 PID (U盘 + 串口)

typedef enum {
    USB_MODE_DAP_CDC = 0,
    USB_MODE_MSC_CDC = 1
} usb_mode_t;

static volatile usb_mode_t s_usb_mode = USB_MODE_DAP_CDC;

// 1. 设备描述符 (根据模式动态切换 PID)
static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = PID_DAP_CDC,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    desc_device.idProduct = (s_usb_mode == USB_MODE_MSC_CDC) ? PID_MSC_CDC : PID_DAP_CDC;
    return (uint8_t const *)&desc_device;
}

// 2. HID 报表描述符 (CMSIS-DAP)
uint8_t const desc_hid_report[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40,
    0x09, 0x01, 0x81, 0x02, 0x09, 0x01, 0x91, 0x02, 0xC0
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// 3. 配置描述符 A: 默认模式 (HID + CDC，共 3 个接口，无 MSC)
#define CONFIG_DAP_CDC_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)
static uint8_t const desc_dap_cdc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_DAP_CDC_LEN, 0x00, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report),
                             0x01, 0x81, CFG_TUD_HID_EP_BUFSIZE, 1),
    TUD_CDC_DESCRIPTOR(1, 5, 0x82, 8, 0x03, 0x83, CFG_TUD_CDC_EP_BUFSIZE)
};

// 4. 配置描述符 B: U 盘模式 (CDC + MSC，共 3 个接口)
#define CONFIG_MSC_CDC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
static uint8_t const desc_msc_cdc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_MSC_CDC_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, 5, 0x82, 8, 0x03, 0x83, CFG_TUD_CDC_EP_BUFSIZE),
    TUD_MSC_DESCRIPTOR(2, 6, 0x04, 0x84, 64)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    if (s_usb_mode == USB_MODE_MSC_CDC) {
        return desc_msc_cdc_configuration;
    }
    return desc_dap_cdc_configuration;
}

// 5. 字符串描述符
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Raspberry Pi",
    "Pico Multi-Tool",
    "12345678",
    "CMSIS-DAP",
    "Pico CDC Serial",
    "Pico TF Storage",
};

static uint16_t _desc_str[32];
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

// 6. 核心模式切换函数 (触发 USB 热重插拔)
void usb_switch_mode(uint8_t mode) {
    if ((uint8_t)s_usb_mode == mode) return;
    tud_disconnect();
    sleep_ms(150); // 确保 Windows 完全卸载当前驱动栈
    s_usb_mode = (usb_mode_t)mode;
    tud_connect();
}

// 7. 回调处理
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)bufsize;
    if (!g_dap_enabled) return;

    static uint8_t resp_buf[64];
    memset(resp_buf, 0, sizeof(resp_buf));
    dap_process_command(buffer, resp_buf);
    g_dap_packet_counter++;
    tud_hid_report(0, resp_buf, 64);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_cdc_rx_cb(uint8_t itf) {
    (void)itf;
}