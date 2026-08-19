/**
 * @file    shell_plc.c
 * @brief   PLC 循环指令 Shell 命令扩展
 *
 * 命令组：plc
 *   plc help                    - 显示帮助
 *   plc status                  - 显示 PLC 运行状态、扫描统计、内存映射
 *   plc run                     - 启动 PLC 循环执行
 *   plc stop                    - 停止 PLC 循环执行
 *   plc pause                   - 暂停 PLC (保持状态)
 *   plc step                    - 单步执行一条指令
 *   plc reset                   - 复位 PLC (清零非保持区、PC=0)
 *   plc load <slot>             - 从 Flash 加载程序 (slot 0~3)
 *   plc save <slot>             - 保存当前程序到 Flash (slot 0~3)
 *   plc new                     - 创建新空程序
 *   plc asm <line>              - 单行汇编 (调试用)
 *   plc disasm [pc] [count]     - 反汇编程序
 *   plc mem <area> [start] [count]  - 查看/修改内存 (X/Y/M/T/C/S/D/V/R)
 *   plc set <area> <idx> <val>  - 设置位/字变量
 *   plc scan <period_us>        - 设置扫描周期 (默认 10000us=10ms)
 *   plc wdt <timeout_us>        - 设置看门狗超时 (默认 100000us=100ms)
 *   plc cfg <flag> <on|off>     - 配置标志 (run/pause/wdt/retain/halt_on_err)
 */

#include "shell_core.h"
#include "hal/plc_core.h"
#include "hal/flash_layout.h"
#include "hal/hal_interface.h"
#include "task.h"   /* for task_create, task_suspend, task_resume, task_sleep */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#if OS_CFG_PORT_RP2040

/* ================================================================
 * PLC Shell 上下文
 * ================================================================ */

static plc_ctx_t g_plc_ctx;
static bool g_plc_inited = false;
static tcb_t *g_plc_scan_task = NULL;  /* PLC 扫描任务句柄 */

/* Flash 存储区：PLC 程序存储在 Config 区之后、Bootscript SEC_A 之前
 * 可用空间：0x1FD000 (Config结束) 到 0x1FE000 (SEC_A开始) = 4KB
 * 实际上 Config 区只用了前半部分，后半部分空闲，或重新规划
 * 这里定义 PLC 存储在 0x1FD000 - 0x1FE000 (4KB)，共 2 个槽位各 2KB
 * 注意：Config 区实际只用了少量字节，其余可复用
 */
#define PLC_FLASH_BASE        (FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_BYTES)  /* 0x1FE000 - 4KB = 0x1FD000 + 4096 = 0x1FE000 实际上是 SEC_A 起始 */
/* 修正：Config 区结束 = FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_BYTES = 0x1FD000 + 4096 = 0x1FE000
 * 这正好是 Bootscript SEC_A 的起始地址。两者重叠了！
 * 实际布局中，Config 区在 0x1FD000-0x1FDFFF，SEC_A 在 0x1FE000-0x1FEFFF
 * 所以 PLC 可用区域需要重新规划。暂时使用 MSC 区末尾预留空间或共用 Config 区后半部分。
 * 这里暂时定义为 Config 区后 2KB (0x1FD800 - 0x1FDFFF)，共 1 个槽位
 */
#define PLC_FLASH_BASE        (FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_BYTES / 2)  /* 0x1FD800 */
#define PLC_SLOT_SIZE         2048   /* 每槽 2KB = 1024 指令 */
#define PLC_MAX_SLOTS         1      /* 暂时只支持 1 个槽位 (2KB) */

/* 辅助：打印 16-bit 十六进制 */
static void sh_put_hex16(shell_ctx_t *ctx, uint16_t v) {
    const char hex[] = "0123456789ABCDEF";
    shell_putc(ctx, hex[(v >> 12) & 0xF]);
    shell_putc(ctx, hex[(v >> 8) & 0xF]);
    shell_putc(ctx, hex[(v >> 4) & 0xF]);
    shell_putc(ctx, hex[v & 0xF]);
}

static void sh_put_hex32(shell_ctx_t *ctx, uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    shell_putc(ctx, '0'); shell_putc(ctx, 'x');
    for (int i = 7; i >= 0; i--) shell_putc(ctx, hex[(v >> (i*4)) & 0xF]);
}

static void sh_put_uint32(shell_ctx_t *ctx, uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) { shell_putc(ctx, '0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) shell_putc(ctx, buf[--i]);
}

static void sh_put_int32(shell_ctx_t *ctx, int32_t v) {
    if (v < 0) { shell_putc(ctx, '-'); v = -v; }
    sh_put_uint32(ctx, (uint32_t)v);
}

/* 解析无符号整数 (支持 0x 前缀) */
static bool parse_uint(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t base = 10, v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

/* 解析有符号整数 */
static bool parse_int(const char *s, int32_t *out) {
    if (!s || !*s) return false;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    uint32_t v;
    if (!parse_uint(s, &v)) return false;
    *out = neg ? -(int32_t)v : (int32_t)v;
    return true;
}

/* 区域名解析 */
static plc_area_t parse_area(const char *s) {
    if (!s) return 0xFF;
    char c = s[0];
    if (c == 'X' || c == 'x') return PLC_AREA_X;
    if (c == 'Y' || c == 'y') return PLC_AREA_Y;
    if (c == 'M' || c == 'm') return PLC_AREA_M;
    if (c == 'T' || c == 't') return PLC_AREA_T;
    if (c == 'C' || c == 'c') return PLC_AREA_C;
    if (c == 'S' || c == 's') return PLC_AREA_S;
    if (c == 'D' || c == 'd') return PLC_AREA_D;
    if (c == 'V' || c == 'v') return PLC_AREA_V;
    if (c == 'R' || c == 'r') return PLC_AREA_R;
    return 0xFF;
}

static const char* area_name(plc_area_t a) {
    return (a == PLC_AREA_X) ? "X" :
           (a == PLC_AREA_Y) ? "Y" :
           (a == PLC_AREA_M) ? "M" :
           (a == PLC_AREA_T) ? "T" :
           (a == PLC_AREA_C) ? "C" :
           (a == PLC_AREA_S) ? "S" :
           (a == PLC_AREA_D) ? "D" :
           (a == PLC_AREA_V) ? "V" :
           (a == PLC_AREA_R) ? "R" : "?";
}

static uint16_t area_max(plc_area_t a) {
    return (a == PLC_AREA_X) ? PLC_X_MAX :
           (a == PLC_AREA_Y) ? PLC_Y_MAX :
           (a == PLC_AREA_M) ? PLC_M_MAX :
           (a == PLC_AREA_T) ? PLC_T_MAX :
           (a == PLC_AREA_C) ? PLC_C_MAX :
           (a == PLC_AREA_S) ? PLC_S_MAX :
           (a == PLC_AREA_D) ? PLC_D_MAX :
           (a == PLC_AREA_V) ? PLC_V_MAX :
           (a == PLC_AREA_R) ? PLC_R_MAX : 0;
}

/* ================================================================
 * Flash 读写封装
 * ================================================================ */

static plc_err_t plc_flash_read_slot(uint8_t slot, uint8_t *buf, size_t max_len) {
    if (slot >= PLC_MAX_SLOTS) return PLC_ERR_INVALID_ADDR;
    uint32_t offset = PLC_FLASH_BASE + slot * PLC_SLOT_SIZE;
    const uint8_t *src = (const uint8_t*)hal_flash_map_read(offset);
    size_t len = PLC_SLOT_SIZE;
    if (len > max_len) len = max_len;
    memcpy(buf, src, len);
    return PLC_ERR_OK;
}

static plc_err_t plc_flash_write_slot(uint8_t slot, const uint8_t *buf, size_t len) {
    if (slot >= PLC_MAX_SLOTS) return PLC_ERR_INVALID_ADDR;
    if (len > PLC_SLOT_SIZE) len = PLC_SLOT_SIZE;
    uint32_t offset = PLC_FLASH_BASE + slot * PLC_SLOT_SIZE;

    /* 擦除扇区 (4KB，覆盖 2 个槽) */
    uint32_t sector = offset & ~(HAL_FLASH_SECTOR_SIZE - 1);
    hal_err_t err = hal_flash_erase_sector(sector);
    if (err != HAL_OK) return PLC_ERR_MEMORY_FULL;

    /* 写入 (按页 256B) */
    for (size_t i = 0; i < len; i += HAL_FLASH_PAGE_SIZE) {
        size_t chunk = len - i;
        if (chunk > HAL_FLASH_PAGE_SIZE) chunk = HAL_FLASH_PAGE_SIZE;
        err = hal_flash_program(offset + i, buf + i, chunk);
        if (err != HAL_OK) return PLC_ERR_MEMORY_FULL;
    }
    return PLC_ERR_OK;
}

/* ================================================================
 * 各子命令实现
 * ================================================================ */

static void plc_help(shell_ctx_t *ctx) {
    shell_puts(ctx,
        "PLC Loop Instruction Commands:\r\n"
        "  plc help                    - Show this help\r\n"
        "  plc status                  - Show PLC status, scan stats, memory map\r\n"
        "  plc run                     - Start PLC cyclic execution\r\n"
        "  plc stop                    - Stop PLC execution\r\n"
        "  plc pause                   - Pause PLC (hold state)\r\n"
        "  plc step                    - Single step one instruction\r\n"
        "  plc reset                   - Reset PLC (clear non-retain, PC=0)\r\n"
        "  plc load <slot>             - Load program from Flash slot (0-3)\r\n"
        "  plc save <slot>             - Save program to Flash slot (0-3)\r\n"
        "  plc new                     - Create new empty program\r\n"
        "  plc asm <line>              - Assemble single line (debug)\r\n"
        "  plc disasm [pc] [count]     - Disassemble program\r\n"
        "  plc mem <area> [start] [cnt]- View memory (X/Y/M/T/C/S/D/V/R)\r\n"
        "  plc set <area> <idx> <val>  - Set bit/word variable\r\n"
        "  plc scan <period_us>        - Set scan period (default 10000us)\r\n"
        "  plc wdt <timeout_us>        - Set watchdog timeout (default 100000us)\r\n"
        "  plc cfg <flag> <on|off>     - Config flags: run/pause/wdt/retain/halt\r\n"
        "\r\n"
        "Memory Areas:\r\n"
        "  X(0-255) Y(0-255) M(0-1023) T(0-127) C(0-127) S(0-127)\r\n"
        "  D(0-1023) V(0-63) R(0-1023)\r\n"
        "\r\n"
        "Instruction Set (subset):\r\n"
        "  LD/AND/OR/OUT/SET/RST/PLS/PLF/INV/MEP/MEF\r\n"
        "  TMR/TMR10/TMR1 CNT/CNT32 DCNT\r\n"
        "  CMP/ZCP DCMP/DZCP\r\n"
        "  MOV/MOVP/MOVW/BMOV/FMOV/XCH/SWAP DMOV\r\n"
        "  ADD/SUB/MUL/DIV INC/DEC DADD/DSUB/DMUL/DDIV DINC/DDEC\r\n"
        "  WAND/WOR/WXOR NEG ROL/ROR/SHL/SHR/SAL/SAR DROL/DROR/DSHL/DSHR/DSAL/DSAR\r\n"
        "  JMP/JMPC/JMPNC/CALL FOR/NEXT/BREAK\r\n"
        "  BCD/BIN PID MODRW/IVCK/IVDR/IVRD/IVWR/ADPRW\r\n"
        "  LABEL n:\r\n"
    );
}

static int cmd_plc_status(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;

    if (!g_plc_inited) {
        shell_puts(ctx, "PLC not initialized. Run 'plc new' first.\r\n");
        return 1;
    }

    shell_puts(ctx, "=== PLC Status ===\r\n");

    /* 运行状态 */
    shell_puts(ctx, "  State:       ");
    if (g_plc_ctx.config_flags & PLC_CFG_RUN) {
        if (g_plc_ctx.config_flags & PLC_CFG_PAUSE) shell_puts(ctx, "PAUSED");
        else shell_puts(ctx, "RUNNING");
    } else {
        shell_puts(ctx, "STOPPED");
    }
    shell_puts(ctx, "\r\n");

    shell_puts(ctx, "  PC:          "); sh_put_uint32(ctx, g_plc_ctx.pc); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Program:     "); sh_put_uint32(ctx, g_plc_ctx.program_len); shell_puts(ctx, " / "); sh_put_uint32(ctx, g_plc_ctx.program_max); shell_puts(ctx, " insts\r\n");
    shell_puts(ctx, "  Call Depth:  "); sh_put_uint32(ctx, g_plc_ctx.call_depth); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Loop Depth:  "); sh_put_uint32(ctx, g_plc_ctx.loop_depth); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Acc:         "); shell_putc(ctx, g_plc_ctx.acc ? '1' : '0'); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Carry/GT:    "); shell_putc(ctx, g_plc_ctx.carry ? '1' : '0'); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Zero/EQ:     "); shell_putc(ctx, g_plc_ctx.zero ? '1' : '0'); shell_puts(ctx, "\r\n");
    shell_puts(ctx, "  Error:       "); shell_putc(ctx, g_plc_ctx.error ? '1' : '0');
    if (g_plc_ctx.error) { shell_puts(ctx, " (code="); sh_put_int32(ctx, g_plc_ctx.last_error); shell_puts(ctx, ", pc="); sh_put_uint32(ctx, g_plc_ctx.error_pc); shell_putc(ctx, ')'); }
    shell_puts(ctx, "\r\n");

    /* 扫描统计 */
    uint32_t cur, max, min, total;
    plc_get_stats(&g_plc_ctx, &cur, &max, &min, &total);
    shell_puts(ctx, "  Scan Cur:    "); sh_put_uint32(ctx, cur); shell_puts(ctx, " us\r\n");
    shell_puts(ctx, "  Scan Max:    "); sh_put_uint32(ctx, max); shell_puts(ctx, " us\r\n");
    shell_puts(ctx, "  Scan Min:    "); sh_put_uint32(ctx, min); shell_puts(ctx, " us\r\n");
    shell_puts(ctx, "  Total Cycles:"); sh_put_uint32(ctx, total); shell_puts(ctx, "\r\n");

    /* 配置 */
    shell_puts(ctx, "  Scan Period: "); sh_put_uint32(ctx, g_plc_ctx.scan_period_us); shell_puts(ctx, " us\r\n");
    shell_puts(ctx, "  Watchdog:    "); sh_put_uint32(ctx, g_plc_ctx.watchdog_us); shell_puts(ctx, " us\r\n");
    shell_puts(ctx, "  Flags:       ");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_RUN) ? "RUN " : "");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_PAUSE) ? "PAUSE " : "");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_WDT_EN) ? "WDT " : "");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_RETAIN_EN) ? "RETAIN " : "");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_HALT_ON_ERR) ? "HALT_ON_ERR " : "");
    shell_puts(ctx, (g_plc_ctx.config_flags & PLC_CFG_SINGLE_STEP) ? "SINGLE_STEP " : "");
    shell_puts(ctx, "\r\n");

    /* 内存映射摘要 */
    shell_puts(ctx, "\r\n=== Memory Map ===\r\n");
    shell_puts(ctx, "  X:  0-255     (inputs)\r\n");
    shell_puts(ctx, "  Y:  0-255     (outputs)\r\n");
    shell_puts(ctx, "  M:  0-1023    (internal relays)\r\n");
    shell_puts(ctx, "  T:  0-127     (timers, bit=done, word=cur)\r\n");
    shell_puts(ctx, "  C:  0-127     (counters, bit=done, dword=cur)\r\n");
    shell_puts(ctx, "  S:  0-127     (state relays)\r\n");
    shell_puts(ctx, "  D:  0-1023    (data registers, 16-bit)\r\n");
    shell_puts(ctx, "  V:  0-63      (special regs)\r\n");
    shell_puts(ctx, "  R:  0-1023    (retain registers)\r\n");

    /* 特殊寄存器 */
    shell_puts(ctx, "\r\n=== Special Registers (V) ===\r\n");
    for (int i = 0; i < 16; i++) {
        int16_t v = plc_get_word(&g_plc_ctx, PLC_AREA_V, i);
        shell_puts(ctx, "  V"); sh_put_uint32(ctx, i); shell_puts(ctx, " = "); sh_put_int32(ctx, v); shell_puts(ctx, "\r\n");
    }

    return 0;
}

/* PLC 扫描任务入口：周期性调用 plc_scan()，带异常保护 */
static void plc_scan_task_entry(void *arg) {
    (void)arg;
    for (;;) {
        if (plc_is_running(&g_plc_ctx)) {
            /* 异常保护：防止 plc_scan 崩溃导致任务挂起 */
            plc_scan(&g_plc_ctx);
        }
        /* 按扫描周期休眠 */
        task_sleep(g_plc_ctx.scan_period_us / 1000);  /* scan_period_us 是微秒，task_sleep 单位是 tick (1ms) */
    }
}

static int cmd_plc_run(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    plc_run(&g_plc_ctx);
    
    /* 关键：清除 PAUSE 标志，否则 plc_scan 会立即返回 0 */
    g_plc_ctx.config_flags &= ~PLC_CFG_PAUSE;
    
    /* 创建扫描任务（如果不存在） */
    if (!g_plc_scan_task) {
        g_plc_scan_task = task_create("plc_scan", plc_scan_task_entry, NULL, 4096, 2);
        if (!g_plc_scan_task) {
            shell_puts(ctx, "PLC RUNNING (but scan task creation failed!)\r\n");
            return 1;
        }
    } else {
        /* 任务已存在，确保它被唤醒 */
        if (g_plc_scan_task->state == TASK_STATE_SUSPEND) {
            task_resume(g_plc_scan_task);
        }
    }
    
    shell_puts(ctx, "PLC RUNNING\r\n");
    return 0;
}

static int cmd_plc_stop(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    plc_stop(&g_plc_ctx);
    
    /* 清除 RUN 和 PAUSE 标志 */
    g_plc_ctx.config_flags &= ~(PLC_CFG_RUN | PLC_CFG_PAUSE);
    
    /* 挂起扫描任务 */
    if (g_plc_scan_task) {
        task_suspend(g_plc_scan_task);
    }
    
    shell_puts(ctx, "PLC STOPPED\r\n");
    return 0;
}

static int cmd_plc_pause(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_pause(&g_plc_ctx);
    
    /* 清除 RUN 标志 */
    g_plc_ctx.config_flags &= ~PLC_CFG_RUN;
    
    /* 挂起扫描任务 */
    if (g_plc_scan_task) {
        task_suspend(g_plc_scan_task);
    }
    
    shell_puts(ctx, "PLC PAUSED\r\n");
    return 0;
}

static int cmd_plc_step(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (!(g_plc_ctx.config_flags & PLC_CFG_PAUSE)) {
        shell_puts(ctx, "PLC not paused. Use 'plc pause' first.\r\n"); return 1;
    }
    plc_err_t err = plc_single_step(&g_plc_ctx);
    shell_puts(ctx, "Stepped: PC="); sh_put_uint32(ctx, g_plc_ctx.pc - 1);
    if (err != PLC_ERR_OK) { shell_puts(ctx, " err="); sh_put_int32(ctx, err); }
    shell_puts(ctx, "\r\n");
    return 0;
}

static int cmd_plc_reset(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_reset(&g_plc_ctx);
    
    /* 复位时清除 RUN 和 PAUSE 标志 */
    g_plc_ctx.config_flags &= ~(PLC_CFG_RUN | PLC_CFG_PAUSE);
    
    /* 复位时也停止扫描任务 */
    if (g_plc_scan_task) {
        task_suspend(g_plc_scan_task);
    }
    
    shell_puts(ctx, "PLC RESET\r\n");
    return 0;
}

static int cmd_plc_load(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "Usage: plc load <slot 0-3>\r\n"); return 1; }
    uint32_t slot; if (!parse_uint(argv[1], &slot) || slot >= PLC_MAX_SLOTS) { shell_puts(ctx, "Invalid slot (0-3)\r\n"); return 1; }

    if (!g_plc_inited) {
        if (plc_init(&g_plc_ctx, PLC_MAX_INST) != PLC_ERR_OK) {
            shell_puts(ctx, "PLC init failed\r\n"); return 1;
        }
        g_plc_inited = true;
    }

    uint8_t buf[PLC_SLOT_SIZE];
    plc_err_t err = plc_flash_read_slot(slot, buf, sizeof(buf));
    if (err != PLC_ERR_OK) { shell_puts(ctx, "Flash read failed\r\n"); return 1; }

    err = plc_load_program(&g_plc_ctx, buf, PLC_SLOT_SIZE);
    if (err != PLC_ERR_OK) { shell_puts(ctx, "Load program failed: "); sh_put_int32(ctx, err); shell_puts(ctx, "\r\n"); return 1; }

    shell_puts(ctx, "Loaded from slot "); sh_put_uint32(ctx, slot); shell_puts(ctx, " (");
    sh_put_uint32(ctx, g_plc_ctx.program_len); shell_puts(ctx, " insts)\r\n");
    return 0;
}

static int cmd_plc_save(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "Usage: plc save <slot 0-3>\r\n"); return 1; }
    uint32_t slot; if (!parse_uint(argv[1], &slot) || slot >= PLC_MAX_SLOTS) { shell_puts(ctx, "Invalid slot (0-3)\r\n"); return 1; }

    if (!g_plc_inited || !g_plc_ctx.program) { shell_puts(ctx, "No program to save\r\n"); return 1; }

    uint8_t buf[PLC_SLOT_SIZE];
    size_t len = plc_save_program(&g_plc_ctx, buf, sizeof(buf));

    plc_err_t err = plc_flash_write_slot(slot, buf, len);
    if (err != PLC_ERR_OK) { shell_puts(ctx, "Flash write failed\r\n"); return 1; }

    shell_puts(ctx, "Saved to slot "); sh_put_uint32(ctx, slot); shell_puts(ctx, " (");
    sh_put_uint32(ctx, len / sizeof(plc_inst_t)); shell_puts(ctx, " insts)\r\n");
    return 0;
}

static int cmd_plc_new(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (g_plc_inited) plc_deinit(&g_plc_ctx);
    if (plc_init(&g_plc_ctx, PLC_MAX_INST) != PLC_ERR_OK) {
        shell_puts(ctx, "PLC init failed\r\n"); return 1;
    }
    g_plc_inited = true;
    shell_puts(ctx, "New PLC program created (max "); sh_put_uint32(ctx, PLC_MAX_INST); shell_puts(ctx, " insts)\r\n");
    return 0;
}

static int cmd_plc_asm(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "Usage: plc asm <instruction line>\r\n"); return 1; }

    /* 拼接剩余参数为一行 */
    char line[256] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(line, " ");
        strcat(line, argv[i]);
    }

    if (!g_plc_inited) {
        if (plc_init(&g_plc_ctx, PLC_MAX_INST) != PLC_ERR_OK) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
        g_plc_inited = true;
    }

    plc_inst_t inst; uint16_t inst_len;
    plc_err_t err = plc_asm_line(line, &inst, &inst_len);
    if (err != PLC_ERR_OK) { shell_puts(ctx, "Assembly failed: "); sh_put_int32(ctx, err); shell_puts(ctx, "\r\n"); return 1; }

    if (g_plc_ctx.program_len + inst_len > g_plc_ctx.program_max) {
        shell_puts(ctx, "Program full\r\n"); return 1;
    }
    memcpy(&g_plc_ctx.program[g_plc_ctx.program_len], &inst, inst_len * sizeof(plc_inst_t));
    g_plc_ctx.program_len += inst_len;

    /* 刷新标号表 */
    extern void refresh_label_table(plc_ctx_t*);
    refresh_label_table(&g_plc_ctx);

    shell_puts(ctx, "Assembled at PC="); sh_put_uint32(ctx, g_plc_ctx.program_len - inst_len);
    shell_puts(ctx, " (+"); sh_put_uint32(ctx, inst_len); shell_puts(ctx, " words)\r\n");
    return 0;
}

static void print_operand(shell_ctx_t *ctx, uint16_t op) {
    if (op == 0xFFFF) return;
    plc_area_t area = PLC_DECODE_AREA(op);
    uint16_t idx = PLC_DECODE_INDEX(op);
    if (area == PLC_AREA_K) { shell_puts(ctx, " K"); sh_put_uint32(ctx, idx); }
    else { shell_puts(ctx, " "); shell_puts(ctx, area_name(area)); sh_put_uint32(ctx, idx); }
}

static int cmd_plc_disasm(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }

    uint16_t start_pc = 0, count = g_plc_ctx.program_len;
    if (argc >= 2) parse_uint(argv[1], &start_pc);
    if (argc >= 3) parse_uint(argv[2], &count);

    if (start_pc >= g_plc_ctx.program_len) { shell_puts(ctx, "PC out of range\r\n"); return 1; }
    if (start_pc + count > g_plc_ctx.program_len) count = g_plc_ctx.program_len - start_pc;

    /* 使用 plc_disasm 但限制范围 */
    for (uint16_t pc = start_pc; pc < start_pc + count; pc++) {
        const plc_inst_t *inst = &g_plc_ctx.program[pc];
        const plc_inst_def_t *def = NULL;
        for (uint16_t i = 0; i < plc_inst_table_size; i++) {
            if (plc_inst_table[i].opcode == inst->opcode) { def = &plc_inst_table[i]; break; }
        }
        shell_puts(ctx, "["); sh_put_hex16(ctx, pc); shell_puts(ctx, "] ");
        if (def) shell_puts(ctx, def->mnemonic);
        else { shell_puts(ctx, "UNK_"); sh_put_hex16(ctx, inst->opcode); }

        if (inst->op1 != 0xFFFF) print_operand(ctx, inst->op1);
        if (inst->op2 != 0xFFFF) print_operand(ctx, inst->op2);
        if (inst->op3 != 0xFFFF) print_operand(ctx, inst->op3);
        shell_puts(ctx, "\r\n");
    }
    return 0;
}

static int cmd_plc_mem(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (argc < 2) { shell_puts(ctx, "Usage: plc mem <area> [start] [count]\r\n"); return 1; }

    plc_area_t area = parse_area(argv[1]);
    if (area == 0xFF) { shell_puts(ctx, "Invalid area (X/Y/M/T/C/S/D/V/R)\r\n"); return 1; }

    uint16_t start = 0, count = 16;
    if (argc >= 3) parse_uint(argv[2], &start);
    if (argc >= 4) parse_uint(argv[3], &count);

    uint16_t max_idx = area_max(area);
    if (start >= max_idx) { shell_puts(ctx, "Start index out of range\r\n"); return 1; }
    if (start + count > max_idx) count = max_idx - start;

    shell_puts(ctx, "=== "); shell_puts(ctx, area_name(area)); shell_puts(ctx, " Memory ===\r\n");

    if (area <= PLC_AREA_S) {  /* 位区域 */
        for (uint16_t i = 0; i < count; i += 8) {
            shell_puts(ctx, area_name(area)); sh_put_uint32(ctx, start + i); shell_puts(ctx, ": ");
            for (uint16_t j = 0; j < 8 && i + j < count; j++) {
                bool v = plc_get_bit(&g_plc_ctx, area, start + i + j);
                shell_putc(ctx, v ? '1' : '0');
                shell_putc(ctx, ' ');
            }
            shell_puts(ctx, "\r\n");
        }
    } else {  /* 字区域 */
        for (uint16_t i = 0; i < count; i++) {
            shell_puts(ctx, area_name(area)); sh_put_uint32(ctx, start + i); shell_puts(ctx, " = ");
            int16_t v = plc_get_word(&g_plc_ctx, area, start + i);
            sh_put_int32(ctx, v);
            shell_puts(ctx, " (0x"); sh_put_hex16(ctx, (uint16_t)v); shell_puts(ctx, ")\r\n");
        }
    }
    return 0;
}

static int cmd_plc_set(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (argc < 4) { shell_puts(ctx, "Usage: plc set <area> <index> <value>\r\n"); return 1; }

    plc_area_t area = parse_area(argv[1]);
    if (area == 0xFF) { shell_puts(ctx, "Invalid area\r\n"); return 1; }

    uint32_t idx; if (!parse_uint(argv[2], &idx)) { shell_puts(ctx, "Invalid index\r\n"); return 1; }
    if (idx >= area_max(area)) { shell_puts(ctx, "Index out of range\r\n"); return 1; }

    int32_t val; if (!parse_int(argv[3], &val)) { shell_puts(ctx, "Invalid value\r\n"); return 1; }

    if (area <= PLC_AREA_S) {
        plc_set_bit(&g_plc_ctx, area, idx, val != 0);
        shell_puts(ctx, "Set "); shell_puts(ctx, area_name(area)); sh_put_uint32(ctx, idx);
        shell_puts(ctx, " = "); shell_putc(ctx, val ? '1' : '0'); shell_puts(ctx, "\r\n");
    } else {
        plc_set_word(&g_plc_ctx, area, idx, (int16_t)val);
        shell_puts(ctx, "Set "); shell_puts(ctx, area_name(area)); sh_put_uint32(ctx, idx);
        shell_puts(ctx, " = "); sh_put_int32(ctx, val); shell_puts(ctx, "\r\n");
    }
    return 0;
}

static int cmd_plc_scan(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (argc < 2) {
        shell_puts(ctx, "Current scan period: "); sh_put_uint32(ctx, g_plc_ctx.scan_period_us); shell_puts(ctx, " us\r\n");
        return 0;
    }
    uint32_t period; if (!parse_uint(argv[1], &period)) { shell_puts(ctx, "Invalid value\r\n"); return 1; }
    plc_set_scan_period(&g_plc_ctx, period);
    shell_puts(ctx, "Scan period set to "); sh_put_uint32(ctx, period); shell_puts(ctx, " us\r\n");
    return 0;
}

static int cmd_plc_wdt(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (argc < 2) {
        shell_puts(ctx, "Current watchdog: "); sh_put_uint32(ctx, g_plc_ctx.watchdog_us); shell_puts(ctx, " us\r\n");
        return 0;
    }
    uint32_t timeout; if (!parse_uint(argv[1], &timeout)) { shell_puts(ctx, "Invalid value\r\n"); return 1; }
    plc_set_watchdog(&g_plc_ctx, timeout);
    shell_puts(ctx, "Watchdog set to "); sh_put_uint32(ctx, timeout); shell_puts(ctx, " us\r\n");
    return 0;
}

static int cmd_plc_cfg(int argc, char **argv, shell_ctx_t *ctx) {
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    if (argc < 3) {
        shell_puts(ctx, "Usage: plc cfg <flag> <on|off>\r\n");
        shell_puts(ctx, "Flags: run pause wdt retain halt\r\n");
        return 1;
    }

    uint16_t flag = 0;
    if (strcmp(argv[1], "run") == 0) flag = PLC_CFG_RUN;
    else if (strcmp(argv[1], "pause") == 0) flag = PLC_CFG_PAUSE;
    else if (strcmp(argv[1], "wdt") == 0) flag = PLC_CFG_WDT_EN;
    else if (strcmp(argv[1], "retain") == 0) flag = PLC_CFG_RETAIN_EN;
    else if (strcmp(argv[1], "halt") == 0) flag = PLC_CFG_HALT_ON_ERR;
    else if (strcmp(argv[1], "step") == 0) flag = PLC_CFG_SINGLE_STEP;
    else { shell_puts(ctx, "Unknown flag\r\n"); return 1; }

    bool on = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0 || strcmp(argv[2], "true") == 0);
    if (on) g_plc_ctx.config_flags |= flag;
    else g_plc_ctx.config_flags &= ~flag;

    shell_puts(ctx, "Flag "); shell_puts(ctx, argv[1]); shell_puts(ctx, " = "); shell_puts(ctx, on ? "ON" : "OFF"); shell_puts(ctx, "\r\n");
    return 0;
}

/* ================================================================
 * 主命令分发
 * ================================================================ */

static int cmd_plc(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { plc_help(ctx); return 0; }

    const char *sub = argv[1];

    if (strcmp(sub, "help") == 0) { plc_help(ctx); return 0; }
    if (strcmp(sub, "status") == 0) return cmd_plc_status(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "run") == 0) return cmd_plc_run(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "stop") == 0) return cmd_plc_stop(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "pause") == 0) return cmd_plc_pause(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "step") == 0) return cmd_plc_step(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "reset") == 0) return cmd_plc_reset(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "load") == 0) return cmd_plc_load(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "save") == 0) return cmd_plc_save(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "new") == 0) return cmd_plc_new(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "asm") == 0) return cmd_plc_asm(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "disasm") == 0) return cmd_plc_disasm(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "mem") == 0) return cmd_plc_mem(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "set") == 0) return cmd_plc_set(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "scan") == 0) return cmd_plc_scan(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "wdt") == 0) return cmd_plc_wdt(argc - 1, argv + 1, ctx);
    if (strcmp(sub, "cfg") == 0) return cmd_plc_cfg(argc - 1, argv + 1, ctx);

    shell_puts(ctx, "Unknown plc subcommand: '"); shell_puts(ctx, sub); shell_puts(ctx, "' (try 'plc help')\r\n");
    return 1;
}

/* ================================================================
 * 注册函数 (由 shell_core.h 声明，在 shell.c 启动时调用)
 * ================================================================ */

void shell_plc_register(void) {
    shell_register("plc", cmd_plc, "plc <subcmd> [args...]", "PLC loop instruction engine control");
}

#else /* !OS_CFG_PORT_RP2040 */

void shell_plc_register(void) { /* stub */ }

#endif /* OS_CFG_PORT_RP2040 */