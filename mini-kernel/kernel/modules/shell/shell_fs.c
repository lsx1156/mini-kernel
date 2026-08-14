/**
 * @file    shell_fs.c
 * @brief   v2.2 Shell 文件 / U 盘子命令：
 *              msc mount/eject/status/format
 *              ls / cd / pwd / mkdir / rmdir / rm / cat
 *
 *  【互斥原则】（主机 USB MSC ↔ 本机 Shell 文件操作）：
 *   - `msc mount`  → ejected=false：主机 USB 可读写；Shell 只能"读"（ls/cat），不能写。
 *   - `msc eject`  → ejected=true：主机 USB 显示"无介质"；Shell 可读可写所有命令。
 *   - `msc format` → 强制 ejected=true + f_mkfs（FAT16，1012KB） + remount。
 *   - 首启动自动做 f_mkfs（如果 Flash 空片），完成后 ejected=true（相当于默认 Shell 先独占写）。
 *
 *  【工作目录】Shell 维护一个 s_cwd[256] 记录当前目录（"/" 开头）；
 *   - cd  切换（绝对路径或相对路径）
 *   - ls/pwd/mkdir/rmdir/rm/cat 都先把相对路径拼接到 s_cwd
 *   - 所有命令最终都传绝对路径给 FatFs API
 *
 *  【TCHAR 注意】FF_USE_LFN=0：TCHAR = char，_T(x) = x，_tcslen = strlen 等，
 *  直接用标准 string.h 函数即可，不要调用 Windows 专用 _tcs* 宏（GCC 无）。
 */

#include "shell_core.h"
#include "ff.h"
#include "fatfs_api.h"
#include "msc_blockdev.h"
#include "hal/flash_layout.h"
#include "hal/hal_interface.h"   /* HAL_FLASH_SECTOR_SIZE */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#if OS_CFG_FATFS

/* ================================================================
 *  0. TCHAR 兼容宏（GCC 环境下 _tcs* 不存在，直接映射到标准 C 函数）
 *     无论 FF_USE_LFN 是 0 还是 1，RP2040-GCC 都没有 Windows SDK 的
 *     _tcslen / _tcscpy / _tcsncpy，所以这里无条件定义一组别名。
 *     FF_USE_LFN=0 → TCHAR=char；FF_USE_LFN=1 → TCHAR=WCHAR 也用不到
 *     因为 shell 输入路径是 char，我们的实现只用 char 版本的标准函数。
 * ================================================================ */
#ifndef _tcslen
  #define _tcslen    strlen
#endif
#ifndef _tcscpy
  #define _tcscpy    strcpy
#endif
#ifndef _tcsncpy
  #define _tcsncpy   strncpy
#endif

/* ================================================================
 *  1. CWD 路径拼接辅助
 * ================================================================ */
#define FS_MAX_CWD    255
static TCHAR s_cwd[FS_MAX_CWD + 1] = _T("/");   /* 当前工作目录（绝对，以 / 开头、结尾无 /，除非根） */

static void path_normalize(TCHAR *out, size_t out_sz, const TCHAR *in);

/**
 * @brief  把"绝对/相对路径输入"解析成绝对路径
 * @param  user_path   用户输入路径（可能带 "0:" 前缀；我们会自动去掉）
 * @param  out         输出绝对路径（至少 FS_MAX_CWD + 1 字节）
 * @param  out_sz      输出缓冲区大小
 */
static bool resolve_abs_path(const TCHAR *user_path, TCHAR *out, size_t out_sz) {
    if (!user_path || !out || out_sz < 2) return false;
    TCHAR work[FS_MAX_CWD + 1];
    size_t wi = 0;

    /* 跳过 "0:" 驱动器前缀，若存在 */
    if (user_path[0] && user_path[1] == _T(':')) user_path += 2;
    /* 跳过开头空白 */
    while (*user_path == _T(' ') || *user_path == _T('\t')) user_path++;

    if (*user_path == _T('/') || *user_path == _T('\\')) {
        /* 绝对路径 */
        work[wi++] = _T('/');
        user_path++;
    } else {
        /* 相对路径：复制 s_cwd，末尾补 '/' */
        size_t cwd_len = _tcslen(s_cwd);
        if (cwd_len + 1 > FS_MAX_CWD) return false;
        memcpy(work, s_cwd, cwd_len * sizeof(TCHAR));
        wi = cwd_len;
        if (work[wi - 1] != _T('/')) { work[wi++] = _T('/'); }
    }

    /* 追加 user_path（小心截断） */
    while (*user_path && wi < FS_MAX_CWD) {
        TCHAR c = *user_path++;
        if (c == _T('\\')) c = _T('/');
        work[wi++] = c;
    }
    work[wi] = _T('\0');

    path_normalize(out, out_sz, work);
    return true;
}

/** 路径规范化：合并 "//"、解析 "./" 和 "../" */
static void path_normalize(TCHAR *out, size_t out_sz, const TCHAR *in) {
    if (!out || !out_sz) return;
    size_t oi = 0;
    out[oi++] = _T('/');
    while (*in) {
        const TCHAR *seg_start = in;
        while (*in != _T('/') && *in) in++;
        size_t slen = (size_t)(in - seg_start);
        if (slen == 0u || (slen == 1u && seg_start[0] == _T('.'))) {
            /* 空段或 '.'  → 跳过 */
        } else if (slen == 2u && seg_start[0] == _T('.') && seg_start[1] == _T('.')) {
            /* ".." → 回退一个段；永远不越过 '/' */
            if (oi > 1u) {
                oi--;   /* 去掉末尾 '/' */
                while (oi > 1u && out[oi - 1] != _T('/')) oi--;
            }
        } else {
            if (oi + slen + 1u < out_sz) {
                memcpy(out + oi, seg_start, slen * sizeof(TCHAR));
                oi += slen;
                out[oi++] = _T('/');
            }
        }
        if (*in == _T('/')) in++;   /* 跳过 / */
    }
    if (oi > 1u) oi--;               /* 去掉末尾多余 '/' */
    out[oi] = _T('\0');
}

/* ================================================================
 *  2. FatFs 错误码 → 人类可读字符串
 * ================================================================ */
static const char *fr_str(FRESULT fr) {
    switch (fr) {
        case FR_OK:                return "OK";
        case FR_DISK_ERR:          return "Disk error (Flash I/O)";
        case FR_INT_ERR:           return "Internal error (corrupted BPB/FAT)";
        case FR_NOT_READY:         return "Drive not ready";
        case FR_NO_FILE:           return "No such file";
        case FR_NO_PATH:           return "No such directory";
        case FR_INVALID_NAME:      return "Invalid filename";
        case FR_DENIED:            return "Access denied (read-only dir or file open)";
        case FR_EXIST:             return "Entry already exists";
        case FR_INVALID_OBJECT:    return "Invalid file/dir object";
        case FR_WRITE_PROTECTED:   return "Write protected (host USB busy; do `msc eject` first)";
        case FR_INVALID_DRIVE:     return "Invalid drive";
        case FR_NOT_ENABLED:       return "Volume not mounted";
        case FR_NO_FILESYSTEM:     return "No valid FAT; run `msc format` or re-mount USB";
        case FR_MKFS_ABORTED:      return "f_mkfs aborted (bad parameter)";
        case FR_TIMEOUT:           return "Timeout";
        case FR_LOCKED:            return "File locked";
        case FR_NOT_ENOUGH_CORE:   return "Not enough RAM (LFN alloc failed)";
        case FR_TOO_MANY_OPEN_FILES: return "Too many open files";
        case FR_INVALID_PARAMETER: return "Invalid parameter";
        default:                   return "Unknown FatFs error";
    }
}

/* ================================================================
 *  3. Shell 子命令：msc  （mount / eject / status / format）
 * ================================================================ */
static int cmd_msc(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        shell_puts(ctx,
            "msc: USB Mass Storage control\n"
            "  msc mount   - Mark medium mounted → host can R/W, shell only READ\n"
            "  msc eject   - Mark medium ejected → host sees NOT READY, shell can WRITE\n"
            "  msc status  - Show MSC + FatFs state\n"
            "  msc format  - [DANGER] Rebuild FAT16 (erases ALL files)\n"
        );
        return 0;
    }
    const char *sub = argv[1];

    if (!strcmp(sub, "mount")) {
        if (fatfs_try_enter_write_mode()) {
            /* 当前 ejected=true（允许写），如果用户 mount 到主机 → 切 ejected=false */
        }
        /* FatFs 需要确保 mount。我们可能未 mount（或之前 ejected=true 让 f_open 失败） */
        if (!fatfs_is_mounted()) {
            FRESULT fr;
            /* 临时 ejected=false 以便 mount 真读 BPB */
            bool prev = msc_blockdev_is_ejected();
            msc_blockdev_set_ejected(false);
            fr = fatfs_init_and_mount();
            msc_blockdev_set_ejected(prev);
            if (fr != FR_OK) {
                shell_puts(ctx, "FAIL: mount failed: ");
                shell_puts(ctx, fr_str(fr));
                shell_puts(ctx, "\n  Try `msc format` (irreversibly erases all data)\n");
                return 2;
            }
        }
        msc_blockdev_set_ejected(false);
        shell_puts(ctx, "OK: MSC medium MOUNTED — host USB can now use the drive.\n"
                        "  CAUTION: Shell write commands (mkdir/rm/cp/write) LOCKED until `msc eject`.\n");
        return 0;
    }

    if (!strcmp(sub, "eject")) {
        msc_blockdev_set_ejected(true);
        shell_puts(ctx, "OK: MSC medium EJECTED — host USB will see 'no medium'.\n"
                        "  Shell commands (mkdir/rm/cat/ls) now fully usable.\n");
        return 0;
    }

    if (!strcmp(sub, "status")) {
        shell_puts(ctx, "============================================================\n"
                        "  MSC U盘 + FatFs 状态\n"
                        "============================================================\n");
        /* MSC 分区信息 */
        char line[96];
        shell_puts(ctx, "  Flash Partition (RP2040 W25Q16/2MB):\n");
        shell_snprintf(line, sizeof(line),
            "    · MSC offset    : 0x%06X (%u KiB)\n"
            "    · Sectors       : %u × %u B = %u B (%.1f KiB)\n",
            (unsigned)FLASH_LAYOUT_MSC_OFFSET, (unsigned)(FLASH_LAYOUT_MSC_OFFSET / 1024u),
            (unsigned)FLASH_LAYOUT_MSC_SECTORS, (unsigned)MSC_BLOCKDEV_SECTOR_BYTES,
            (unsigned)FLASH_LAYOUT_MSC_BYTES,
            (double)FLASH_LAYOUT_MSC_BYTES / 1024.0);
        shell_puts(ctx, line);
        shell_snprintf(line, sizeof(line),
            "    · Host-ejected  : %s\n"
            "    · Blank (0xFF)  : %s\n"
            "    · FatFs mounted : %s\n"
            "    · Mkfs this boot: %s\n",
            msc_blockdev_is_ejected() ? "YES (shell R/W exclusive)" : "NO (host USB R/W)",
            msc_blockdev_is_blank() ? "YES (not formatted)" : "NO",
            fatfs_is_mounted() ? "YES" : "NO",
            fatfs_mkfs_done_this_boot() ? "YES (fresh FAT16 created)" : "NO");
        shell_puts(ctx, line);
        shell_puts(ctx, "  Usage hints:\n"
                        "    · To browse via Windows Explorer: `msc mount` then reinsert USB cable.\n"
                        "    · To mkdir/rm/cp files from shell:    `msc eject` first.\n"
                        "    · Both sides see the same Flash data (just cannot write simultaneously).\n"
                        "============================================================\n");
        return 0;
    }

    if (!strcmp(sub, "format")) {
        shell_puts(ctx, "WARNING: This IRREVERSIBLY ERASES the entire data partition.\n"
                        "  Are you SURE? Re-enter the same command to confirm:\n"
                        "      msc format\n"
                        "  (confirmation string stored in RAM; valid until next command)\n");
        /* 两步确认：存储一次 pending */
        static bool s_confirmed = false;
        if (!s_confirmed) { s_confirmed = true; return 0; }
        s_confirmed = false;

        shell_puts(ctx, "Formatting MSC partition as FAT16... ");

        /* 1. 切 ejected=false（允许 disk_write）并执行 f_mkfs 前先 unmount */
        f_unlink(_T("0:")); (void)0;
        { FATFS dummy; memset(&dummy, 0, sizeof(dummy)); f_mount(NULL, _T("0:"), 0); } /* unmount */
        msc_blockdev_set_ejected(false);

        /* 2. f_mkfs */
        BYTE work[512];
        MKFS_PARM opt;
        memset(&opt, 0, sizeof(opt));
        opt.fmt = FM_FAT;
        opt.n_fat = 2;
        opt.align = HAL_FLASH_SECTOR_SIZE / MSC_BLOCKDEV_SECTOR_BYTES;
        opt.n_root = 128;
        opt.au_size = 0;
        FRESULT fr = f_mkfs(_T("0:"), &opt, work, sizeof(work));
        if (fr != FR_OK) {
            shell_puts(ctx, "FAIL: "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n');
            msc_blockdev_set_ejected(true);
            return 3;
        }

        /* 3. Remount + 强制 ejected=true（Shell 独占模式） */
        msc_blockdev_set_ejected(false);
        FATFS *pf = fatfs_get_obj();
        fr = f_mount(pf, _T("0:"), 1);
        msc_blockdev_set_ejected(true);   /* 格式化完 → Shell 先独占 */
        if (fr != FR_OK) {
            shell_puts(ctx, "OK (formatted) but remount FAIL: ");
            shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n');
            return 4;
        }
        /* 重置 s_cwd 为 "/" */
        _tcscpy(s_cwd, _T("/"));
        shell_puts(ctx, "OK. Fresh FAT16 created; shell exclusive (ejected=true).\n"
                        "  Run `msc mount` to let host see the drive.\n");
        return 0;
    }

    shell_puts(ctx, "Unknown msc subcommand: '");
    shell_puts(ctx, sub);
    shell_puts(ctx, "' (try msc with no args)\n");
    return 1;
}

/* ================================================================
 *  4. Shell 文件 / 目录命令  （基于 FatFs，路径统一走 resolve_abs_path）
 * ================================================================ */

static int cmd_pwd(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    shell_puts(ctx, s_cwd);
    shell_putc(ctx, '\n');
    return 0;
}

static int cmd_cd(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { s_cwd[0] = _T('/'); s_cwd[1] = 0; return 0; }
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(argv[1], abs, sizeof(abs))) {
        shell_puts(ctx, "cd: path too long\n");
        return 1;
    }
    /* validate: try f_opendir */
    FRESULT fr;
    bool was_eject = msc_blockdev_is_ejected();
    if (!was_eject) msc_blockdev_set_ejected(true);   /* 临时允许读介质状态 */
    DIR d;
    fr = f_opendir(&d, abs);
    if (!was_eject) msc_blockdev_set_ejected(false);
    if (fr == FR_OK) {
        f_closedir(&d);
        _tcsncpy(s_cwd, abs, FS_MAX_CWD); s_cwd[FS_MAX_CWD] = 0;
        return 0;
    }
    shell_puts(ctx, "cd: ");
    shell_puts(ctx, fr_str(fr));
    shell_puts(ctx, " ("); shell_puts(ctx, abs); shell_puts(ctx, ")\n");
    return 2;
}

static int cmd_ls(int argc, char **argv, shell_ctx_t *ctx) {
    const TCHAR *target = (argc >= 2) ? argv[1] : s_cwd;
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(target, abs, sizeof(abs))) {
        shell_puts(ctx, "ls: path too long\n"); return 1;
    }

    /* ls 总是只读：即使 ejected=false 也允许（读不破坏） */
    DIR d;
    FILINFO fi;
    FRESULT fr;
    bool was_eject = msc_blockdev_is_ejected();
    if (!was_eject) msc_blockdev_set_ejected(true);
    fr = f_opendir(&d, abs);
    if (!was_eject) msc_blockdev_set_ejected(false);
    if (fr != FR_OK) {
        shell_puts(ctx, "ls: ");
        shell_puts(ctx, fr_str(fr));
        shell_puts(ctx, " ("); shell_puts(ctx, abs); shell_puts(ctx, ")\n");
        return 2;
    }

    char line[96];
    shell_puts(ctx, abs); shell_puts(ctx, ":\n");
    int total = 0;
    for (;;) {
        if (!was_eject) msc_blockdev_set_ejected(true);
        fr = f_readdir(&d, &fi);
        if (!was_eject) msc_blockdev_set_ejected(false);
        if (fr != FR_OK || fi.fname[0] == 0) break;
        if (!total++) shell_puts(ctx, "  Mode  Size     Date    Time    Name\n"
                                      "  ----  -------  ------  ------  --------------------\n");
        const char *mode = (fi.fattrib & AM_DIR) ? "DIR" : "   ";
        shell_snprintf(line, sizeof(line), "  %-3s  %7lu  %02u/%02u/%02u  %02u:%02u  %s\n",
            mode, (unsigned long)fi.fsize,
            ((unsigned)((fi.fdate >> 5u) & 0xFu)),    /* month */
            ((unsigned)(fi.fdate & 0x1Fu)),             /* day */
            ((unsigned)((fi.fdate >> 9u) & 0x7Fu) + 1980u), /* year */
            ((unsigned)((fi.ftime >> 11u) & 0x1Fu)),    /* hour */
            ((unsigned)((fi.ftime >> 5u) & 0x3Fu)),     /* min */
            fi.fname);
        shell_puts(ctx, line);
    }
    if (!was_eject) msc_blockdev_set_ejected(true);
    f_closedir(&d);
    if (!was_eject) msc_blockdev_set_ejected(false);
    if (!total) shell_puts(ctx, "  (empty directory)\n");
    return 0;
}

static int cmd_mkdir(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "mkdir <dirname>\n"); return 1; }
    if (!fatfs_try_enter_write_mode()) {
        shell_puts(ctx, "mkdir: Write locked. Run `msc eject` first to prevent USB+Shell collision.\n");
        return 1;
    }
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(argv[1], abs, sizeof(abs))) { shell_puts(ctx, "mkdir: path too long\n"); return 1; }
    FRESULT fr = f_mkdir(abs);
    if (fr != FR_OK) { shell_puts(ctx, "mkdir: "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n'); return 2; }
    return 0;
}

static int cmd_rmdir(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "rmdir <dir> (must be empty)\n"); return 1; }
    if (!fatfs_try_enter_write_mode()) {
        shell_puts(ctx, "rmdir: Write locked. Run `msc eject` first.\n"); return 1;
    }
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(argv[1], abs, sizeof(abs))) { shell_puts(ctx, "rmdir: path too long\n"); return 1; }
    FRESULT fr = f_unlink(abs);
    if (fr != FR_OK) { shell_puts(ctx, "rmdir: "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n'); return 2; }
    return 0;
}

static int cmd_rm(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "rm <file-or-dir> (dir must be empty)\n"); return 1; }
    if (!fatfs_try_enter_write_mode()) {
        shell_puts(ctx, "rm: Write locked. Run `msc eject` first.\n"); return 1;
    }
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(argv[1], abs, sizeof(abs))) { shell_puts(ctx, "rm: path too long\n"); return 1; }
    FRESULT fr = f_unlink(abs);
    if (fr != FR_OK) { shell_puts(ctx, "rm: "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n'); return 2; }
    return 0;
}

/* cat：简单把文件内容原样输出到终端；二进制文件会有乱码（未过滤） */
static int cmd_cat(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "cat <file>\n"); return 1; }
    TCHAR abs[FS_MAX_CWD + 1];
    if (!resolve_abs_path(argv[1], abs, sizeof(abs))) { shell_puts(ctx, "cat: path too long\n"); return 1; }
    /* cat 只读，任意 ejected 状态 OK */
    FIL f;
    FRESULT fr;
    bool was_eject = msc_blockdev_is_ejected();
    if (!was_eject) msc_blockdev_set_ejected(true);
    fr = f_open(&f, abs, FA_READ);
    if (!was_eject) msc_blockdev_set_ejected(false);
    if (fr != FR_OK) { shell_puts(ctx, "cat: "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n'); return 2; }
    char buf[256];
    UINT got;
    for (;;) {
        if (!was_eject) msc_blockdev_set_ejected(true);
        fr = f_read(&f, buf, sizeof(buf), &got);
        if (!was_eject) msc_blockdev_set_ejected(false);
        if (fr != FR_OK || got == 0) break;
        for (UINT i = 0; i < got; i++) shell_putc(ctx, buf[i]);
    }
    if (!was_eject) msc_blockdev_set_ejected(true);
    f_close(&f);
    if (!was_eject) msc_blockdev_set_ejected(false);
    if (fr != FR_OK) { shell_puts(ctx, "cat: read error "); shell_puts(ctx, fr_str(fr)); shell_putc(ctx, '\n'); return 3; }
    return 0;
}

/* ================================================================
 *  5. 命令表（由 shell 主模块调用 shell_fs_register）
 *  shell_register 签名：4 参数 name, handler, usage, help
 * ================================================================ */
void shell_fs_register(void) {
    shell_register("msc",   cmd_msc,   "msc mount|eject|status|format",   "USB Mass Storage control + partition format");
    shell_register("pwd",   cmd_pwd,   "pwd",                              "Print current directory");
    shell_register("cd",    cmd_cd,    "cd [path]",                        "Change current directory (abs or rel, default /)");
    shell_register("ls",    cmd_ls,    "ls [path]",                        "List directory contents (root . if no arg)");
    shell_register("mkdir", cmd_mkdir, "mkdir <dir>",                      "Create directory (needs `msc eject` first)");
    shell_register("rmdir", cmd_rmdir, "rmdir <dir>",                      "Remove EMPTY directory (needs `msc eject`)");
    shell_register("rm",    cmd_rm,    "rm <file|empty-dir>",              "Delete file or empty dir (needs `msc eject`)");
    shell_register("cat",   cmd_cat,   "cat <file>",                       "Print file content (no binary filter)");
}

#else /* OS_CFG_FATFS == 0 */
void shell_fs_register(void) { /* stub */ }
#endif /* OS_CFG_FATFS */
