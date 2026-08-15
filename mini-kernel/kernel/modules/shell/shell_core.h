/**
 * @file    shell_core.h
 * @brief   Shell 对外导出类型 + 扩展 API（新增 shell_register / shell_ctx_t，
 *          以便 shell_fs.c 往同一个 dispatch 表里挂命令）
 *
 *  历史版本（v0.1~v0.2）的 Shell 只有一个 static g_cmd_table[] 静态数组。
 *  v2.2 引入 MSC + FatFs，命令数从 ~20 涨到 ~28，想把 FS 命令放到独立编译单元
 *  而不互相 #include 各自 static 数组，于是新增一个"动态命令扩展表"：
 *    · shell_register(name, fn, usage, help)  → 追加命令（最多 SHELL_DYNAMIC_CMDS 条）
 *    · shell_fs_register()                    → 由 shell.c 在构建完静态表后调用
 *  命令 dispatch：先扫静态表，扫不到再扫动态表（兼容静态表 + 新命令都可用 help）。
 */
#ifndef SHELL_CORE_H
#define SHELL_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Shell 命令回调签名（和 shell.c 里 typedef 保持一致）---------- */
typedef struct shell_ctx_t shell_ctx_t;   /* 透明上下文（当前未使用，保留） */
typedef int (*shell_cmd_fn_ext_t)(int argc, char **argv, shell_ctx_t *ctx);

/* 兼容性：老 shell.c 里命令函数签名没有 ctx 参数（static 内部的）。
 * 扩展 API 统一带 ctx（=NULL 代表默认交互上下文），调用方都写 NULL 即可。*/
struct shell_ctx_t { int dummy; };   /* 占一个字节，避免空结构体 warning */

/* ---------- 扩展 API ---------- */

/** 追加一条新命令到"扩展命令表"，成功返回 true；满了返回 false。 */
int  shell_register(const char *name,
                    shell_cmd_fn_ext_t handler,
                    const char *usage,
                    const char *help);

/** 在扩展命令表中查找：找不到返回 -1；找到返回 index（内部用，也给 shell.c dispatch 用）。*/
int  shell_ext_lookup(const char *name, shell_cmd_fn_ext_t *out_fn,
                      const char **out_usage, const char **out_help);

/** 枚举扩展命令（help 命令打印用）：idx from 0..N-1，false 表示无更多。 */
int  shell_ext_count(void);
int  shell_ext_get(int idx, const char **out_name, shell_cmd_fn_ext_t *out_fn,
                   const char **out_usage, const char **out_help);

/* ---------- Shell 内提供的输出 API（简化：直接用 hal_console_putc；
 *            我们另外提供若干 helper，给 shell_fs.c 等扩展文件共用）---------- */
void    shell_putc(shell_ctx_t *ctx, char c);
void    shell_puts(shell_ctx_t *ctx, const char *s);
int     shell_snprintf(char *buf, size_t bufsz, const char *fmt, ...);

/* ---------- 初始化钩子（由 shell.c 在启动 banner 前调用）---------- */
void shell_fs_register(void);      /* 定义在 shell_fs.c（注册 msc/ls/cd/...） */
void shell_ovclk_register(void);   /* 定义在 shell_ovclk.c（注册 ovclk） */
void shell_mcore_register(void);   /* 定义在 shell_mcore.c（注册 mcore） */

#ifdef __cplusplus
}
#endif

#endif /* SHELL_CORE_H */
