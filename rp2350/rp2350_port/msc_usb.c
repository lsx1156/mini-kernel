/**
 * @file    msc_usb.c
 * @brief   RP2350 USB Composite (CDC + MSC) 描述符与端点实现
 * 
 * 基于 TinyUSB，提供：
 *   - CDC (Interface 0,1): 串口调试 COM 口
 *   - MSC (Interface 2):   U 盘功能
 * 
 * VID/PID 沿用 Pi Foundation (0x2E8A / 0x0005)
 */

#include "hal_port.h"
#include "msc_blockdev.h"
#include <tusb.h>
#include <string.h>

/* ================================================================
 * USB 描述符定义
 * ================================================================ */

#define USB_VID                       0x2E8A
#define USB_PID                       0x000A  /* 0x0005=Pico, 0x000A=Pico 2 */

/* 接口编号 */
enum {
    ITF_NUM_CDC = 0,      /* CDC CCI */
    ITF_NUM_CDC_DATA,     /* CDC Data */
    ITF_NUM_MSC,          /* MSC */
    ITF_NUM_TOTAL
};

/* 端点分配 */
#define EPNUM_CDC_NOTIF               0x81  /* EP1 IN  - CDC 通知 */
#define EPNUM_CDC_OUT                 0x02  /* EP2 OUT - CDC 数据接收 */
#define EPNUM_CDC_IN                  0x82  /* EP2 IN  - CDC 数据发送 */
#define EPNUM_MSC_OUT                 0x03  /* EP3 OUT - MSC 接收 */
#define EPNUM_MSC_IN                  0x83  /* EP3 IN  - MSC 发送 */

/* ================================================================
 * 设备描述符
 * ================================================================ */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = TUSB_CLASS_MISC,  /* Composite 使用 MISC 类 */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 1
};

/* ================================================================
 * 配置描述符 (CDC + MSC)
 * ================================================================ */

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_configuration[CONFIG_TOTAL_LEN] = {
    /* Configuration Descriptor */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    
    /* CDC (Interface 0,1) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 64, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    
    /* MSC (Interface 2) */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64)
};

/* ================================================================
 * 字符串描述符
 * ================================================================ */

static const char *const string_desc[] = {
    (const char[]){0x09, 0x04},          // 0: 支持语言 English
    "Raspberry Pi",                       // 1: Manufacturer
    "Pico 2 (RP2350)",                   // 2: Product
    "123456789012",                      // 3: Serial (运行时从唯一 ID 生成)
    "CDC Serial",                        // 4: CDC Interface
    "Mass Storage",                      // 5: MSC Interface
};

/* ================================================================
 * TinyUSB 回调实现
 * ================================================================ */

/* 设备描述符 */
const tusb_desc_device_t *tud_descriptor_device_cb(void) {
    return &desc_device;
}

/* 配置描述符 */
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* 字符串描述符 */
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[64];
    uint8_t len = 0;
    
    if (index == 0) {
        desc_str[1] = 0x0409;  // English
        len = 1;
    } else if (index < sizeof(string_desc) / sizeof(string_desc[0])) {
        const char *str = string_desc[index];
        len = strlen(str);
        for (uint8_t i = 0; i < len; i++) {
            desc_str[1 + i] = str[i];
        }
    }
    
    desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * len + 2);
    return desc_str;
}

/* ================================================================
 * MSC 回调
 * ================================================================ */

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;
    DRESULT res = disk_read(0, buffer, lba, bufsize / 512);
    return (res == RES_OK) ? (int32_t)bufsize : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;
    DRESULT res = disk_write(0, buffer, lba, bufsize / 512);
    return (res == RES_OK) ? (int32_t)bufsize : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t *scsi_cmd, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)bufsize;
    /* 简化：只处理基本 SCSI 命令 */
    return 0;
}

/* MSC 准备就绪 */
bool tud_msc_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}

/* MSC 容量 */
uint32_t tud_msc_capacity_cb(uint8_t lun) {
    (void)lun;
    return MSC_BDEV_NUM_BLOCKS;
}

/* ================================================================
 * 导出描述符获取函数 (供 hal_port.h 声明)
 * ================================================================ */

const tusb_desc_device_t *msc_usb_get_device_descriptor(void) {
    return &desc_device;
}

const char **msc_usb_get_string_descriptors(void) {
    return string_desc;
}

const uint8_t *msc_usb_get_config_descriptor(void) {
    return desc_configuration;
}