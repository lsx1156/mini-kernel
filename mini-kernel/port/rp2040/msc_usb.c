/**
 * @file    msc_usb.c
 * @brief   v2.2 USB Composite：CDC（原串口调试）+ MSC（新 U 盘）同时枚举
 *
 *  【设计说明】
 *  Pico SDK 的 pico_stdio_usb 模块默认只提供 CDC 单接口，并以 weak 符号
 *  提供了 tud_desc_device_cb / tud_desc_configuration_cb 等描述符回调。
 *  本文件以"非弱符号同名函数"覆盖这些弱符号（linker 优先选本文件实现），
 *  把描述符替换成 IAD 模式的 CDC+MSC 双接口复合设备。
 *
 *  主机端表现：
 *    · 同时出现 1 个 COM 口（原来的 CDC 命令行 mk> 正常用）
 *    · 同时出现 1 个"可移动磁盘"盘符（容量 1016KB，FAT16，可建子目录）
 *
 *  端点分配（Full Speed）：
 *    EP0       Control
 *    EP01 IN   CDC 数据 BULK IN       (0x81)
 *    EP01 OUT  CDC 数据 BULK OUT      (0x01)
 *    EP02 IN   CDC 中断通知          (0x82, 16ms interval, 8 bytes)
 *    EP03 OUT  MSC BOT BULK OUT       (0x03)
 *    EP04 IN   MSC BOT BULK IN        (0x84)
 *
 *  注意：Pico SDK 的 stdio_usb 只认 CDC 接口编号为 0、data iface=1。
 *  本描述符严格保持"Interface 0 = CDC Comm, Interface 1 = CDC Data"，
 *  这样 stdio_usb 的 CDC ACM 回调仍正常工作，输出无需改代码。
 *  MSC 放在 Interface 2，不与 stdio_usb 的内部逻辑冲突。
 */

#include "tusb.h"
#include "msc_blockdev.h"
#include "hal/flash_layout.h"
#include <string.h>
#include <stdint.h>

/* TinyUSB msc.h 未定义 VERIFY(10) opcode；SCSI 标准 opcode = 0x2F。
 * 我们自己补充一个宏，避免隐式未定义报错。 */
#ifndef SCSI_CMD_VERIFY_10
#define SCSI_CMD_VERIFY_10   0x2Fu
#endif

/* ======================================================================
 *  1. Device Descriptor（Composite IAD 模式）
 * ====================================================================== */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    /* IAD（Interface Association Descriptor）要求：bDeviceClass=0xEF,
     * SubClass=0x02, Protocol=0x01 → 告诉主机"配置描述符里有 IAD，
     * 不要按旧规则把接口0类=2当成单 CDC，要读取 IAD 组合关系"。 */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    /* VID/PID：沿用 Raspberry Pi Foundation 默认 USB Serial (0x2E8A/0x0005)
     * Windows 已内置 usbser.inf 兼容驱动，"USB Serial Device (COMx)"自动识别。
     * 类是 IAD，设备管理器会同时装 USB 串行端口 + 磁盘驱动器两个驱动。 */
    .idVendor           = 0x2E8Au,
    .idProduct          = 0x0005u,
    .bcdDevice          = 0x0220u,     /* v2.2 */

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* ======================================================================
 *  2. Configuration Descriptor（Composite: CDC(0+1) + MSC(2)）
 * ====================================================================== */
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

#define EPNUM_CDC_DATA    1     /* CDC 数据：BULK EP1 IN+OUT（Pico SDK 默认） */
#define EPNUM_CDC_NOTIF   2     /* CDC 通知：INT EP2 IN                 */
#define EPNUM_MSC_OUT     3     /* MSC BOT：BULK OUT                   */
#define EPNUM_MSC_IN      4     /* MSC BOT：BULK IN                    */

uint8_t const desc_configuration[] = {
    /* TUD_CONFIG_DESCRIPTOR(config_num, _itfcount, _stridx, _total_len, _attribute, _power_ma)
     *  注意：itfcount = 3  (CDC Comm 0 + CDC Data 1 + MSC 2)，此版本 TinyUSB 需要 6 个参数。 */
    TUD_CONFIG_DESCRIPTOR(
        /* config_num        */ 1,
        /* itf_count         */ 3,
        /* string index      */ 0,
        /* total_length      */ CONFIG_TOTAL_LEN,
        /* power_attrs       */ TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        /* power_mA          */ 100),

    /* CDC Comm + Data Interface
     * TUD_CDC_DESCRIPTOR(_itfnum, _stridx, _ep_notif, _ep_notif_size, _epout, _epin, _epsize)
     *  注意：此版本 TinyUSB 只需要 7 个参数——Comm iface = _itfnum，Data iface 自动= _itfnum+1。
     *  所以 itf_num=0 → CDC Comm=0 / CDC Data=1（与 Pico SDK stdio_usb 默认约定一致）。 */
    TUD_CDC_DESCRIPTOR(
        /* itf_num (Comm)   */ 0,
        /* stridx (notif)   */ 4,
        /* ep_notif_addr    */ 0x80 | EPNUM_CDC_NOTIF,
        /* notif_epsize     */ 8,
        /* ep_out (Data)    */ EPNUM_CDC_DATA,
        /* ep_in  (Data)    */ 0x80 | EPNUM_CDC_DATA,
        /* bulk_epsize      */ 64),

    /* Mass Storage (SCSI transparent, BOT protocol) */
    TUD_MSC_DESCRIPTOR(
        /* itf_num          */ 2,
        /* stridx           */ 5,
        /* ep_out_addr      */ EPNUM_MSC_OUT,
        /* ep_in_addr       */ 0x80 | EPNUM_MSC_IN,
        /* bulk_epsize      */ 64)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* ======================================================================
 *  3. String Descriptors（ASCII → UTF16-LE 转换由 tusb_desc_strarray 处理）
 * ====================================================================== */
static char const* string_desc_arr [] = {
    (const char[]) {0x09, 0x04},     /* 0: 支持语言 ID (English, 0x0409) */
    "Mini Kernel Team",              /* 1: iManufacturer */
    "Mini Kernel v2.2 CDC+MSC",      /* 2: iProduct */
    "rp2040-00000001",               /* 3: iSerialNumber */
    "MiniKernel CDC ACM",            /* 4: CDC Comm iface string */
    "MiniKernel MSC Storage",        /* 5: MSC iface string */
};

static uint16_t _desc_str[32 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr)) return NULL;
        const char* str = string_desc_arr[index];
        /* 将 ASCII 转换为 UTF-16-LE，最多 32 字符 */
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 32) chr_count = 32;
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)(uint8_t)str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return _desc_str;
}

/* ======================================================================
 *  4. TinyUSB MSC Class 回调
 * ====================================================================== */

/* ---- Inquiry：告诉主机"这个 USB 存储器的厂家/产品/版本" ----
 *   此版本 TinyUSB：返回 void，产品信息通过 3 个输出数组返回。 */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "MINIKRNL";              /* 必须恰好 8 字节（不足补空格） */
    const char pid[] = "MINIK DATA DISK ";      /* 必须恰好 16 字节 */
    const char rev[] = "2.20";                  /* 必须恰好 4 字节 */
    memcpy(vendor_id,   vid, 8);
    memcpy(product_id,  pid, 16);
    memcpy(product_rev, rev, 4);
}

/* ---- Test Unit Ready：主机周期性查询介质是否就绪 ---- */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    /* ejected = true → 主机查询会得到 NOT READY / MEDIUM NOT PRESENT，
     *   符合"安全删除硬件"后的语义，Windows 上再插 U 盘需要
     *   用户 `msc mount`（或拔插 USB）。*/
    if (msc_blockdev_is_ejected()) {
        /* SENSE KEY = NOT READY, ASC = MEDIUM NOT PRESENT */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

/* ---- Capacity：介质容量（扇区数+每扇区字节） ---- */
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_count = (uint32_t)msc_blockdev_sector_count();
    *block_size  = (uint16_t)MSC_BLOCKDEV_SECTOR_BYTES;
}

/* ---- Sector Size：新 TinyUSB 版本 API 直接要求 512 ---- */
uint16_t tud_msc_sector_size_cb(uint8_t lun) {
    (void)lun;
    return MSC_BLOCKDEV_SECTOR_BYTES;
}

/* ---- Start/Stop Unit：主机"安全删除"/"弹出"/"重新加载介质" ---- */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                            bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject) {
        if (start) {
            /* 主机要求"加载介质"（拔出后重新插入） → 清 ejected */
            msc_blockdev_set_ejected(false);
        } else {
            /* 主机要求"弹出介质"（安全删除硬件） → 置 ejected */
            msc_blockdev_set_ejected(true);
        }
    }
    return true;
}

/* ---- Prevent/Allow Medium Removal：Windows "弹出前"锁定 ---- */
bool tud_msc_prevent_removal_cb(uint8_t lun, bool prevent) {
    (void)lun;
    (void)prevent;
    /* 我们的介质是内置 Flash，不支持物理移除，所以无论"允许/禁止移除"
     *   都直接返回 true。仅影响主机提示。 */
    return true;
}

/* ---- READ(10)：主机读扇区 → 把 Flash 数据填进 buffer 并返回字节数 ---- */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           void* buffer, uint32_t bufsize) {
    (void)lun;
    /* offset != 0：TinyUSB 分批传输，每次要 bufsize 字节（连续 LBA）。
     *   我们统一读 (bufsize / 512) 个扇区，然后以 sector 粒度读取。 */
    if ((bufsize % MSC_BLOCKDEV_SECTOR_BYTES) != 0u) {
        /* TinyUSB 有时会做非整扇区拆分（offset 不为 0 时 bufsize 可能更小）。
         *   这种情况做单扇区 read 后按偏移切片。*/
        uint8_t tmp[MSC_BLOCKDEV_SECTOR_BYTES];
        if (!msc_blockdev_read(lba, tmp)) return -1;
        if (offset + bufsize > MSC_BLOCKDEV_SECTOR_BYTES) return -1;
        memcpy(buffer, tmp + offset, bufsize);
        return (int32_t)bufsize;
    }
    uint32_t const count = bufsize / MSC_BLOCKDEV_SECTOR_BYTES;
    /* offset == 0 → 常规读，直接多扇区读；offset != 0 且 bufsize 是 512 的倍数
     *   也允许（理论上 offset 实际在正常 TinyUSB MSC 里永远是 0 或子扇区级）。
     *   这里为了简单，只对 offset=0 走 multi 路径；其他走子扇区拆分。 */
    if (offset == 0u) {
        if (!msc_blockdev_read_multi(lba, count, (uint8_t*)buffer)) return -1;
        return (int32_t)bufsize;
    } else {
        /* 跨多个 LBA 的 offset 情形：逐个 LBA 处理，先读全扇区再 memcpy 段 */
        uint8_t *dst = (uint8_t*)buffer;
        uint32_t remain = bufsize;
        uint32_t cur_off = offset;
        uint32_t cur_lba = lba;
        while (remain > 0u) {
            uint8_t tmp[MSC_BLOCKDEV_SECTOR_BYTES];
            if (!msc_blockdev_read(cur_lba, tmp)) return -1;
            uint32_t take = MSC_BLOCKDEV_SECTOR_BYTES - cur_off;
            if (take > remain) take = remain;
            memcpy(dst, tmp + cur_off, take);
            dst += take; remain -= take; cur_off = 0u; cur_lba++;
        }
        return (int32_t)bufsize;
    }
}

/* ---- WRITE(10)：主机写扇区 → 写 Flash ---- */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                            uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    /* 已弹出状态：主机尝试写 → 返回 CHECK CONDITION（Write Protected） */
    if (msc_blockdev_is_ejected()) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }
    if ((bufsize % MSC_BLOCKDEV_SECTOR_BYTES) != 0u) {
        /* 子扇区写（罕见但标准允许）。RMW：读整扇 → 改段 → 写回 */
        uint8_t tmp[MSC_BLOCKDEV_SECTOR_BYTES];
        if (!msc_blockdev_read(lba, tmp)) return -1;
        if (offset + bufsize > MSC_BLOCKDEV_SECTOR_BYTES) return -1;
        memcpy(tmp + offset, buffer, bufsize);
        if (!msc_blockdev_write(lba, tmp)) return -1;
        return (int32_t)bufsize;
    }
    uint32_t const count = bufsize / MSC_BLOCKDEV_SECTOR_BYTES;
    if (offset == 0u) {
        if (!msc_blockdev_write_multi(lba, count, buffer)) return -1;
        return (int32_t)bufsize;
    } else {
        uint8_t *src = (uint8_t*)buffer;
        uint32_t remain = bufsize;
        uint32_t cur_off = offset;
        uint32_t cur_lba = lba;
        while (remain > 0u) {
            uint8_t tmp[MSC_BLOCKDEV_SECTOR_BYTES];
            if (!msc_blockdev_read(cur_lba, tmp)) return -1;
            uint32_t take = MSC_BLOCKDEV_SECTOR_BYTES - cur_off;
            if (take > remain) take = remain;
            memcpy(tmp + cur_off, src, take);
            if (!msc_blockdev_write(cur_lba, tmp)) return -1;
            src += take; remain -= take; cur_off = 0u; cur_lba++;
        }
        return (int32_t)bufsize;
    }
}

/* ---- WRITE(10) 完成回调（可选，此处仅诊断） ---- */
void tud_msc_write10_complete_cb(uint8_t lun) {
    (void)lun;
}

/* ---- SCSI Command：非 READ10/WRITE10/TEST_UNIT_READY 的其他 SCSI 命令 ----
 *   此版本 TinyUSB：返回 int32_t，
 *     · >= 0 → 同步完成，返回值 = 实际写入 buffer 的响应字节数（可以为 0）
 *     · -1   → SCSI 命令失败（TinyUSB 会自动 STALL / 置 CHECK CONDITION）。*/
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void* buffer, uint16_t bufsize) {
    void const* resp = NULL;
    uint16_t resplen = 0u;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_READ_CAPACITY_10: {
            /* 容量 = sectors × bytes_per_sector（标准 512） */
            uint32_t sc = (uint32_t)msc_blockdev_sector_count() - 1u;  /* last LBA */
            uint32_t bs = MSC_BLOCKDEV_SECTOR_BYTES;
            static uint8_t rd_cap10[8];
            rd_cap10[0] = (uint8_t)((sc >> 24) & 0xFF);
            rd_cap10[1] = (uint8_t)((sc >> 16) & 0xFF);
            rd_cap10[2] = (uint8_t)((sc >> 8) & 0xFF);
            rd_cap10[3] = (uint8_t)(sc & 0xFF);
            rd_cap10[4] = (uint8_t)((bs >> 24) & 0xFF);
            rd_cap10[5] = (uint8_t)((bs >> 16) & 0xFF);
            rd_cap10[6] = (uint8_t)((bs >> 8) & 0xFF);
            rd_cap10[7] = (uint8_t)(bs & 0xFF);
            resp = rd_cap10;
            resplen = 8u;
            break;
        }

        case SCSI_CMD_READ_FORMAT_CAPACITY: {
            /* 读格式容量：当前介质容量 + 块大小（Windows/OS X 查询用） */
            static uint8_t rd_fmt_cap[12];
            memset(rd_fmt_cap, 0, sizeof(rd_fmt_cap));
            uint32_t sc = (uint32_t)msc_blockdev_sector_count();
            uint32_t bs = MSC_BLOCKDEV_SECTOR_BYTES;
            /* Capacity list header (4 bytes) + one descriptor entry (8 bytes) */
            rd_fmt_cap[3] = 0x08;   /* Capacity list length = 8 bytes (1 entry) */
            rd_fmt_cap[4] = (uint8_t)((sc >> 24) & 0xFF);
            rd_fmt_cap[5] = (uint8_t)((sc >> 16) & 0xFF);
            rd_fmt_cap[6] = (uint8_t)((sc >> 8) & 0xFF);
            rd_fmt_cap[7] = (uint8_t)(sc & 0xFF);
            rd_fmt_cap[8] = 0x02;   /* Descriptor type = formatted media */
            rd_fmt_cap[9] = (uint8_t)((bs >> 16) & 0xFF);
            rd_fmt_cap[10] = (uint8_t)((bs >> 8) & 0xFF);
            rd_fmt_cap[11] = (uint8_t)(bs & 0xFF);
            resp = rd_fmt_cap;
            resplen = 12u;
            break;
        }

        case SCSI_CMD_MODE_SENSE_6: {
            /* Mode Sense(6)：主机查询 Write Protect /介质状态 */
            static uint8_t ms6[4] = {0};
            ms6[0] = 0x03; ms6[1] = 0x00; ms6[2] = msc_blockdev_is_ejected() ? 0x80u : 0x00u; ms6[3] = 0x00;
            resp = ms6; resplen = 4u;
            if (bufsize < resplen) resplen = bufsize;
            break;
        }

        case SCSI_CMD_REQUEST_SENSE: {
            /* Request Sense：返回 18 字节"无错误"的 Sense Data（fixed format） */
            static uint8_t const req_sense[18] = {
                0x70,                       /* Response Code = Current Errors, Valid=1 */
                0x00,                       /* Obsolete */
                0x00,                       /* Sense Key = NO SENSE */
                0x00, 0x00, 0x00, 0x00,     /* Information (4 bytes) */
                0x0A,                       /* Additional Sense Length = 10 */
                0x00, 0x00, 0x00, 0x00,     /* Cmd-specific info (4) */
                0x00, 0x00,                 /* ASC/ASCQ = No additional sense info */
                0x00,                       /* Field Replaceable Unit Code */
                0x00, 0x00, 0x00            /* Sense Key Specific */
            };
            resp = req_sense; resplen = 18u;
            if (bufsize < resplen) resplen = bufsize;
            break;
        }

        case SCSI_CMD_VERIFY_10:
            /* Verify(10)：BYTCHK=0 就直接 PASS（我们 Flash 写入本身是同步+读回校验逻辑已做） */
            resplen = 0u;
            break;

        default: {
            /* 其他 SCSI：返回 CHECK CONDITION，Sense Key=ILLEGAL REQUEST */
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;   /* int32_t 错误返回 */
        }
    }

    /* TinyUSB 要求：把 resp 拷到 buffer（最多 bufsize）并返回真实有效字节数 */
    if (resp && resplen) {
        if (bufsize < resplen) resplen = bufsize;
        memcpy(buffer, resp, resplen);
    }
    return (int32_t)resplen;
}
