/* ================================================================
 * usb_print_test.c — 100% Pico SDK，零依赖 Mini Kernel
 *
 * 用途：验证 USB CDC 双向通信（输出 + 输入回显）。
 *   - 每 500ms 打印 CDC 状态诊断 + 测试行
 *   - 直接用 TinyUSB API 检查 CDC RX FIFO
 *   - LED 亮 100ms 标识收到数据
 *
 * 【自描述符说明】（v2.2 修复）：
 *   为了不与 mini-kernel 全局过滤 pico_stdio_usb_descriptors.c 冲突，
 *   本文件自带 CDC-only 描述符（不需要 stdio_usb_descriptors.c），
 *   同时提供 MSC 回调空桩（CFG_TUD_MSC 全局被开启时链接需要），
 *   但描述符里没声明 MSC Interface，主机永远不会调用这些空桩。
 * ================================================================ */
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#ifndef PICO_DEFAULT_LED_PIN
#  define PICO_DEFAULT_LED_PIN 25
#endif

/* ============ USB 描述符（自提供，不依赖 pico_stdio_usb_descriptors.c） ============ */

#define _PID_MAP(itf, n)   ( (CFG_TUD_##itf) << (n) )
#define USB_PID            (0x000a | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | \
                            _PID_MAP(HID, 2) | _PID_MAP(MIDI, 3) | _PID_MAP(VENDOR, 4) )
#define USB_VID            0x2E8A
#define USB_BCD            0x0200

/* Device descriptor */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* Configuration descriptor (CDC only, 2 interface) */
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#if CFG_TUSB_MCU == OPT_MCU_RP2040
#  define EPNUM_CDC_NOTIF  1
#  define EPNUM_CDC_OUT    2
#  define EPNUM_CDC_IN     2
#else
#  define EPNUM_CDC_NOTIF  1
#  define EPNUM_CDC_OUT    2
#  define EPNUM_CDC_IN     2
#endif

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x80 | EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, 0x80 | EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* String descriptors */
char const* string_desc_arr [] = {
    (const char[]) {0x09, 0x04},     /* 0: is supported language is English (0x0409) */
    "Raspberry Pi",                  /* 1: Manufacturer */
    "Pico CDC Test (usb_print_test)",/* 2: Product */
    "usb-print-test-0001",           /* 3: Serials, should use chip ID */
    "pico_stdio_usb CDC",            /* 4: CDC Interface */
};

static uint16_t _desc_str[32];
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0]))) return NULL;
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1+i] = (uint16_t) str[i];
    }
    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* ============ MSC 空桩（应对全局 CFG_TUD_MSC=1 开启时的链接要求） ============
 * 说明：本测试固件 Configuration 描述符里**没有声明 MSC Interface**，
 *       主机不会枚举到 MSC，因此这些回调永远不会被调用。
 *       它们只是为了满足 tinyusb msc_device.c 的链接要求。
 * ======================================================================== */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun; (void)vendor_id; (void)product_id; (void)product_rev;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_count = 0;
    *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void* buffer, uint32_t bufsize) {
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t* buffer, uint32_t bufsize) {
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void* buffer, uint16_t bufsize) {
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    return -1;
}

/* ============ main ============ */

int main(void) {
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;

    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);

    /* stdio_init_all() 同时启用 UART0 和 USB CDC */
    stdio_init_all();

    uint32_t n = 0;
    while (1) {
        /* 驱动 TinyUSB 状态机（双保险，SDK IRQ 也应该调） */
        tud_task();

        /* 打印 CDC 状态 + 测试行 */
        printf("[%lu] ready=%d cdc_conn=%d avail=%d\n",
               (unsigned long)n++,
               (int)tud_ready(),
               (int)tud_cdc_connected(),
               (int)tud_cdc_available());
        fflush(stdout);

        /* 方法 1：直接 TinyUSB CDC 读 */
        if (tud_cdc_available()) {
            char buf[64];
            uint32_t count = tud_cdc_read(buf, sizeof(buf) - 1);
            buf[count] = '\0';
            printf("CDC_RX(%u): %s\n", (unsigned)count, buf);
            gpio_put(LED, 1);
            sleep_ms(100);
            gpio_put(LED, 0);
        }

        /* 方法 2：SDK stdio 读（fallback） */
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            printf("STDIO_RX: 0x%02X\n", ch);
            gpio_put(LED, 1);
            sleep_ms(100);
            gpio_put(LED, 0);
        }

        sleep_ms(500);
    }
    return 0;
}
