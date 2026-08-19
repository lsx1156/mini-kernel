/**
 * @file    shell_plc.c
 * @brief   RP2350 PLC Shell 命令
 */

#include "shell_core.h"
#include "hal/plc_core.h"
#include "hal/flash_layout.h"
#include "hal/hal_interface.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* PLC Shell 上下文 */
static plc_ctx_t g_plc_ctx;
static bool g_plc_inited = false;
static task_handle_t g_plc_scan_task = NULL;

/* 前向声明 */
static void plc_scan_task_entry(void *arg);

static int cmd_plc_help(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    shell_puts(ctx, "PLC Loop Instruction Commands:\r\n");
    shell_puts(ctx, "  plc help                    - Show this help\r\n");
    shell_puts(ctx, "  plc status                  - Show PLC status, scan stats, memory map\r\n");
    shell_puts(ctx, "  plc run                     - Start PLC cyclic execution\r\n");
    shell_puts(ctx, "  plc stop                    - Stop PLC execution\r\n");
    shell_puts(ctx, "  plc pause                   - Pause PLC (hold state)\r\n");
    shell_puts(ctx, "  plc step                    - Single step one instruction\r\n");
    shell_puts(ctx, "  plc reset                   - Reset PLC (clear non-retain, PC=0)\r\n");
    shell_puts(ctx, "  plc load <slot>             - Load program from Flash slot (0-3)\r\n");
    shell_puts(ctx, "  plc save <slot>             - Save program to Flash slot (0-3)\r\n");
    shell_puts(ctx, "  plc new                     - Create new empty program\r\n");
    shell_puts(ctx, "  plc asm <line>              - Assemble single line (debug)\r\n");
    shell_puts(ctx, "  plc disasm [pc] [count]     - Disassemble program\r\n");
    shell_puts(ctx, "  plc mem <area> [start] [cnt]- View memory (X/Y/M/T/C/S/D/V/R)\r\n");
    shell_puts(ctx, "  plc set <area> <idx> <val>  - Set bit/word variable\r\n");
    shell_puts(ctx, "  plc scan <period_us>        - Set scan period (default 10000us)\r\n");
    shell_puts(ctx, "  plc wdt <timeout_us>        - Set watchdog timeout (default 100000us)\r\n");
    shell_puts(ctx, "  plc cfg <flag> <on|off>     - Config flags: run/pause/wdt/retain/halt\r\n");
    shell_puts(ctx, "\r\nMemory Areas:\r\n");
    shell_puts(ctx, "  X(0-255) Y(0-255) M(0-1023) T(0-127) C(0-127) S(0-127)\r\n");
    shell_puts(ctx, "  D(0-1023) V(0-63) R(0-1023)\r\n");
    shell_puts(ctx, "\r\nInstruction Set (subset):\r\n");
    shell_puts(ctx, "  LD/AND/OR/OUT/SET/RST/PLS/PLF/INV/MEP/MEF\r\n");
    shell_puts(ctx, "  TMR/TMR10/TMR1 CNT/CNT32 DCNT\r\n");
    shell_puts(ctx, "  CMP/ZCP DCMP/DZCP\r\n");
    shell_puts(ctx, "  MOV/MOVP/MOVW/BMOV/FMOV/XCH/SWAP DMOV\r\n");
    shell_puts(ctx, "  ADD/SUB/MUL/DIV INC/DEC DADD/DSUB/DMUL/DDIV DINC/DDEC\r\n");
    shell_puts(ctx, "  WAND/WOR/WXOR NEG ROL/ROR/SHL/SHR/SAL/SAR DROL/DROR/DSHL/DSHR/DSAL/DSAR\r\n");
    shell_puts(ctx, "  JMP/JMPC/JMPNC/CALL FOR/NEXT/BREAK\r\n");
    shell_puts(ctx, "  BCD/BIN PID MODRW/IVCK/IVDR/IVRD/IVWR/ADPRW\r\n");
    shell_puts(ctx, "  LABEL n:\r\n");
    return 0;
}

static int plc_init_if_needed(void) {
    if (!g_plc_inited) {
        if (plc_init(&g_plc_ctx) != PLC_ERR_OK) {
            return -1;
        }
        g_plc_inited = true;
    }
    return 0;
}

static int cmd_plc_status(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    
    char buf[512];
    plc_format_status(&g_plc_ctx, buf, sizeof(buf));
    shell_puts(ctx, buf);
    return 0;
}

static int cmd_plc_run(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    plc_run(&g_plc_ctx);
    
    /* 创建扫描任务（如果不存在） */
    if (!g_plc_scan_task) {
        g_plc_scan_task = task_create("plc_scan", plc_scan_task_entry, NULL, 4096, 2);
        if (!g_plc_scan_task) {
            shell_puts(ctx, "PLC scan task create failed.\r\n");
            plc_stop(&g_plc_ctx);
            return 1;
        }
    } else {
        task_resume(g_plc_scan_task);
    }
    
    shell_puts(ctx, "PLC RUNNING\r\n");
    return 0;
}

static int cmd_plc_stop(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_stop(&g_plc_ctx);
    g_plc_ctx.config_flags &= ~(PLC_CFG_RUN | PLC_CFG_PAUSE);
    if (g_plc_scan_task) task_suspend(g_plc_scan_task);
    shell_puts(ctx, "PLC STOPPED\r\n");
    return 0;
}

static int cmd_plc_pause(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_pause(&g_plc_ctx);
    g_plc_ctx.config_flags &= ~PLC_CFG_RUN;
    g_plc_ctx.config_flags |= PLC_CFG_PAUSE;
    if (g_plc_scan_task) task_suspend(g_plc_scan_task);
    shell_puts(ctx, "PLC PAUSED\r\n");
    return 0;
}

static int cmd_plc_step(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_step(&g_plc_ctx);
    shell_puts(ctx, "PLC STEPPED\r\n");
    return 0;
}

static int cmd_plc_reset(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    plc_reset(&g_plc_ctx);
    g_plc_ctx.config_flags &= ~(PLC_CFG_RUN | PLC_CFG_PAUSE);
    if (g_plc_scan_task) task_suspend(g_plc_scan_task);
    shell_puts(ctx, "PLC RESET\r\n");
    return 0;
}

static int cmd_plc_load(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc load <slot 0-3>\r\n"); return 1; }
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    
    int slot = atoi(argv[1]);
    if (slot < 0 || slot > 3) { shell_puts(ctx, "slot 0-3\r\n"); return 1; }
    
    if (plc_load_program(&g_plc_ctx, slot) == PLC_ERR_OK) {
        shell_printf(ctx, "Loaded slot %d\r\n", slot);
    } else {
        shell_puts(ctx, "Load failed\r\n");
    }
    return 0;
}

static int cmd_plc_save(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc save <slot 0-3>\r\n"); return 1; }
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    
    int slot = atoi(argv[1]);
    if (slot < 0 || slot > 3) { shell_puts(ctx, "slot 0-3\r\n"); return 1; }
    
    if (plc_save_program(&g_plc_ctx, slot) == PLC_ERR_OK) {
        shell_printf(ctx, "Saved slot %d\r\n", slot);
    } else {
        shell_puts(ctx, "Save failed\r\n");
    }
    return 0;
}

static int cmd_plc_new(int argc, char **argv, shell_ctx_t *ctx) {
    (void)argc; (void)argv;
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    plc_new_program(&g_plc_ctx);
    shell_puts(ctx, "New PLC program created (max 2048 insts)\r\n");
    return 0;
}

static int cmd_plc_asm(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc asm <line>\r\n"); return 1; }
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    
    char line[256] = {0};
    for (int i = 1; i < argc; i++) {
        strcat(line, argv[i]);
        if (i < argc - 1) strcat(line, " ");
    }
    
    uint16_t pc = g_plc_ctx.program_len;
    if (plc_assemble_line(&g_plc_ctx, line, &pc) == PLC_ERR_OK) {
        shell_printf(ctx, "Assembled at PC=%u\r\n", pc);
    } else {
        shell_puts(ctx, "Assemble failed\r\n");
    }
    return 0;
}

static int cmd_plc_disasm(int argc, char **argv, shell_ctx_t *ctx) {
    if (plc_init_if_needed()) { shell_puts(ctx, "PLC init failed\r\n"); return 1; }
    
    uint16_t pc = 0;
    uint16_t count = g_plc_ctx.program_len;
    
    if (argc >= 2) pc = atoi(argv[1]);
    if (argc >= 3) count = atoi(argv[2]);
    
    char buf[256];
    for (uint16_t i = 0; i < count && (pc + i) < g_plc_ctx.program_len; i++) {
        plc_disasm_inst(&g_plc_ctx, pc + i, buf, sizeof(buf));
        shell_printf(ctx, "%04X: %s\r\n", pc + i, buf);
    }
    return 0;
}

static int cmd_plc_mem(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc mem <X|Y|M|T|C|S|D|V|R> [start] [count]\r\n"); return 1; }
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    plc_area_e area = PLC_AREA_X;
    char a = toupper(argv[1][0]);
    switch (a) {
        case 'X': area = PLC_AREA_X; break;
        case 'Y': area = PLC_AREA_Y; break;
        case 'M': area = PLC_AREA_M; break;
        case 'T': area = PLC_AREA_T; break;
        case 'C': area = PLC_AREA_C; break;
        case 'S': area = PLC_AREA_S; break;
        case 'D': area = PLC_AREA_D; break;
        case 'V': area = PLC_AREA_V; break;
        case 'R': area = PLC_AREA_R; break;
        default: shell_puts(ctx, "area: X/Y/M/T/C/S/D/V/R\r\n"); return 1;
    }
    
    uint16_t start = (argc >= 3) ? atoi(argv[2]) : 0;
    uint16_t count = (argc >= 4) ? atoi(argv[3]) : 16;
    
    plc_dump_memory(&g_plc_ctx, area, start, count, ctx);
    return 0;
}

static int cmd_plc_set(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 4) { shell_puts(ctx, "usage: plc set <area> <idx> <val>\r\n"); return 1; }
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    plc_area_e area = PLC_AREA_X;
    char a = toupper(argv[1][0]);
    switch (a) {
        case 'X': area = PLC_AREA_X; break;
        case 'Y': area = PLC_AREA_Y; break;
        case 'M': area = PLC_AREA_M; break;
        case 'T': area = PLC_AREA_T; break;
        case 'C': area = PLC_AREA_C; break;
        case 'S': area = PLC_AREA_S; break;
        case 'D': area = PLC_AREA_D; break;
        case 'V': area = PLC_AREA_V; break;
        case 'R': area = PLC_AREA_R; break;
        default: shell_puts(ctx, "area: X/Y/M/T/C/S/D/V/R\r\n"); return 1;
    }
    
    uint16_t idx = atoi(argv[2]);
    int32_t val = atoi(argv[3]);
    
    if (plc_set_value(&g_plc_ctx, area, idx, val) == PLC_ERR_OK) {
        shell_printf(ctx, "Set %c%u = %ld\r\n", a, idx, (long)val);
    } else {
        shell_puts(ctx, "Set failed\r\n");
    }
    return 0;
}

static int cmd_plc_scan(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc scan <period_us>\r\n"); return 1; }
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    uint32_t period = atoi(argv[1]);
    if (period < 1000 || period > 1000000) { shell_puts(ctx, "period 1000~1000000 us\r\n"); return 1; }
    
    g_plc_ctx.scan_period_us = period;
    shell_printf(ctx, "Scan period set to %lu us\r\n", period);
    return 0;
}

static int cmd_plc_wdt(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) { shell_puts(ctx, "usage: plc wdt <timeout_us>\r\n"); return 1; }
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    uint32_t timeout = atoi(argv[1]);
    if (timeout < 10000 || timeout > 10000000) { shell_puts(ctx, "timeout 10000~10000000 us\r\n"); return 1; }
    
    g_plc_ctx.watchdog_timeout_us = timeout;
    shell_printf(ctx, "Watchdog timeout set to %lu us\r\n", timeout);
    return 0;
}

static int cmd_plc_cfg(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 3) { shell_puts(ctx, "usage: plc cfg <flag> <on|off>\r\nflags: run pause wdt retain halt\r\n"); return 1; }
    if (!g_plc_inited) { shell_puts(ctx, "PLC not initialized.\r\n"); return 1; }
    
    uint32_t flag = 0;
    if (strcmp(argv[1], "run") == 0) flag = PLC_CFG_RUN;
    else if (strcmp(argv[1], "pause") == 0) flag = PLC_CFG_PAUSE;
    else if (strcmp(argv[1], "wdt") == 0) flag = PLC_CFG_WDT;
    else if (strcmp(argv[1], "retain") == 0) flag = PLC_CFG_RETAIN;
    else if (strcmp(argv[1], "halt") == 0) flag = PLC_CFG_HALT_ON_ERR;
    else { shell_puts(ctx, "flags: run pause wdt retain halt\r\n"); return 1; }
    
    if (strcmp(argv[2], "on") == 0) g_plc_ctx.config_flags |= flag;
    else if (strcmp(argv[2], "off") == 0) g_plc_ctx.config_flags &= ~flag;
    else { shell_puts(ctx, "on|off\r\n"); return 1; }
    
    shell_puts(ctx, "Config updated\r\n");
    return 0;
}

static const shell_cmd_t plc_cmds[] = {
    {"help",       cmd_plc_help,       "PLC help"},
    {"status",     cmd_plc_status,     "PLC status"},
    {"run",        cmd_plc_run,        "Start PLC"},
    {"stop",       cmd_plc_stop,       "Stop PLC"},
    {"pause",      cmd_plc_pause,      "Pause PLC"},
    {"step",       cmd_plc_step,       "Single step"},
    {"reset",      cmd_plc_reset,      "Reset PLC"},
    {"load",       cmd_plc_load,       "Load from Flash"},
    {"save",       cmd_plc_save,       "Save to Flash"},
    {"new",        cmd_plc_new,        "New program"},
    {"asm",        cmd_plc_asm,        "Assemble line"},
    {"disasm",     cmd_plc_disasm,     "Disassemble"},
    {"mem",        cmd_plc_mem,        "View memory"},
    {"set",        cmd_plc_set,        "Set variable"},
    {"scan",       cmd_plc_scan,       "Set scan period"},
    {"wdt",        cmd_plc_wdt,        "Set watchdog"},
    {"cfg",        cmd_plc_cfg,        "Config flags"},
};

void shell_plc_register(void) {
    for (size_t i = 0; i < sizeof(plc_cmds)/sizeof(plc_cmds[0]); i++) {
        shell_register(&plc_cmds[i]);
    }
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