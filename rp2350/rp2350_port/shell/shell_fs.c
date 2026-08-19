/**
 * @file    shell_fs.c
 * @brief   RP2350 Shell 文件系统命令 (ls/cd/pwd/mkdir/rmdir/rm/cat)
 */

#include "shell_core.h"
#include "hal_port.h"
#include "msc_blockdev.h"
#include <ff.h>
#include <string.h>
#include <stdio.h>

static FATFS g_fs;
static bool g_fs_mounted = false;
static char g_cwd[256] = "/";

static FRESULT mount_fs(void) {
    if (g_fs_mounted) return FR_OK;
    FRESULT res = f_mount(&g_fs, "0:", 1);
    if (res == FR_OK) g_fs_mounted = true;
    return res;
}

static FRESULT unmount_fs(void) {
    if (!g_fs_mounted) return FR_OK;
    FRESULT res = f_unmount("0:");
    if (res == FR_OK) g_fs_mounted = false;
    return res;
}

static void cmd_ls(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    DIR dir;
    FILINFO fno;
    FRESULT res = f_opendir(&dir, g_cwd);
    if (res != FR_OK) { shell_puts(ctx, "opendir failed\r\n"); return; }
    
    shell_puts(ctx, "  Size     Type Name\r\n");
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        char type = (fno.fattrib & AM_DIR) ? 'D' : 'F';
        shell_printf(ctx, "%10lu %c %s\r\n", (unsigned long)fno.fsize, type, fno.fname);
    }
    f_closedir(&dir);
}

static void cmd_cd(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: cd <path>\r\n"); return; }
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    char path[256];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path)-1);
    } else {
        if (strcmp(g_cwd, "/") == 0) {
            snprintf(path, sizeof(path), "/%s", argv[1]);
        } else {
            snprintf(path, sizeof(path), "%s/%s", g_cwd, argv[1]);
        }
    }
    path[sizeof(path)-1] = '\0';
    
    DIR dir;
    FRESULT res = f_opendir(&dir, path);
    if (res == FR_OK) {
        f_closedir(&dir);
        strncpy(g_cwd, path, sizeof(g_cwd)-1);
        g_cwd[sizeof(g_cwd)-1] = '\0';
        shell_printf(ctx, "cwd: %s\r\n", g_cwd);
    } else {
        shell_puts(ctx, "no such directory\r\n");
    }
}

static void cmd_pwd(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    shell_printf(ctx, "%s\r\n", g_cwd);
}

static void cmd_mkdir(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: mkdir <path>\r\n"); return; }
    if (!g_msc_ejected) { shell_puts(ctx, "eject first: msc eject\r\n"); return; }
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    FRESULT res = f_mkdir(argv[1]);
    if (res == FR_OK) shell_puts(ctx, "ok\r\n");
    else shell_printf(ctx, "failed: %d\r\n", res);
}

static void cmd_rmdir(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: rmdir <path>\r\n"); return; }
    if (!g_msc_ejected) { shell_puts(ctx, "eject first: msc eject\r\n"); return; }
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    FRESULT res = f_unlink(argv[1]);
    if (res == FR_OK) shell_puts(ctx, "ok\r\n");
    else shell_printf(ctx, "failed: %d\r\n", res);
}

static void cmd_rm(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: rm <file>\r\n"); return; }
    if (!g_msc_ejected) { shell_puts(ctx, "eject first: msc eject\r\n"); return; }
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    FRESULT res = f_unlink(argv[1]);
    if (res == FR_OK) shell_puts(ctx, "ok\r\n");
    else shell_printf(ctx, "failed: %d\r\n", res);
}

static void cmd_cat(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: cat <file>\r\n"); return; }
    if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
    
    FIL fil;
    FRESULT res = f_open(&fil, argv[1], FA_READ);
    if (res != FR_OK) { shell_puts(ctx, "open failed\r\n"); return; }
    
    char buf[256];
    UINT br;
    while ((res = f_read(&fil, buf, sizeof(buf)-1, &br)) == FR_OK && br > 0) {
        buf[br] = '\0';
        shell_puts(ctx, buf);
    }
    f_close(&fil);
}

static void cmd_msc(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        shell_puts(ctx, "msc <eject|mount|format>\r\n");
        return;
    }
    
    if (strcmp(argv[1], "eject") == 0) {
        unmount_fs();
        msc_blockdev_set_ejected(true);
        shell_puts(ctx, "ejected\r\n");
    } else if (strcmp(argv[1], "mount") == 0) {
        msc_blockdev_set_ejected(false);
        shell_puts(ctx, "mounted to host\r\n");
    } else if (strcmp(argv[1], "format") == 0) {
        msc_blockdev_set_ejected(true);
        if (mount_fs() != FR_OK) { shell_puts(ctx, "mount failed\r\n"); return; }
        MKFS_PARM opt = { FM_FAT16, 0, 0, 0, 0 };
        BYTE work[FF_MAX_SS];
        FRESULT res = f_mkfs("0:", &opt, work, sizeof(work));
        if (res == FR_OK) shell_puts(ctx, "formatted FAT16\r\n");
        else shell_printf(ctx, "format failed: %d\r\n", res);
    } else {
        shell_puts(ctx, "usage: msc <eject|mount|format>\r\n");
    }
}

static const shell_cmd_t fs_cmds[] = {
    {"ls",   cmd_ls,   "list files"},
    {"cd",   cmd_cd,   "change directory"},
    {"pwd",  cmd_pwd,  "print working directory"},
    {"mkdir", cmd_mkdir, "make directory"},
    {"rmdir", cmd_rmdir, "remove directory"},
    {"rm",   cmd_rm,   "remove file"},
    {"cat",  cmd_cat,  "show file content"},
    {"msc",  cmd_msc,  "msc eject|mount|format"},
};

void shell_fs_register(void) {
    for (size_t i = 0; i < sizeof(fs_cmds)/sizeof(fs_cmds[0]); i++) {
        shell_register(&fs_cmds[i]);
    }
}