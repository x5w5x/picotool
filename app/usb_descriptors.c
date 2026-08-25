#include "tusb.h"
#include "cmsis_dap.h"
#include <string.h>

#define USB_PID   0xD003
#define USB_VID   0x2E8A

// 接口枚举索引 (共 3 个接口: 1 个 HID + 2 个 CDC)
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

// 端点分配
#define EPNUM_HID         0x01
#define EPNUM_CDC_NOTIF   0x82
#define EPNUM_CDC_DATA    0x03

// 1. 设备描述符 (必须标记为 MISC / IAD 复合设备类)
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,        // 0xEF (MISC)
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,   // 0x02 (Common)
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,      // 0x01 (IAD)
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// 2. HID 报表描述符 (CMSIS-DAP 专用)
uint8_t const desc_hid_report[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x15, 0x00,        // Logical Minimum (0)
    0x26, 0xFF, 0x00,  // Logical Maximum (255)
    0x75, 0x08,        // Report Size (8 bits)
    0x95, 0x40,        // Report Count (64 bytes)
    0x09, 0x01,        // Usage (0x01)
    0x81, 0x02,        // Input (Data, Var, Abs)
    0x09, 0x01,        // Usage (0x01)
    0x91, 0x02,        // Output (Data, Var, Abs)
    0xC0               // End Collection
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// 3. 配置描述符 (HID + CDC IAD 复合)
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)

uint8_t const desc_configuration[] = {
    // A. 配置头 (3 个接口)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // B. HID 描述符 (CMSIS-DAP)
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report),
                             EPNUM_HID, 0x80 | EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 1),

    // C. CDC 描述符 (虚拟串口，自动携带 IAD)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_DATA, 0x80 | EPNUM_CDC_DATA, CFG_TUD_CDC_EP_BUFSIZE)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// 4. 字符串描述符
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: 英语
    "Raspberry Pi",             // 1: Manufacturer
    "Pico CMSIS-DAP",           // 2: Product (Keil 识别关键字)
    "12345678",                 // 3: Serial
    "CMSIS-DAP",                // 4: HID Interface
    "Pico CDC Serial",          // 5: CDC Interface
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

// 5. HID 数据回调
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