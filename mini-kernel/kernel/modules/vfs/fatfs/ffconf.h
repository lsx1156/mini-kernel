/**
 * @file    ffconf.h
 * @brief   FatFs R0.15 完整配置（FFCONF_DEF = 80286，匹配 SDK ff.h FF_DEFINED）
 *
 *  【FFCONF_DEF 必须 == FF_DEFINED 80286】
 *  SDK 的 ff.h 里写着：`#if FF_DEFINED != FFCONF_DEF → #error Wrong config file`。
 *  因此这里 guard 宏名必须是 FFCONF_DEF，值必须 = 80286（FatFs R0.15 版本号）。
 *
 *  【设计取舍：RP2040 + 1012KB MSC U 盘】
 *    - 读/写/删 文件，创建/删除/切换 目录
 *    - FAT16 格式化 (f_mkfs) + 自动挂载
 *    - 只 8.3 短名（FF_USE_LFN=0，最省 RAM）
 *    - 代码体积 & RAM 尽量裁剪，不影响内核运行
 */
#ifndef FFCONF_DEF
#define FFCONF_DEF 80286

#include "os_config.h"

/* 给 ff.h 的 "ff.h → ffconf.h" 检查用：ffconf.h 被正确找到。
 * 若预处理时出现 "ffconf.h not found"，这行的 define 会留在预处理器里，
 * 后续 #ifdef 可诊断。 */
#define _FFCONF_LOADED_OK_  1

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* This option switches read-only configuration. (0:Read/Write or 1:Read-only)
/  Read-only configuration removes writing API functions, f_write(), f_sync(),
/  f_unlink(), f_mkdir(), f_chmod(), f_rename(), f_truncate(), f_getfree()
/  and optional writing functions as well. */


#define FF_FS_MINIMIZE	0
/* This option defines minimization level to remove some basic API functions.
/
/   0: Basic functions are fully enabled.
/   1: f_stat(), f_getfree(), f_unlink(), f_mkdir(), f_truncate() and f_rename()
/      are removed.
/   2: f_opendir(), f_readdir() and f_closedir() are removed in addition to 1.
/   3: f_lseek() function is removed in addition to 2. */


#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	1
#define FF_STRF_ENCODE	3
/* This option switches string functions, f_gets(), f_putc(), f_puts() and f_printf().
/
/  0: Disable string functions.
/  1: Enable without LF-CRLF conversion.
/  2: Enable with LF-CRLF conversion. */


#define FF_USE_FIND		0
/* This option switches filtered directory read functions, f_findfirst() and
/  f_findnext(). (0:Disable, 1:Enable 2:Enable with matching altname[] too) */


#define FF_USE_MKFS		1
/* This option switches f_mkfs() function. (0:Disable or 1:Enable)
/  Mini Kernel v2.2：必须启用——首启动空片时自动 f_mkfs 建 FAT16。*/


#define FF_USE_FASTSEEK	0
/* This option switches fast seek function. (0:Disable or 1:Enable) */


#define FF_USE_EXPAND	0
/* This option switches f_expand function. (0:Disable or 1:Enable) */


#define FF_USE_CHMOD	0
/* This option switches attribute manipulation functions, f_chmod() and f_utime().
/  (0:Disable or 1:Enable) Also FF_FS_READONLY needs to be 0 to enable this option. */


#define FF_USE_LABEL	0
/* This option switches volume label functions, f_getlabel() and f_setlabel().
/  (0:Disable or 1:Enable) */


#define FF_USE_FORWARD	0
/* This option switches f_forward() function. (0:Disable or 1:Enable) */


/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* This option specifies the OEM code page to be used on the target system.
/  Incorrect code page setting can cause a file open failure.
/
/   437 - U.S. (OEM U.S.，英文+标准符号；省 LFN 转换表 RAM)
/   932 - Japanese (Shift-JIS)
/   936 - Simplified Chinese (GBK) — 需要额外 c936.c 约 180KB 码表，v2.2 默认不用。
/   949 - Korean
/   950 - Traditional Chinese (Big5)
*/


#define FF_USE_LFN		0
#define FF_MAX_LFN		64
/* The FF_USE_LFN switches the support for LFN (long file name).
/
/   0: Disable LFN. FF_MAX_LFN has no effect.
/   1: Enable LFN with static working buffer on the BSS. Always NOT thread-safe.
/   2: Enable LFN with dynamic working buffer on the STACK.
/   3: Enable LFN with dynamic working buffer on the HEAP.
/
/  Mini Kernel v2.2 默认关 LFN（=0）：最省 RAM、避免 kmalloc；Windows
/  写文件时会生成长名，但 FatFs 只要读到短名 8.3 条目即可列出目录。*/


#define FF_LFN_UNICODE	0
/* This option switches the character encoding on the API when LFN is enabled.
/
/   0: ANSI/OEM in current CP (TCHAR = char)
/   1: Unicode in UTF-16 (TCHAR = WCHAR)
/   2: Unicode in UTF-8 (TCHAR = char)
/   3: Unicode in UTF-32 (TCHAR = DWORD)
*/


#define FF_LFN_BUF		64
#define FF_SFN_BUF		12
/* This set of options defines size of file name members in the FILINFO structure
/  which is used to read out directory items. These values should be suffcient for
/  the file names to read. The maximum possible length of the read file name depends
/  on character encoding. When LFN is not enabled, these options have no effect. */


#define FF_FS_RPATH		2
/* This option configures support for relative path.
/
/   0: Disable relative path and ignore built-in functions.
/   1: Enable relative path.
/   2: Enable relative path and f_chdir / f_chdrive functions are also added.
/  Mini Kernel v2.2 需要 cd（f_chdir），所以设为 2。*/


#define FF_VOLUMES		1
/* Number of volumes (logical drives) to be used. (1-10)
/  只有一个物理 Flash MSC 分区。*/


#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
/* FF_STR_VOLUME_ID switches support for volume ID in arbitrary strings.
/  When FF_STR_VOLUME_ID is set to 1 or 2, arbitrary strings can be used as drive
/  number in the path name. FF_VOLUME_STRS specifies the valid strings.
/  不使用字符串卷 ID。*/


#define FF_MULTI_PARTITION	0
/* This option switches support for multiple volumes on the physical drive.
/  不支持分区表：整个 MSC 分区就是一个 FAT16 卷。*/


#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* This set of options configures the range of sector size to be supported. (512,
/  1024, 2048 or 4096) Always set both 512 for most systems, generic memory card and
/  harddisk. But a larger value may be required for on-board flash memory and some
/  type of optical media. When FF_MAX_SS is larger than FF_MIN_SS, FatFs is configured
/  for variable sector size mode and disk_ioctl() function needs to implement
/  GET_SECTOR_SIZE command.
/  MSC 标准：固定 512B/sector。*/


#define FF_LBA64		0
/* This option switches support for 64-bit LBA. (0:Disable or 1:Enable)
/  仅 1012KB 盘，32-bit LBA 绰绰有余。*/


#define FF_MIN_GPT		0x10000000
/* Threshold size in sectors for switching from MBR to GPT in f_mkfs. When
/  FF_LBA64 == 0, this option has no effect. */


#define FF_USE_TRIM		0
/* This option switches support for ATA-TRIM. (0:Disable or 1:Enable)
/  无需 Trim：W25Q16JV 是 NOR Flash，不支持 TRIM。*/


/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* This option switches tiny buffer configuration. (0:Normal or 1:Tiny)
/  0 = 每个 FILE 对象有独立扇区缓冲（性能更好），RP2040 RAM 够用。*/


#define FF_FS_EXFAT		0
/* This option switches support for exFAT filesystem. (0:Disable or 1:Enable)
/  只用到 <1MB 盘，FAT16 足矣；exFAT 还需要额外 EXFAT 库约 20KB。*/


#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2024
/* 不启用 RTC（无硬件时钟），文件时间戳默认为 2024-01-01。
/  启用 NORTC=1 可省 ~500B 时间转换代码。*/


#define FF_FS_NOFSINFO	0
/* If you need to know correct free space on the FAT32 volume, set bit 0 of this
/  option and f_getfree() function at first time after volume mount will force
/  a full FAT scan. Bit 1 controls the use of last allocated cluster number.
/
/  bit0=0: Use free cluster count in the FSINFO if available.
/  bit0=1: Do not trust free cluster count in the FSINFO.
/  bit1=0: Use last allocated cluster number in the FSINFO if available.
/  bit1=1: Do not trust last allocated cluster number in the FSINFO.
*/


#define FF_FS_LOCK		0
/* The option FF_FS_LOCK switches file lock function to control duplicated file open
/  and illegal operation to open objects. This option must be 0 when FF_FS_READONLY is 1.
/  简单 shell：不会并发打开同一文件读写，设为 0 省 RAM。*/


/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
#define FF_SYNC_t		HANDLE
/* 不启用重入：Shell 单线程跑命令；U 盘写是 MSC USB 中断/任务上下文，但
/  ejected 状态互斥已经保证 USB 可写时 Shell 禁止写，Shell 可写时 USB 无法写。
/  因此不存在竞争，无需重入锁。（FF_SYNC_t 定义仅当 _REENTRANT=1 时用，此处随便写）*/


#define FF_FS_WORD_ACCESS	0
/* 0=字节访问（最通用；RP2040 是小端 flash 对齐也可以设 1，但保守 0 最稳）*/


/*---------------------------------------------------------------------------/
/ Debug Configuration
/---------------------------------------------------------------------------*/

#define FF_DBG_FILE		0
/* 关闭 FatFs 内部 debug printf；Shell 输出通过 shell_puts() 自行处理。*/

#endif /* FFCONF_DEF */
