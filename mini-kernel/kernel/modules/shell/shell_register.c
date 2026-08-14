/**
 * @file    shell_register.c
 * @brief   Shell 动态命令扩展表实现 + shell_* 输出 helper
 *          （v2.2 为了把 FS/MSC 命令拆到 shell_fs.c 而不改动 shell.c 的静态表太多）
 */
#include "shell_core.h"
#include "hal/hal_interface.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define SHELL_DYNAMIC_CMDS_MAX   32

typedef struct {
    const char           *name;
    shell_cmd_fn_ext_t   handler;
    const char           *usage;
    const char           *help;
} ext_entry_t;

static ext_entry_t s_ext[SHELL_DYNAMIC_CMDS_MAX];
static int         s_ext_count = 0;

int shell_register(const char *name,
                   shell_cmd_fn_ext_t handler,
                   const char *usage,
                   const char *help) {
    if (s_ext_count >= SHELL_DYNAMIC_CMDS_MAX) return 0;
    /* 重名检查 */
    for (int i = 0; i < s_ext_count; i++) {
        if (strcmp(s_ext[i].name, name) == 0) return 0;
    }
    ext_entry_t *e = &s_ext[s_ext_count++];
    e->name = name; e->handler = handler; e->usage = usage; e->help = help;
    return 1;
}

int shell_ext_lookup(const char *name, shell_cmd_fn_ext_t *out_fn,
                     const char **out_usage, const char **out_help) {
    for (int i = 0; i < s_ext_count; i++) {
        if (strcmp(s_ext[i].name, name) == 0) {
            if (out_fn)    *out_fn    = s_ext[i].handler;
            if (out_usage) *out_usage = s_ext[i].usage;
            if (out_help)  *out_help  = s_ext[i].help;
            return i;
        }
    }
    return -1;
}

int shell_ext_count(void) { return s_ext_count; }

int shell_ext_get(int idx, const char **out_name, shell_cmd_fn_ext_t *out_fn,
                  const char **out_usage, const char **out_help) {
    if (idx < 0 || idx >= s_ext_count) return 0;
    if (out_name)  *out_name  = s_ext[idx].name;
    if (out_fn)    *out_fn    = s_ext[idx].handler;
    if (out_usage) *out_usage = s_ext[idx].usage;
    if (out_help)  *out_help  = s_ext[idx].help;
    return 1;
}

/* ---------- 输出 helper（直接走 hal_console_putc，避免依赖 shell.c static 内部）---------- */
void shell_putc(shell_ctx_t *ctx, char c)           { (void)ctx; hal_console_putc(c); }
void shell_puts(shell_ctx_t *ctx, const char *s)    { (void)ctx; while (*s) hal_console_putc(*s++); }

int  shell_snprintf(char *buf, size_t bufsz, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, bufsz, fmt, ap);   /* Pico SDK 提供 vsnprintf（nanolib/nano-specs 版够用） */
    va_end(ap);
    if (n < 0) return 0;
    if ((size_t)n >= bufsz) n = (int)(bufsz - 1u);
    return n;
}
