/**
 * @file    shell.c
 * @brief   Mini Kernel 交互式命令行 Shell（运行在内核任务中）
 *
 *  特性：
 *    · 非阻塞 getc 拼行，配合 USB CDC / UART0 双通道 SDK stdio
 *    · 支持 \b 退格、\r\n 行结束、清屏命令
 *    · 命令表驱动，易于扩展
 *    · 命令：help / ps / heap / tick / version / suspend / resume / kill / clear / led / syscalls
 *
 *  注意：本文件由 kernel/modules/shell/ 下的 CMake add_module_if_enabled
 *        扫描并编入，裁剪宏 OS_CFG_SHELL=0 时整体不编译（链接零开销）。
 */
#include "task.h"
#include "mem.h"
#include "hal_interface.h"
#include "os_config.h"
#include "syscall_contract.h"
#include "shell_core.h"        /* v2.2 扩展命令注册 API（shell_register/shell_ext_*）*/
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Pico SDK API 前向声明（shell 模块无 SDK 头文件路径） */
#define PICO_ERROR_TIMEOUT ((int)((unsigned int)-1))
extern int getchar_timeout_us(uint32_t timeout_us);

/* USB 纯轮询驱动由 HAL 层统一提供 hal_usb_poll()：
 * 内部做条件式 INTE 恢复 + dcd_int_handler + tud_task_ext + EP1 OUT 搬运。
 * hal_console_getc 内部已经调用 _usb_force_poll，所以 shell 主循环
 * 不再单独 poll，只补一个 CDC IN flush，确保打印字符立刻到达主机。 */
extern void hal_usb_poll(void);
extern void tud_cdc_n_write_flush(uint8_t itf);
extern bool tud_cdc_n_available(uint8_t itf);
extern uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize);
static inline void _tud_flush_only(void) {
    tud_cdc_n_write_flush(0);            /* 仅刷新 CDC IN 发送缓冲区 */
}

/* 对外导出符号声明（满足 -Wmissing-prototypes；裁剪关闭时下面也有空桩） */
void shell_start(void);

#if OS_CFG_SHELL

/* bootscript 固化子系统（放在文件最开头，避免先引用后定义 + include 位置冲突） */
#include "bootscript.h"

/* 版本字符串（由 demo_app.c 提供的公共实现） */
extern const char *k_version(void);

/* ================================================================
 * Shell 基本常量
 * ================================================================ */
#define SHELL_LINE_SIZE       128   /* 单条命令最大长度 */
#define SHELL_MAX_ARGS        16    /* 最大参数个数（含命令本身）。
                                     *   之前 10 会静默截断复合子命令（如 i2c cmds
                                     *   0 0x3C + 8 字节 = 12 token）导致末尾参数丢失
                                     *   且无任何报错。16 足够 I2C/SPI/UART 长命令。 */
#define SHELL_PROMPT_STR      "mk> "
#define SHELL_TASK_NAME       "shell"
#define SHELL_TASK_STACK      2048  /* cmd_i2c fill/rd 用 256B 局部缓冲 + I2C 驱动调用 + 中断栈帧，768 会溢出破坏 g_task_pool */
#define SHELL_TASK_WEIGHT     1

/* ================================================================
 * 控制台输出辅助（避免依赖 k_printf 的完整实现）
 *   注意：函数名加 sh_ 前缀，避免和 shell_core.h 中扩展 API
 *   sh_putc(shell_ctx_t*, char) 发生"同名但签名不同"的类型冲突。
 * ================================================================ */
static void sh_putc(char c)           { hal_console_putc(c); }
static void sh_puts(const char *s)    { while (*s) hal_console_putc(*s++); }
static void sh_crlf(void)             { sh_puts("\r\n"); }

/* ================================================================
 * readline 风格输入行保护（v2.2.5 新增）
 *
 *   问题：后台任务（如 vtest 的 VT3）输出会打断用户正在输入的命令行，
 *         导致用户看到 `mk> vtest st[VT3] Round #10` 混在一起，
 *         容易输错（如把 `v` 误输成 `/v`）。
 *
 *   解决：shell_async_enter() 清当前行 → 后台输出 → shell_async_exit() 恢复
 *         `mk> ` + 用户已输入的字符。效果类似 Linux readline。
 *
 *   用法：
 *     shell_async_enter();
 *     sh_puts("...后台输出...\r\n");
 *     shell_async_exit();
 * ================================================================ */
static char g_shell_line[SHELL_LINE_SIZE];
static volatile int g_shell_pos = 0;
/* 【v2.2.10 修复】async 互斥锁：防止多个后台任务同时调用
 *   shell_async_enter/exit 导致 g_shell_pos/g_shell_line 状态损坏。
 *   例如 VT2 和 VT3 同时输出会互相覆盖 readline 状态 → 崩溃。*/
static volatile int g_shell_async_busy = 0;

static void shell_async_enter(void) {
    /* 自旋等待其他任务的 async 块完成（单核时间片调度，等待期间
     *   会被切走，让持锁任务有机会执行 exit 释放锁）。*/
    while (g_shell_async_busy) {
        task_sleep(1);   /* 让出 CPU，等持锁任务退出 async 块 */
    }
    g_shell_async_busy = 1;
    __asm volatile ("" ::: "memory");
    /* \r 回到行首 + VT100 \033[K 清除当前行 */
    hal_console_putc('\r');
    hal_console_putc(0x1B); hal_console_putc('['); hal_console_putc('K');
}

static void shell_async_exit(void) {
    /* 恢复提示符 + 用户已输入的内容 */
    sh_puts(SHELL_PROMPT_STR);
    int pos = g_shell_pos;
    for (int i = 0; i < pos && i < SHELL_LINE_SIZE; i++) {
        hal_console_putc(g_shell_line[i]);
    }
    __asm volatile ("" ::: "memory");
    g_shell_async_busy = 0;
}

static void shell_put_uint32(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) { hal_console_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) hal_console_putc(buf[--i]);
}

static void shell_put_hex32(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    hal_console_putc('0'); hal_console_putc('x');
    for (int i = 7; i >= 0; i--) hal_console_putc(hex[(v >> (i*4)) & 0xF]);
}

static void shell_pad_spaces(int n) { while (n-- > 0) hal_console_putc(' '); }

/* 把 hal_err_t / int 错误码打印成 "HAL_ERR_IO(-7)" 的可读形式；无对应名字则打印数值。
 *   这样用户看到的就不是 4294967289 这种 uint 截断的大数了。 */
static void shell_put_err(int err) {
    const char *name = NULL;
    switch (err) {
        case  0: name = "HAL_OK"; break;
        case -1: name = "HAL_ERR";      break;
        case -2: name = "HAL_ERR_INVAL";break;
        case -3: name = "HAL_ERR_NOMEM";break;
        case -4: name = "HAL_ERR_BUSY"; break;
        case -5: name = "HAL_ERR_TIMEOUT"; break;
        case -6: name = "HAL_ERR_NOTSUP";  break;
        case -7: name = "HAL_ERR_IO";      break;
        case -8: name = "HAL_ERR_PARAM";   break;
        case -9: name = "HAL_ERR_FULL";    break;
        default: break;
    }
    if (name) {
        sh_puts(name);
        hal_console_putc('(');
    }
    /* 有符号十进制：先出负号再绝对值 */
    if (err < 0) { hal_console_putc('-'); err = -err; }
    shell_put_uint32((uint32_t)err);
    if (name) hal_console_putc(')');
}

static const char *task_state_str(task_state_t s) {
    switch (s) {
        case TASK_STATE_READY:   return "READY";
        case TASK_STATE_RUNNING: return "RUNNING";
        case TASK_STATE_SLEEP:   return "SLEEP";
        case TASK_STATE_SUSPEND: return "SUSPEND";
        case TASK_STATE_DEAD:    return "DEAD";
        default:                 return "?";
    }
}

/* ================================================================
 * 命令处理函数签名 & 命令表结构体
 * ================================================================ */
typedef int (*shell_cmd_fn_t)(int argc, char **argv);
typedef struct {
    const char     *name;
    shell_cmd_fn_t  handler;
    const char     *usage;
    const char     *help;
} shell_cmd_t;

/* 别名：g_cmd_table 里引用 cmd_save/cmd_unsave/cmd_list，但实现在 bootscript
 * 代码块里命名为 cmd_bootscript_save 等 —— 命令表初始化之前先 define 做重定向，
 * 这样 g_cmd_table 中取函数指针时取的就是 bootscript_* 版本。*/
#define cmd_save    cmd_bootscript_save
#define cmd_unsave  cmd_bootscript_unsave
#define cmd_list    cmd_bootscript_list

/* —— 各命令处理函数前向声明 —— */
static int cmd_help(int argc, char **argv);
static int cmd_ps(int argc, char **argv);
static int cmd_heap(int argc, char **argv);
static int cmd_tick(int argc, char **argv);
static int cmd_version(int argc, char **argv);
static int cmd_suspend(int argc, char **argv);
static int cmd_resume(int argc, char **argv);
static int cmd_kill(int argc, char **argv);
static int cmd_clear(int argc, char **argv);
static int cmd_led(int argc, char **argv);
static int cmd_syscalls(int argc, char **argv);
static int cmd_gpio(int argc, char **argv);
static int cmd_i2c(int argc, char **argv);
static int cmd_save(int argc, char **argv);
static int cmd_unsave(int argc, char **argv);
static int cmd_list(int argc, char **argv);
static int cmd_boot(int argc, char **argv);
static int cmd_factory_reset(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_vtest(int argc, char **argv);
static int cmd_jobs(int argc, char **argv);

/* —— vtest: 三任务嵌套调度稳定性验证（v2.2.5 调度系统完全正常的验证工具）
 *   VT1 (vt_led): 板载 LED 500ms 心跳翻转 → 验证 SLEEP→READY 轮转
 *   VT2 (vt_oled): 每 2s 向 OLED SSD1306 写一帧棋盘格/计数器 → 验证 I2C 阻塞
 *                 操作 (5 chunks × 1024B) 期间调度器能切走它，心跳继续跳
 *   VT3 (vt_ctrl): 每 3s 一轮，malloc/free 压力 + suspend/resume VT2
 *                 → 验证任务嵌套控制：VT3 操作 VT2 状态，期间 VT1 仍按时跳，
 *                   heap alloc/free 后零碎片，状态机/队列不损坏
 * —— */
static tcb_t *g_vt1 = NULL;  /* LED 心跳任务句柄 */
static tcb_t *g_vt2 = NULL;  /* OLED 刷新任务句柄 */
static tcb_t *g_vt3 = NULL;  /* 压力/嵌套控制任务句柄 */
static volatile uint8_t  g_vtest_running = 0;   /* 1=在跑，0=已停止/未启动 */
static volatile uint8_t  g_vt_oled_bus  = 0;    /* VT2 用的 I2C 总线号 (0/1) */
static volatile uint8_t  g_vt_oled_addr = 0x3C; /* VT2 用的 OLED 7-bit 地址 */
/* 【v2.2.11 · 栈保险】VT2 的 256B chunk buffer 放静态 BSS 段，不占栈空间。
 *   旧版放 `uint8_t block[256]` 局部变量 = 栈直接少 256B。
 *   1536B 总栈减去调用深度 ≈ 剩 1000B 余量，再砍 256B 很容易触发
 *   tick_hook 的 4×MAGIC 金丝雀检测 → 静默挂起 → 看起来像"崩溃黑屏"。
 *   移到静态后相当于 VT2 栈凭空多 256B 余量，且不违反用户"不增任务栈大小"的要求。
 *   同时把 5B 命令缓冲 cbuf 也静态化（本来 5B 影响不大，但顺手做了一致）。*/
static uint8_t g_vt2_block[256];
static uint8_t g_vt2_cbuf[5];
/* 统计计数器（vtest status 展示）：用户可直观判断三个任务都在被调度 */
static volatile uint32_t g_vt1_beats   = 0;  /* LED 心跳翻转次数 */
static volatile uint32_t g_vt2_frames  = 0;  /* OLED 写 GDRAM 帧数 (含 NACK 失败) */
static volatile uint32_t g_vt2_errs    = 0;  /* OLED I2C NACK 次数 */
static volatile uint32_t g_vt3_rounds  = 0;  /* VT3 嵌套控制轮次 */
static volatile uint32_t g_vt3_susps   = 0;  /* VT3 成功 suspend VT2 次数 */
static volatile uint32_t g_vt3_rsms    = 0;  /* VT3 成功 resume VT2 次数 */

/* 三个验证任务 entry（定义在 shell.c 末尾，cmd_vtest 旁边） */
static void vt_task_led(void *arg);
static void vt_task_oled(void *arg);
static void vt_task_ctrl(void *arg);

#undef cmd_save
#undef cmd_unsave
#undef cmd_list

/* bootscript/shell 内部跨函数引用的前向声明（避免先引用后定义） */
static int  shell_exec_line(char *line);        /* 定义在 命令解析 dispatch 末尾 */
static int  cmd_bootscript_boot_exec(int argc, char **argv);  /* 定义在 bootscript 命令块末尾 */
extern int   bootscript_run_all(void);          /* 定义在 bootscript 命令块前面（非 static，暴露 API） */

/* ================================================================
 * 命令表（驱动扩展：新增命令只需加一行）
 * 构建命令表之前临时定义 save/unsave/list → cmd_bootscript_* 映射
 * ================================================================ */
static const shell_cmd_t g_cmd_table[] = {
#define cmd_save    cmd_bootscript_save
#define cmd_unsave  cmd_bootscript_unsave
#define cmd_list    cmd_bootscript_list
    { "help",    cmd_help,    "help [cmd]",            "打印帮助信息" },
    { "ps",      cmd_ps,      "ps",                    "列出所有任务（ID/状态/栈/剩余tick）" },
    { "heap",    cmd_heap,    "heap",                  "打印内核堆空闲字节 / 最大空闲块" },
    { "tick",    cmd_tick,    "tick",                  "打印当前系统 tick 值（@OS_CFG_TICK_HZ Hz）" },
    { "version", cmd_version, "version | ver",         "打印内核版本字符串" },
    { "suspend", cmd_suspend, "suspend <id>",          "挂起指定 ID 的任务" },
    { "resume",  cmd_resume,  "resume <id>",           "恢复指定 ID 的被挂起任务" },
    { "kill",    cmd_kill,    "kill <id>",             "销毁指定 ID 的任务（不可恢复）" },
    { "clear",   cmd_clear,   "clear | cls",           "清屏并将光标移至左上角" },
    { "led",     cmd_led,     "led on | off | toggle", "控制 RP2040 板载 LED (GPIO25)" },
    { "gpio",    cmd_gpio,    "gpio help | init ... | read <pin> | write <pin> 0|1 | toggle <pin>",
                                                    "GPIO 子命令：初始化/读/写/翻转任意 RP2040 引脚" },
    { "i2c",     cmd_i2c,     "i2c help | init ... | scan | wr | rd | memwr | memrd",
                                                    "I2C 主机子命令：扫描总线/读写/寄存器访问" },
    { "vtest",   cmd_vtest,   "vtest start [bus] [addr] | stop | status",
                                                    "三任务嵌套调度验证: start=启动VT1(LED心跳)/VT2(OLED刷新)/VT3(压力+嵌套suspend-resume); status=查看三任务计数器; stop=销毁三任务" },
    { "jobs",    cmd_jobs,    "jobs",                  "列出用户创建的后台任务（类似 Linux jobs）" },
    { "save",    cmd_save,    "save <any command...>",
                                                    "把任意命令追加到 Flash 固化区（不立即执行；下次开机自动执行）" },
    { "unsave",  cmd_unsave,  "unsave <idx> | all",   "删除固化命令：按序号或一键清空" },
    { "list",    cmd_list,    "list",                  "列出所有已固化命令（Flash 双备份 + CRC）" },
    { "boot",    cmd_boot,    "boot exec | boot flash_test | boot status",
                                                    "boot: 固化指令子系统；exec=立即跑；status=查看上次回放结果(解决启动时USB输出被丢)；flash_test=B线路SPI自检" },
    { "factory_reset", cmd_factory_reset, "factory_reset | factory_reset confirm",
                                                    "出厂重置：擦除所有持久化数据(bootscript+末尾保留区)；保留内核固件本身" },
    { "reboot",   cmd_reboot,   "reboot",            "软复位系统：重新执行完整启动流程(重现开机画面 + 应用固化超频/多核)" },
    { "syscalls",cmd_syscalls,"syscalls",              "列出系统调用契约表" },
    { "ver",     cmd_version, NULL, NULL },            /* version 的别名 */
    { "cls",     cmd_clear,   NULL, NULL },            /* clear 的别名 */
};
#undef cmd_save
#undef cmd_unsave
#undef cmd_list
#define SHELL_CMD_NUM  (sizeof(g_cmd_table) / sizeof(g_cmd_table[0]))

/* ================================================================
 * 各命令实现
 * ================================================================ */

/* help [cmd] */
static int cmd_help(int argc, char **argv) {
    if (argc >= 2) {
        /* 过滤具体命令的帮助 —— 先静态表、再扩展表 */
        const char *name = argv[1];
        for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
            if (!g_cmd_table[i].usage) continue;
            if (strcmp(g_cmd_table[i].name, name) == 0) {
                sh_puts("Usage  : "); sh_puts(g_cmd_table[i].usage); sh_crlf();
                sh_puts("Summary: "); sh_puts(g_cmd_table[i].help ? g_cmd_table[i].help : ""); sh_crlf();
                return 0;
            }
        }
        shell_cmd_fn_ext_t extfn = NULL;
        const char *eu = NULL, *eh = NULL;
        if (shell_ext_lookup(name, &extfn, &eu, &eh) >= 0) {
            sh_puts("Usage  : "); sh_puts(eu ? eu : ""); sh_crlf();
            sh_puts("Summary: "); sh_puts(eh ? eh : ""); sh_crlf();
            return 0;
        }
        sh_puts("Unknown command: "); sh_puts(name); sh_crlf();
        return 1;
    }
    sh_puts("=== Mini Kernel Shell Help ===\r\n");
    sh_puts("Command          Description\r\n");
    sh_puts("----------------------------------------\r\n");
    for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
        if (!g_cmd_table[i].usage) continue;
        sh_puts(g_cmd_table[i].name);
        int pad = 17 - (int)strlen(g_cmd_table[i].name);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        sh_puts(g_cmd_table[i].help ? g_cmd_table[i].help : "");
        sh_crlf();
    }
    /* v2.2 新增：扩展命令（msc / ls / cd / pwd / mkdir / rmdir / rm / cat） */
    int ext_n = shell_ext_count();
    for (int i = 0; i < ext_n; i++) {
        const char *nm = NULL, *eu = NULL, *eh = NULL;
        shell_cmd_fn_ext_t fn;
        if (!shell_ext_get(i, &nm, &fn, &eu, &eh)) continue;
        if (!eu) continue;   /* 跳过别名（usage=NULL） */
        sh_puts(nm);
        int pad = 17 - (int)strlen(nm);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        sh_puts(eh ? eh : "");
        sh_crlf();
    }
    sh_puts("----------------------------------------\r\n");
    sh_puts("Tips: 支持退格键(\\b)。任务ID用 'ps' 命令查询。\r\n");
    sh_puts("调度说明: 本内核为时间片轮转(非抢占)。weight=任务权重倍数,\r\n");
    sh_puts("          单次连续运行 = 5ms × weight。weight 只影响每次轮到\r\n");
    sh_puts("          能连续占用的时长, 不改变排队顺序, 无优先级抢占。\r\n");
    sh_puts("          任务创建见 'k_task_create(name,fn,arg,stack,weight)'。\r\n");
    return 0;
}

/* ps */
static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_crlf();
    sh_puts("--- Task List ---\r\n");
    sh_puts("ID  Name        State     TicksLeft  StackBase  StackSize  StackOK\r\n");
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        tcb_t *t = g_task_pool[i];
        if (!t) continue;
        /* ID */
        shell_put_uint32(t->id);
        sh_puts("   ");
        /* Name + pad */
        sh_puts(t->name);
        int pad = 12 - (int)strlen(t->name);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        /* State */
        sh_puts(task_state_str(t->state));
        shell_pad_spaces(9 - (int)strlen(task_state_str(t->state)));
        /* TicksLeft */
        shell_put_uint32(t->ticks_to_sleep);
        hal_console_putc(' ');
        /* StackBase */
        shell_put_hex32((uint32_t)(uintptr_t)t->stack_base);
        hal_console_putc(' ');
        /* StackSize */
        shell_put_uint32(t->stack_size);
        hal_console_putc(' ');
        /* Stack guard check */
        hal_console_putc(task_stack_check(t) ? 'Y' : 'N');
        sh_crlf();
    }
    /* Current running task */
    sh_puts("Cur: ");
    sh_puts(g_current_task ? g_current_task->name : "<null>");
    sh_crlf();
    sh_puts("-----------------\r\n");
    return 0;
}

/* heap */
static int cmd_heap(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Heap free     = "); shell_put_uint32(kmem_free_size());         sh_puts(" B\r\n");
    sh_puts("Heap max blk  = "); shell_put_uint32(kmem_max_free_block());    sh_puts(" B\r\n");
    return 0;
}

/* tick */
static int cmd_tick(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Tick = ");
    shell_put_uint32(hal_systick_get_tick());
    sh_puts("  (@"); shell_put_uint32(OS_CFG_TICK_HZ); sh_puts(" Hz)\r\n");
    return 0;
}

/* version / ver */
static int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Mini Kernel version: ");
    sh_puts(k_version());
    sh_crlf();
    return 0;
}

/* 通过 ID 查找 TCB（辅助） */
static tcb_t *find_task_by_id(uint32_t id) {
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        if (g_task_pool[i] && g_task_pool[i]->id == id) return g_task_pool[i];
    }
    return NULL;
}

/* suspend <id> */
static int cmd_suspend(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: suspend <id> (use 'ps' for IDs)\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { sh_puts("Invalid ID (not a number)\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { sh_puts("Task id="); shell_put_uint32(id); sh_puts(" not found\r\n"); return 1; }
    if (t == g_current_task) { sh_puts("Cannot suspend the running shell task itself\r\n"); return 1; }
    task_suspend(t);
    sh_puts("Task id="); shell_put_uint32(id);
    sh_puts(" (" ); sh_puts(t->name); sh_puts(") SUSPENDed\r\n");
    return 0;
}

/* resume <id> */
static int cmd_resume(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: resume <id>\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { sh_puts("Invalid ID\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { sh_puts("Task id="); shell_put_uint32(id); sh_puts(" not found\r\n"); return 1; }
    task_resume(t);
    sh_puts("Task id="); shell_put_uint32(id);
    sh_puts(" (" ); sh_puts(t->name); sh_puts(") RESUMEd\r\n");
    return 0;
}

/* kill <id> */
static int cmd_kill(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: kill <id>\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { sh_puts("Invalid ID\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { sh_puts("Task id="); shell_put_uint32(id); sh_puts(" not found\r\n"); return 1; }
    if (t == g_current_task) { sh_puts("Cannot kill the shell task itself\r\n"); return 1; }
    char name_buf[16];
    memcpy(name_buf, t->name, 12); name_buf[12] = 0;
    task_destroy(t);
    sh_puts("Task id="); shell_put_uint32(id);
    sh_puts(" (" ); sh_puts(name_buf); sh_puts(") KILLed (TCB returned to pool)\r\n");
    return 0;
}

/* clear / cls */
static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    /* VT100 兼容：Putty, TeraTerm, cmder, screen, minicom, Windows Terminal 全部支持 */
    sh_puts("\033[2J");    /* ESC[2J 清屏 */
    sh_puts("\033[H");     /* ESC[H  光标回左上角 (row1,col1) */
    return 0;
}

/* led on|off|toggle
 * · 新增：每次命令都检查 hal_gpio_init 返回值（确保 OS_CFG_PERIPH_SERVICE 开启且 HAL 就位）
 * · 新增：写完输出电平后 **立即回读一次 SIO 电平**，失败时打印 "GPIO25 readback mismatch"
 *       这样可以区分"命令没执行"和"命令执行了但硬件没响应"两种情况。 */
static int cmd_led(int argc, char **argv) {
#if OS_CFG_PERIPH_SERVICE
    if (argc < 2) { sh_puts("Usage: led on | off | toggle\r\n"); return 1; }
    /* RP2040 板载 LED 固定是 GPIO25（Pico非W版），这里直接用 hal_gpio 接口 */
    const uint32_t pin = 25;
    hal_err_t ir = hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
    if (ir != HAL_OK) {
        sh_puts("ERROR: led: hal_gpio_init(GPIO25, OUT_PP) returned ");
        shell_put_err((int)ir); sh_puts(" (OS_CFG_PERIPH_SERVICE ON?)\r\n");
        return 1;
    }
    hal_gpio_level_t want = HAL_GPIO_LOW;
    if (strcmp(argv[1], "on") == 0) {
        want = HAL_GPIO_HIGH;
        hal_gpio_write(pin, HAL_GPIO_HIGH);
        sh_puts("LED GPIO25 ON\r\n");
    } else if (strcmp(argv[1], "off") == 0) {
        want = HAL_GPIO_LOW;
        hal_gpio_write(pin, HAL_GPIO_LOW);
        sh_puts("LED GPIO25 OFF\r\n");
    } else if (strcmp(argv[1], "toggle") == 0) {
        hal_gpio_level_t cur = hal_gpio_read(pin);
        want = (cur == HAL_GPIO_HIGH) ? HAL_GPIO_LOW : HAL_GPIO_HIGH;
        hal_gpio_toggle(pin);
        sh_puts("LED GPIO25 toggled\r\n");
    } else {
        sh_puts("Unknown led op (use: on | off | toggle)\r\n");
        return 1;
    }
    /* 回读校验：SIO 寄存器的读回必须等于预期，否则输出 FAIL 标志（这就是用户常说的
     *   "命令执行了但灯没亮" 时最直接的自检证据）。 */
    hal_gpio_level_t got = hal_gpio_read(pin);
    if (got != want) {
        sh_puts("  ↳ FAIL: GPIO25 readback ");
        sh_puts((got == HAL_GPIO_HIGH) ? "HIGH" : "LOW");
        sh_puts(" but expected ");
        sh_puts((want == HAL_GPIO_HIGH) ? "HIGH" : "LOW");
        sh_puts(" (HW problem?)\r\n");
        return 1;
    }
    sh_puts("  ↳ OK: GPIO25=");
    sh_puts((want == HAL_GPIO_HIGH) ? "HIGH" : "LOW");
    sh_puts(" (verified via SIO readback)\r\n");
    return 0;
#else
    (void)argc; (void)argv;
    sh_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, hal_gpio not linked\r\n");
    return 1;
#endif
}

/* syscalls — 列出系统调用契约表 */
static int cmd_syscalls(int argc, char **argv) {
    (void)argc; (void)argv;
    size_t total = syscall_table_size();
    sh_puts("\r\n=== Syscall Contract Table (");
    shell_put_uint32((uint32_t)total);
    sh_puts(" entries) ===\r\n");
    sh_puts("ID    Name            Params  Return     Signature\r\n");
    sh_puts("----------------------------------------------------------\r\n");
    for (size_t i = 0; i < total; i++) {
        const syscall_entry_t *e = syscall_get_entry(i);
        if (!e) break;
        /* ID */
        shell_put_uint32((uint32_t)e->id);
        shell_pad_spaces(6 - (int)(e->id / 10 + 1));
        /* Name */
        sh_puts(e->name);
        shell_pad_spaces(16 - (int)strlen(e->name));
        /* Params */
        shell_put_uint32(e->param_count);
        shell_pad_spaces(8);
        /* Return type */
        sh_puts(e->return_type);
        shell_pad_spaces(11 - (int)strlen(e->return_type));
        /* Signature */
        sh_puts(e->signature);
        sh_crlf();
    }
    sh_puts("----------------------------------------------------------\r\n");
    return 0;
}

/* ================================================================
 * gpio 子命令解析（预编译指令集风格，底层复用 HAL hal_gpio_*）
 *
 * 子命令表（对应 Linux GPIO sysfs / raspi-gpio 风格）：
 *   gpio help
 *   gpio init  <pin> in                    # 输入浮空
 *   gpio init  <pin> out    [0|1]          # 推挽输出，可选初始电平(默认低)
 *   gpio init  <pin> out_od [0|1]          # 开漏输出，可选初始电平
 *   gpio init  <pin> af <af_num>           # 复用功能（SPI/I2C/UART等 FUNCn）
 *   gpio read  <pin>                       # 读引脚电平，输出 0 / 1
 *   gpio write <pin> <0|1>                 # 写输出电平（高/低）
 *   gpio toggle <pin>                      # 翻转引脚
 * ================================================================ */

/* 把字符串解析为非负整数（失败返回 -1） */
static int shell_parse_uint(const char *s, uint32_t *out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10u + (uint32_t)(*p - '0');
    }
    *out = v;
    return 0;
}

static void gpio_help(void) {
    sh_puts("GPIO subcommands (pin=0..29 for RP2040, 30+ = QSPI/SWCLK reserved):\r\n");
    sh_puts("  gpio help                        打印本帮助\r\n");
    sh_puts("  gpio init  <pin> in              初始化: 浮空输入\r\n");
    sh_puts("  gpio init  <pin> out   [0|1]     初始化: 推挽输出，可选初始电平(默认0)\r\n");
    sh_puts("  gpio init  <pin> out_od [0|1]    初始化: 开漏输出，可选初始电平\r\n");
    sh_puts("  gpio init  <pin> af <af_num>     初始化: 复用功能(0=FUNC0/SPI .. 5=FUNC5/SIO)\r\n");
    sh_puts("  gpio read  <pin>                 读引脚电平 -> 打印 0 或 1\r\n");
    sh_puts("  gpio write <pin> <0|1>           设置输出电平\r\n");
    sh_puts("  gpio toggle <pin>                翻转输出电平\r\n");
    sh_puts("Examples:\r\n");
    sh_puts("  gpio init 25 out 1        # GPIO25(板载LED) 推挽输出 初始高电平\r\n");
    sh_puts("  gpio toggle 25            # 翻转 LED\r\n");
    sh_puts("  gpio init 0 af 2          # GPIO0=FUNC2(UART0_TX)\r\n");
    sh_puts("  gpio read 5               # 读 GPIO5 电平\r\n");
}

static int cmd_gpio(int argc, char **argv) {
#if OS_CFG_PERIPH_SERVICE
    /* argv[0] = "gpio"  子命令在 argv[1] */
    if (argc < 2) { gpio_help(); return 1; }
    const char *sub = argv[1];

    if (strcmp(sub, "help") == 0) {
        gpio_help();
        return 0;
    }

    if (strcmp(sub, "init") == 0) {
        if (argc < 4) { sh_puts("Usage: gpio init <pin> in | out [0|1] | out_od [0|1] | af <af_num>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) {
            sh_puts("Invalid pin number (must be 0..29)\r\n"); return 1;
        }
        const char *mode = argv[3];

        if (strcmp(mode, "in") == 0) {
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_IN, 0);
            if (r != HAL_OK) { sh_puts("GPIO"); shell_put_uint32(pin); sh_puts(" init IN failed (pin invalid)\r\n"); return 1; }
            sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" = INPUT (floating)\r\n");
            return 0;
        }
        if (strcmp(mode, "out") == 0) {
            uint32_t init_val = 0;
            if (argc >= 5) {
                if (shell_parse_uint(argv[4], &init_val) != 0 || init_val > 1) {
                    sh_puts("Invalid initial value (must be 0 or 1)\r\n"); return 1;
                }
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
            if (r != HAL_OK) { sh_puts("GPIO"); shell_put_uint32(pin); sh_puts(" init OUT failed\r\n"); return 1; }
            hal_gpio_write(pin, init_val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
            sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" = OUTPUT-PP, level="); shell_put_uint32(init_val); sh_puts("\r\n");
            return 0;
        }
        if (strcmp(mode, "out_od") == 0) {
            uint32_t init_val = 0;
            if (argc >= 5) {
                if (shell_parse_uint(argv[4], &init_val) != 0 || init_val > 1) {
                    sh_puts("Invalid initial value (must be 0 or 1)\r\n"); return 1;
                }
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_OUT_OD, 0);
            if (r != HAL_OK) { sh_puts("GPIO"); shell_put_uint32(pin); sh_puts(" init OUT_OD failed\r\n"); return 1; }
            hal_gpio_write(pin, init_val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
            sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" = OUTPUT-OD, level="); shell_put_uint32(init_val); sh_puts("\r\n");
            return 0;
        }
        if (strcmp(mode, "af") == 0) {
            if (argc < 5) { sh_puts("Usage: gpio init <pin> af <af_num> (0..9 for FUNC0..FUNC9)\r\n"); return 1; }
            uint32_t af_num = 0;
            if (shell_parse_uint(argv[4], &af_num) != 0 || af_num > 9) {
                sh_puts("Invalid af_num (must be 0..9). RP2040: 0=SPI,1=UART0,2=UART1,3=I2C0,4=I2C1,5=SIO,6=PWM,7=SIO/PIO,...\r\n");
                return 1;
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_AF, af_num);
            if (r != HAL_OK) { sh_puts("GPIO"); shell_put_uint32(pin); sh_puts(" init AF failed\r\n"); return 1; }
            sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" = ALT-FUNC"); shell_put_uint32(af_num); sh_puts("\r\n");
            return 0;
        }
        sh_puts("Unknown gpio mode: '"); sh_puts(mode); sh_puts("' (expected: in|out|out_od|af)\r\n");
        return 1;
    }

    if (strcmp(sub, "read") == 0) {
        if (argc < 3) { sh_puts("Usage: gpio read <pin>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { sh_puts("Invalid pin number\r\n"); return 1; }
        hal_gpio_level_t lv = hal_gpio_read(pin);
        sh_puts("GPIO"); shell_put_uint32(pin); sh_puts(" = ");
        sh_puts((lv == HAL_GPIO_HIGH) ? "1 (HIGH)" : "0 (LOW)");
        sh_puts("\r\n");
        return 0;
    }

    if (strcmp(sub, "write") == 0) {
        if (argc < 4) { sh_puts("Usage: gpio write <pin> <0|1>\r\n"); return 1; }
        uint32_t pin = 0, val = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { sh_puts("Invalid pin number\r\n"); return 1; }
        if (shell_parse_uint(argv[3], &val) != 0 || val > 1) { sh_puts("Invalid value (must be 0 or 1)\r\n"); return 1; }
        hal_gpio_write(pin, val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
        sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" = "); shell_put_uint32(val); sh_puts("\r\n");
        return 0;
    }

    if (strcmp(sub, "toggle") == 0) {
        if (argc < 3) { sh_puts("Usage: gpio toggle <pin>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { sh_puts("Invalid pin number\r\n"); return 1; }
        hal_gpio_toggle(pin);
        sh_puts("OK: GPIO"); shell_put_uint32(pin); sh_puts(" toggled. Now = ");
        sh_puts((hal_gpio_read(pin) == HAL_GPIO_HIGH) ? "1 (HIGH)" : "0 (LOW)");
        sh_puts("\r\n");
        return 0;
    }

    sh_puts("Unknown gpio subcommand: '"); sh_puts(sub); sh_puts("' (try 'gpio help')\r\n");
    return 1;

#else /* !PERIPH_SERVICE */
    (void)argc; (void)argv;
    sh_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, GPIO HAL not linked. Set =1 in os_config.h.\r\n");
    return 1;
#endif
}

/* ================================================================
 * i2c 子命令解析（Linux i2cdetect / i2c-tools 风格）
 *
 *   i2c help
 *   i2c init  <bus> <sda_pin> <scl_pin> <hz>
 *                   bus = 0 (I2C0) | 1 (I2C1)
 *                   RP2040: I2C0 默认 GP4=SDA/GP5=SCL; I2C1  GP6=SDA/GP7=SCL
 *                   hz = 100000 (Standard) | 400000 (Fast) | 1000000 (Fast+)
 *   i2c scan  <bus>                   # 探测 0x08~0x77 的 7-bit 地址
 *   i2c wr    <bus> <addr> <B1> [B2 ... Bn]       # 裸写 1+ 字节
 *   i2c rd    <bus> <addr> <len>                  # 裸读 len 字节
 *   i2c memwr <bus> <addr> <reg> <B1> [B2 ...]    # 设备内部寄存器写（reg 8/16-bit mem_addr）
 *   i2c memrd <bus> <addr> <reg> <len>            # 设备内部寄存器读
 *
 * 注意：所有地址字段 <addr>/<reg>/<Bx> 都接受十进制或 0x 十六进制，
 *       但 7-bit I2C 地址禁止自动左移（用户输入的是 Datasheet 里的 7-bit 值）。
 *       hal_port.c 的 i2c_write_blocking 需要 7-bit 地址（Pico SDK 约定）。
 * ================================================================ */

/* 解析 10 进制或带 0x 前缀的十六进制 uint32。失败返回 -1 */
static int shell_parse_uint_auto(const char *s, uint32_t *out) {
    if (!s || !*s) return -1;
    uint32_t base = 10;
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        if (!*p) return -1;
    }
    uint32_t v = 0;
    for (; *p; p++) {
        char c = *p;
        uint32_t digit;
        if (c >= '0' && c <= '9')      digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A') + 10;
        else return -1;
        if (digit >= base) return -1;
        v = v * base + digit;
    }
    *out = v;
    return 0;
}

static void i2c_help(void) {
    sh_puts("I2C subcommands (7-bit device addresses, no auto-left-shift):\r\n");
    sh_puts("  i2c init <bus> <sda> <scl> <hz>      Init + pinmux I2C bus (bus=0|1)\r\n");
    sh_puts("         RP2040 AF: bus0 pins=GP4(SDA)/GP5(SCL), bus1=GP6/GP7\r\n");
    sh_puts("         Typical hz: 100000 / 400000 / 1000000\r\n");
    sh_puts("  i2c scan <bus>                      Scan 7-bit addresses 0x08..0x77\r\n");
    sh_puts("  i2c wr   <bus> <addr> <b1> [..bn]   Raw write 1..N bytes\r\n");
    sh_puts("  i2c rd   <bus> <addr> <len>         Raw read len bytes\r\n");
    sh_puts("  i2c cmds <bus> <addr> <c1> [..cn]   SSD1306-style: 0x00 (Co=0,D/C=0) + N command bytes\r\n");
    sh_puts("  i2c fill <bus> <addr> <byte> <cnt>  SSD1306-style: 0x40 (Co=0,D/C=1) + cnt×byte (GDRAM fill)\r\n");
    sh_puts("  i2c memwr <bus> <addr> <reg> <b1> [..]   Write device register (mem 16-bit)\r\n");
    sh_puts("  i2c memrd <bus> <addr> <reg> <len>       Read device register (mem 16-bit)\r\n");
    sh_puts("Examples:\r\n");
    sh_puts("  i2c init 0 4 5 100000                # Standard-mode on default pins\r\n");
    sh_puts("  i2c scan 0                           # Show connected devices\r\n");
    sh_puts("  i2c wr 0 0x3C 0x00 0xAF              # Write 2 bytes to OLED at 0x3C\r\n");
    sh_puts("  i2c memrd 0 0x50 0x00 16             # Read 16 bytes from AT24Cxx EEPROM @ 0\r\n");
}

/* 打印一个字节为 2 位十六进制，前缀空格 */
static void shell_print_hex8(uint8_t b) {
    const char hex[] = "0123456789ABCDEF";
    hal_console_putc(hex[(b >> 4) & 0xF]);
    hal_console_putc(hex[b & 0xF]);
}

static int cmd_i2c(int argc, char **argv) {
#if OS_CFG_PERIPH_SERVICE
    if (argc < 2) { i2c_help(); return 1; }
    const char *sub = argv[1];

    if (strcmp(sub, "help") == 0) {
        i2c_help();
        return 0;
    }

    if (strcmp(sub, "init") == 0) {
        if (argc < 6) { sh_puts("Usage: i2c init <bus> <sda_pin> <scl_pin> <hz>\r\n"); return 1; }
        uint32_t bus, sda, scl, hz;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &sda) != 0 || sda > 29) { sh_puts("Invalid SDA pin (0..29)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &scl) != 0 || scl > 29) { sh_puts("Invalid SCL pin (0..29)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &hz)  != 0 || hz == 0)  { sh_puts("Invalid hz (non-zero)\r\n"); return 1; }

        /* RP2040 GPIO_FUNC_I2C = 3，SDK 自动按 pin 路由到 I2C0/I2C1
         * 注：Pico SDK i2c_init 会自动 gpio_set_function，但不会自动开上拉！
         * I2C 标准要求 SDA/SCL 必须有上拉（模块内部或外部 4.7kΩ）。
         * 这里直接操作 PADS_BANK0 寄存器开启 ~50kΩ 内部上拉做兜底，
         *   不依赖 Pico SDK 符号，避免 shell 编译单元侵入平台头文件：
         *   PADS_BANK0 基址 = 0x4001C000
         *   PADS_BANK0_GPIO<N> = 0x4001C004 + 4*N
         *     bit3 PUE (Pull-Up Enable)  置 1 开上拉
         *     bit2 PDE (Pull-Dn Enable)  清 0 关下拉
         *     其他位保持 SDK 设置的默认值即可 */
        #define PADS_BANK0_BASE   0x4001C000u
        #define PADS_GPIO(n)      (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x04u + 4u*(n)))
        #define PADS_PUE          (1u << 3)
        #define PADS_PDE          (1u << 2)
        PADS_GPIO(sda) = (PADS_GPIO(sda) | PADS_PUE) & ~PADS_PDE;
        PADS_GPIO(scl) = (PADS_GPIO(scl) | PADS_PUE) & ~PADS_PDE;
        hal_err_t r1 = hal_gpio_init(sda, HAL_GPIO_AF, 3);
        hal_err_t r2 = hal_gpio_init(scl, HAL_GPIO_AF, 3);
        hal_err_t r3 = hal_i2c_init(bus, hz);
        if (r1 != HAL_OK || r2 != HAL_OK) { sh_puts("I2C pinmux FAILED\r\n"); return 1; }
        if (r3 != HAL_OK) { sh_puts("I2C init FAILED\r\n"); return 1; }

        sh_puts("OK: I2C"); shell_put_uint32(bus);
        sh_puts(" SDA=GP"); shell_put_uint32(sda);
        sh_puts(" SCL=GP"); shell_put_uint32(scl);
        sh_puts(" @"); shell_put_uint32(hz); sh_puts("Hz\r\n");
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        if (argc < 3) { sh_puts("Usage: i2c scan <bus>\r\n"); return 1; }
        uint32_t bus;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        sh_puts("Scanning I2C"); shell_put_uint32(bus);
        sh_puts(" (7-bit addresses 0x08..0x77):\r\n   ");
        for (uint8_t col = 0; col < 16; col++) {
            shell_print_hex8(col); sh_putc(' ');
        }
        sh_crlf();
        for (uint8_t row = 0; row < 8; row++) {
            shell_print_hex8(row << 4);
            sh_putc(':'); sh_putc(' ');
            for (uint8_t col = 0; col < 16; col++) {
                uint8_t addr = (uint8_t)((row << 4) | col);
                if (addr < 0x08 || addr > 0x77) {
                    sh_puts("-- ");
                    continue;
                }
                /* 标准探测（RP2040 Pico SDK 推荐方式）：
                 *   Pico SDK 的 i2c_write_blocking 在 len=0 时不会发 START+ADDR，
                 *   直接 ret=0 导致 HAL_OK，表现为 scan 全 ACK（见用户报告）。
                 *   正确做法：执行 "read 1 byte" —— i2c_read_blocking(len=1) 一定会
                 *   发 START + ADDR_R + ACK 采样 + STOP，ACK 即表示该地址有设备。
                 *   读到的 dummy 字节直接丢弃。 */
                uint8_t dummy;
                hal_err_t pr = hal_i2c_rx(bus, addr, &dummy, 1);
                if (pr == HAL_OK) {
                    shell_print_hex8(addr); sh_putc(' ');
                } else {
                    sh_puts("-- ");
                }
            }
            sh_crlf();
        }
        sh_puts("Done.\r\n");
        return 0;
    }

    if (strcmp(sub, "wr") == 0) {
        if (argc < 5) { sh_puts("Usage: i2c wr <bus> <addr> <B1> [B2 ...]\r\n"); return 1; }
        uint32_t bus, addr;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr (0x00..0x7F)\r\n"); return 1; }
        int nbytes = argc - 4;
        if (nbytes > 256) { sh_puts("Too many bytes (>256)\r\n"); return 1; }
        uint8_t buf[256];
        for (int i = 0; i < nbytes; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[4 + i], &v) != 0 || v > 0xFF) {
                sh_puts("Invalid byte at pos "); shell_put_uint32(i); sh_puts(": "); sh_puts(argv[4 + i]); sh_crlf();
                return 1;
            }
            buf[i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_tx(bus, (uint8_t)addr, buf, (size_t)nbytes);
        if (r != HAL_OK) { sh_puts("I2C WR NACK or ERROR at addr=0x"); shell_print_hex8((uint8_t)addr); sh_crlf(); return 1; }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" WR 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts(" ["); for (int i = 0; i < nbytes; i++) { if (i) sh_putc(' '); shell_print_hex8(buf[i]); }
        sh_puts("] ("); shell_put_uint32(nbytes); sh_puts(" bytes)\r\n");
        return 0;
    }

    /* i2c cmds — SSD1306 风格：首字节固定 0x00 (Co=0 D/C#=0 后续全是命令字节) + N 个命令参数
     *   SHELL_MAX_ARGS=10 时：cmd=cmds + bus + addr + 最多 7 个命令字节 / 次。
     *   长初始化序列多分几次 i2c cmds 调用即可。 */
    if (strcmp(sub, "cmds") == 0) {
        if (argc < 5) { sh_puts("Usage: i2c cmds <bus> <addr> <C1> [C2 ... C7]\r\n"); return 1; }
        uint32_t bus, addr;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr\r\n"); return 1; }
        int ncmd = argc - 4;
        if (ncmd > 255) { sh_puts("Too many commands (>255)\r\n"); return 1; }
        /* buf[0] = 0x00 控制前缀，buf[1..ncmd] = 命令字节 */
        uint8_t buf[257];
        buf[0] = 0x00;
        for (int i = 0; i < ncmd; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[4 + i], &v) != 0 || v > 0xFF) {
                sh_puts("Invalid cmd byte at pos "); shell_put_uint32(i); sh_crlf();
                return 1;
            }
            buf[1 + i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_tx(bus, (uint8_t)addr, buf, (size_t)(ncmd + 1));
        if (r != HAL_OK) { sh_puts("I2C CMDS NACK/ERROR @0x"); shell_print_hex8((uint8_t)addr); sh_crlf(); return 1; }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" CMDS 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts(" prefix=0x00 + [");
        for (int i = 0; i < ncmd; i++) { if (i) sh_putc(' '); shell_print_hex8(buf[1 + i]); }
        sh_puts("] ("); shell_put_uint32(ncmd); sh_puts(" commands)\r\n");
        return 0;
    }

    /* i2c fill — SSD1306 风格：首字节 0x40 (Co=0 D/C#=1 后续全是 GDRAM 数据) + cnt × 同一个 byte
     *   典型用途：OLED 全屏点亮 = fill 0x3C 0xFF 1024（128×64 bits / 8）
     *            OLED 清屏      = fill 0x3C 0x00 1024
     *            OLED 条纹     = fill 0x3C 0xAA 1024 */
    if (strcmp(sub, "fill") == 0) {
        if (argc < 6) { sh_puts("Usage: i2c fill <bus> <addr> <byte> <cnt>\r\n"); return 1; }
        uint32_t bus, addr, bytev, count;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &bytev) != 0 || bytev > 0xFF) { sh_puts("Invalid fill byte (0x00..0xFF)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &count) != 0 || count == 0 || count > 8192) { sh_puts("Invalid cnt (1..8192)\r\n"); return 1; }

        /* 分块发送：每块最多 256 字节（控制前缀 0x40 + 最多 255 数据字节）
         *   SSD1306 的列指针自动递增，D/C=1 流可以被多次 I2C START 断续发送，
         *   只要保持 D/C=1 前缀，内部指针不会复位。 */
        uint8_t block[256];
        block[0] = 0x40;       /* D/C=1, 数据流 */
        memset(block + 1, (uint8_t)bytev, 255);   /* 全部填充为 bytev */
        size_t remaining = (size_t)count;
        uint32_t chunks = 0;
        while (remaining > 0) {
            size_t chunk = (remaining > 255) ? 255u : (size_t)remaining;
            hal_err_t r = hal_i2c_tx(bus, (uint8_t)addr, block, chunk + 1);
            if (r != HAL_OK) {
                sh_puts("I2C FILL NACK/ERROR @chunk #");
                shell_put_uint32(chunks);
                sh_puts(" bytes left="); shell_put_uint32(remaining); sh_crlf();
                return 1;
            }
            remaining -= chunk;
            chunks++;
        }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" FILL 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts(" byte=0x"); shell_print_hex8((uint8_t)bytev);
        sh_puts(" ×"); shell_put_uint32(count);
        sh_puts(" bytes ("); shell_put_uint32(chunks); sh_puts(" chunks)\r\n");
        return 0;
    }

    if (strcmp(sub, "rd") == 0) {
        if (argc < 5) { sh_puts("Usage: i2c rd <bus> <addr> <len>\r\n"); return 1; }
        uint32_t bus, addr, len;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &len) != 0 || len == 0 || len > 256) { sh_puts("Invalid len (1..256)\r\n"); return 1; }
        uint8_t buf[256];
        hal_err_t r = hal_i2c_rx(bus, (uint8_t)addr, buf, (size_t)len);
        if (r != HAL_OK) { sh_puts("I2C RD NACK or ERROR at addr=0x"); shell_print_hex8((uint8_t)addr); sh_crlf(); return 1; }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" RD 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts(" -> ["); for (size_t i = 0; i < len; i++) { if (i) sh_putc(' '); shell_print_hex8(buf[i]); }
        sh_puts("] ("); shell_put_uint32(len); sh_puts(" bytes)\r\n");
        return 0;
    }

    if (strcmp(sub, "memwr") == 0) {
        if (argc < 6) { sh_puts("Usage: i2c memwr <bus> <addr> <reg> <B1> [..Bn]\r\n"); return 1; }
        uint32_t bus, addr, reg;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &reg) != 0 || reg > 0xFFFF) { sh_puts("Invalid reg (0..0xFFFF, 16-bit mem addr)\r\n"); return 1; }
        int nbytes = argc - 5;
        if (nbytes <= 0 || nbytes > 254) { sh_puts("Need 1..254 data bytes\r\n"); return 1; }
        uint8_t buf[254];
        for (int i = 0; i < nbytes; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[5 + i], &v) != 0 || v > 0xFF) {
                sh_puts("Invalid byte at pos "); shell_put_uint32(i); sh_crlf();
                return 1;
            }
            buf[i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_mem_write(bus, (uint8_t)addr, (uint16_t)reg, buf, (size_t)nbytes);
        if (r != HAL_OK) { sh_puts("I2C MEMWR ERROR at addr=0x"); shell_print_hex8((uint8_t)addr);
                           sh_puts(" reg=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg); sh_crlf(); return 1; }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" MEMWR 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts("@REG=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg);
        sh_puts(" ["); for (int i = 0; i < nbytes; i++) { if (i) sh_putc(' '); shell_print_hex8(buf[i]); }
        sh_puts("]\r\n");
        return 0;
    }

    if (strcmp(sub, "memrd") == 0) {
        if (argc < 6) { sh_puts("Usage: i2c memrd <bus> <addr> <reg> <len>\r\n"); return 1; }
        uint32_t bus, addr, reg, len;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { sh_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { sh_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &reg) != 0 || reg > 0xFFFF) { sh_puts("Invalid reg (0..0xFFFF)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &len) != 0 || len == 0 || len > 256) { sh_puts("Invalid len (1..256)\r\n"); return 1; }
        uint8_t buf[256];
        hal_err_t r = hal_i2c_mem_read(bus, (uint8_t)addr, (uint16_t)reg, buf, (size_t)len);
        if (r != HAL_OK) { sh_puts("I2C MEMRD ERROR at addr=0x"); shell_print_hex8((uint8_t)addr);
                           sh_puts(" reg=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg); sh_crlf(); return 1; }
        sh_puts("OK: I2C"); shell_put_uint32(bus); sh_puts(" MEMRD 0x"); shell_print_hex8((uint8_t)addr);
        sh_puts("@REG=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg);
        sh_puts(" -> ["); for (size_t i = 0; i < len; i++) { if (i) sh_putc(' '); shell_print_hex8(buf[i]); }
        sh_puts("]\r\n");
        return 0;
    }

    sh_puts("Unknown i2c subcommand: '"); sh_puts(sub); sh_puts("' (try 'i2c help')\r\n");
    return 1;

#else /* !PERIPH_SERVICE */
    (void)argc; (void)argv;
    sh_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, I2C HAL not linked.\r\n");
    return 1;
#endif
}

/* ========== cmd_boot 包装子命令 ========== */
static int cmd_bootscript_status(int argc, char **argv);   /* 前向声明，定义在下方 */

static int cmd_boot(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: boot exec | boot flash_test | boot status\r\n"
                               "  exec       - 立即执行全部固化命令（与开机同路径）\r\n"
                               "  flash_test - (B线路SPI) 擦+写双备份扇区后校验一致性\r\n"
                               "  status     - 查看 RAM 中上次开机 bootscript 回放结果（解决启动时 USB 输出被丢看不到）\r\n"); return 1; }
    const char *sub = argv[1];
    if (strcmp(sub, "exec") == 0)        return cmd_bootscript_boot_exec(argc, argv);
    if (strcmp(sub, "status") == 0)      return cmd_bootscript_status(argc, argv);
    if (strcmp(sub, "flash_test") == 0) {
        hal_err_t e = bootscript_erase_test();
        if (e != HAL_OK) { sh_puts("B-LINE SPI: ERASE FAILED\r\n"); return 1; }
        bool ok = bootscript_verify();
        if (!ok)     { sh_puts("B-LINE SPI: BACKUP MISMATCH/CRC FAIL\r\n"); return 1; }
        sh_puts("B-LINE SPI: Flash backup sectors A/B erase→write OK, headers consistent.\r\n");
        /* 追加并再校验，压力测试单个 slot write 路径 */
        e = bootscript_append("led on");
        if (e != HAL_OK) { sh_puts("B-LINE SPI: APPEND FAILED (rc="); shell_put_uint32(e); sh_puts(")\r\n"); return 1; }
        ok = bootscript_verify();
        if (!ok) { sh_puts("B-LINE SPI: POST-APPEND BACKUP MISMATCH\r\n"); return 1; }
        (void)bootscript_clear_all();
        sh_puts("B-LINE SPI: Append + dual-copy CRC check PASSED.\r\n");
        return 0;
    }
    sh_puts("Unknown boot subcommand: '"); sh_puts(sub); sh_puts("'\r\n"); return 1;
}

/* —— boot status: 打印 RAM 中保存的上次 bootscript_run_all 回放结果 ——
 *   这是 v0.2.0-beta 新增的"事后诊断"核心：
 *     PICO_STDIO_USB_STDOUT_TIMEOUT_US=0 时，用户启动时若还没打开 PuTTY/终端，
 *     bootscript_run_all 打印的 BOOTSCRIPT START/END banner + 所有错误/成功
 *     信息会因 CDC IN FIFO 满被全部丢弃，用户只能看到 mk> 提示符和 list 中的
 *     命令条目 —— 但"命令到底有没有真跑"完全看不到。
 *   
 *   有了 boot status 后：用户任何时候打开终端输入 boot status 都能看到：
 *     · 本次上电 bootscript_run_all 是否曾经运行过
 *     · 共多少条、成功多少、失败多少
 *     · 每条命令的原内容 + shell_exec_line 返回码
 *     · 回放结束后 GPIO25 电平（LED on 是否真的生效）
 *   不再依赖"刚好在启动前打开终端"这种时序。 */
static int cmd_bootscript_status(int argc, char **argv) {
    (void)argc; (void)argv;
    const bootscript_status_t *st = bootscript_get_status();
    sh_puts("==============================================================\r\n");
    sh_puts("  Bootscript Playback Status (RAM resident, since power-on)\r\n");
    sh_puts("==============================================================\r\n");
    if (!st->ran) {
        sh_puts("  · bootscript_run_all() has NOT been called this boot.\r\n");
        sh_puts("  · Possible reasons: OS_CFG_SHELL=0, or shell_start() never invoked.\r\n");
        sh_puts("  · Tip: try 'boot exec' to run it now (same path as power-on).\r\n");
        sh_puts("==============================================================\r\n");
        return 1;
    }
    sh_puts("  · Called        : YES (current boot)\r\n");
    sh_puts("  · Total slots   : "); shell_put_uint32(st->total); sh_crlf();
    sh_puts("  · Executed OK   : "); shell_put_uint32(st->ok_count); sh_crlf();
    sh_puts("  · Executed FAIL : "); shell_put_uint32(st->fail_count); sh_crlf();
    sh_puts("  · GPIO25 level  : ");
    if (st->final_gpio25_level == 0xFFu) {
        sh_puts("UNKNOWN (no PERIPH service or 0 entries)\r\n");
    } else if (st->final_gpio25_level == 1u) {
        sh_puts("HIGH — LED SHOULD BE ON (bootscript `led on` took effect)\r\n");
    } else {
        sh_puts("LOW — LED IS OFF (if you expected ON, check `led on` rc below)\r\n");
    }
    sh_puts("--------------------------------------------------------------\r\n");
    if (st->total == 0) {
        sh_puts("  (No persistent commands stored at boot time. 'list' may show new saves since.)\r\n");
    } else {
        sh_puts("  # | RC  | Result | Command line\r\n");
        sh_puts("----+-----+--------+---------------------------------------------\r\n");
        for (uint8_t i = 0; i < st->total; i++) {
            const bootscript_log_entry_t *e = &st->entries[i];
            /* 序号 */
            shell_pad_spaces(3); shell_put_uint32(i); sh_puts(" | ");
            /* rc: 最多 4 位 + 符号 */
            int rc = e->exec_rc;
            if (rc == -999) sh_puts("CRC  ");  /* 特殊：读失败 */
            else {
                if (rc < 0) { hal_console_putc('-'); rc = -rc; }
                else hal_console_putc(' ');
                if (rc >= 100) { shell_put_uint32((uint32_t)rc / 100); } else sh_putc(' ');
                if (rc >= 10)  { shell_put_uint32(((uint32_t)rc / 10) % 10); } else sh_putc(' ');
                shell_put_uint32((uint32_t)rc % 10);
            }
            sh_puts(" | ");
            /* result */
            if (e->exec_rc == 0)       sh_puts("PASS   | ");
            else if (e->exec_rc == -999) sh_puts("BAD SLOT| ");
            else                        sh_puts("FAIL   | ");
            /* command line（最多 60 字，超长截断 … ） */
            {
                const char *p = e->cmd_line[0] ? e->cmd_line : "(empty)";
                int shown = 0;
                while (*p && shown < 60) { hal_console_putc(*p++); shown++; }
                if (*p) sh_puts("...");
            }
            sh_crlf();
        }
    }
    sh_puts("==============================================================\r\n");
    sh_puts("  Tip: To force re-run and refresh this status, use:  boot exec\r\n");
    sh_puts("==============================================================\r\n");
    return 0;
}

/* ================================================================
 * 行解析器：按"空格"拆分 argv[0..argc-1]
 * ================================================================ */
static int shell_split_argv(char *line, char **argv, int max_args, bool *overflowed /* out */) {
    int argc = 0;
    bool over = false;
    char *p = line;
    while (*p) {
        /* 跳过前导空白（空格/Tab） */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (argc >= max_args) { over = true; break; }
        argv[argc++] = p;
        /* 走到本段 token 末尾 */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    if (overflowed) *overflowed = over;
    return argc;
}

/* ================================================================
 * bootscript 命令：固化指令到板载 Flash（开机自动执行）
 *   save <cmd ...>        → 追加固化（不立即执行）
 *   unsave <idx>|all      → 删除第 idx 条 / 全部清空
 *   list                  → 列出所有已固化命令
 *   boot exec             → 立刻执行全部已固化命令（验证用）
 *
 * 另外"!"是语法糖前缀：!<cmd ...>  =  先执行该命令，若成功再 save 固化。
 *   这个 ! 前缀在 shell_exec_line 入口处剥掉并设置"执行后 append" flag。
 * ================================================================ */
/* （bootscript.h 已经在文件顶部 OS_CFG_SHELL 后 include，此处避免重复 include 导致
 *  conflicting types 编译错误） */

/* 把 (argc, argv) 重新拼成一行字符串（用单空格分隔，每个参数原样）
 *   cmd_save 里还原回命令行字符串后 append。buf 长 SHELL_LINE_SIZE */
static void argv_rejoin(int argc, char **argv, int arg_start, char *buf, size_t buf_len) {
    size_t out = 0;
    for (int i = arg_start; i < argc; i++) {
        size_t alen = strlen(argv[i]);
        if (i > arg_start) {
            if (out + 1 < buf_len) buf[out++] = ' ';
        }
        for (size_t j = 0; j < alen && out + 1 < buf_len; j++) {
            buf[out++] = argv[i][j];
        }
    }
    buf[out] = '\0';
}

/* 公开 API：执行 bootscript 中全部已固化命令（boot_setup 开机和 shell "boot exec" 都会调用）
 *
 * 2026-08-15 用户报告：重启后 list 能看到 #0 led on，但 LED 没亮。
 *   → 增强启动痕迹，绝不静默：
 *       1. 开头打印分隔线 ===== BOOTSCRIPT START ===== + 带 total count
 *       2. 每条命令前后 BEGIN/END，END 显示 shell_exec_line 返回 rc
 *       3. rc != 0 时把 hal_err_t 名字打印出来（如果是 -1~-9）
 *       4. 执行完后 200ms 小延时 + GPIO25 读回电平汇报 LED 当前状态（视觉锚点）
 *       5. 最后再打 ===== BOOTSCRIPT DONE ===== + N ok / M failed
 *       6. 如果 total == 0（且 HAL 已就绪），**主动 dump A/B 扇区诊断**，
 *          防止"save 成功 list 看到、重启却 count=0"的扇区双写不一致被静默掩盖。 */
int bootscript_run_all(void) {
    sh_puts("==============================================================\r\n");
    sh_puts("=====         BOOTSCRIPT START (persistent cmd playback)        =====\r\n");
    sh_puts("==============================================================\r\n");
    uint8_t total = bootscript_count();
    bootscript_rec_begin(total);                 /* ← RAM 记录：begin */
    if (total == 0) {
        sh_puts("[BOOT ] bootscript_count()==0 — No persistent commands saved yet.\r\n");
        sh_puts("[BOOT ] (Use 'save <cmd>' to queue, or '!<cmd>' to exec-then-save)\r\n");
        /* 诊断：A/B 扇区 dump 出来，防止用户 save 后重启显示空、
         *       但是实际上只是双备份 A 或 B 某一侧 CRC/写入没成功。 */
        bootscript_diag_dump();
        sh_puts("==============================================================\r\n");
        sh_puts("=====         BOOTSCRIPT DONE (0 entries, idle)                =====\r\n");
        sh_puts("==============================================================\r\n");
        bootscript_rec_end(0, 0, 0xFFu);           /* ← RAM 记录：end (0 entries, GPIO unknown) */
        return 0;
    }
    sh_puts("[BOOT ] Will run "); shell_put_uint32(total); sh_puts(" persistent command(s):\r\n");
    int failed = 0;
    uint8_t ok_cnt = 0;
    for (uint8_t i = 0; i < total; i++) {
        char line[SHELL_LINE_SIZE];
        sh_puts("[BOOT ] --- BEGIN #"); shell_put_uint32(i); sh_puts(" ---\r\n");
        if (!bootscript_get(i, line, sizeof(line))) {
            sh_puts("[BOOT ] #"); shell_put_uint32(i); sh_puts(": FAILED to read slot (CRC corrupt?)\r\n");
            failed++;
            bootscript_rec_entry(i, "<slot CRC corrupt>", -999);
            sh_puts("[BOOT ] --- END #"); shell_put_uint32(i); sh_puts(" FAIL (read)\r\n");
            continue;
        }
        sh_puts("[BOOT ] #"); shell_put_uint32(i); sh_puts(": $ "); sh_puts(line); sh_crlf();
        /* 【hotfix2: shell_exec_line 内部会用 strtok/strsep 把空格换成 '\0'，
         *  所以在此之前先快照一份原始命令行，供 bootscript_rec_entry 记录使用。
         *  否则 boot status 里会看到 "led"（第一个 token）而不是完整的 "led on"。*/
        char line_snap[SHELL_LINE_SIZE];
        size_t slen = strlen(line);
        if (slen >= sizeof(line_snap)) slen = sizeof(line_snap) - 1u;
        memcpy(line_snap, line, slen); line_snap[slen] = '\0';
        int rc = shell_exec_line(line);
        bootscript_rec_entry(i, line_snap, rc);          /* ← RAM 记录：每条命令结果（完整原始行） */
        if (rc != 0) {
            sh_puts("[BOOT ] #"); shell_put_uint32(i); sh_puts(": exit code=");
            shell_put_err((int)rc); sh_crlf();
            failed++;
            sh_puts("[BOOT ] --- END #"); shell_put_uint32(i); sh_puts(" FAIL (exec)\r\n");
        } else {
            ok_cnt++;
            sh_puts("[BOOT ] --- END #"); shell_put_uint32(i); sh_puts(" OK\r\n");
        }
    }
    /* 回放结束，给用户视觉锚点：GPIO25 电平直接汇报（用户最关心的就是 LED 是否亮） */
    uint8_t gpio25_after = 0xFFu;
#if OS_CFG_PERIPH_SERVICE
    {
        hal_gpio_level_t lvl = hal_gpio_read(25);
        gpio25_after = (lvl == HAL_GPIO_HIGH) ? 1u : 0u;
        sh_puts("[BOOT ] GPIO25 (LED) level after bootscript = ");
        sh_puts((lvl == HAL_GPIO_HIGH) ? "HIGH (LED should be ON)\r\n" : "LOW (LED OFF)\r\n");
    }
#endif
    sh_puts("[BOOT ] Summary: ");
    shell_put_uint32((uint32_t)(total - (uint8_t)failed));
    sh_puts(" ok / ");
    shell_put_uint32((uint32_t)((failed < 0) ? 0 : failed));
    sh_puts(" failed (total="); shell_put_uint32(total); sh_puts(")\r\n");
    sh_puts("==============================================================\r\n");
    sh_puts("=====         BOOTSCRIPT DONE                                   =====\r\n");
    sh_puts("==============================================================\r\n");
    bootscript_rec_end(ok_cnt, (uint8_t)((failed < 0) ? 0 : failed), gpio25_after);
    return failed;
}

static int cmd_bootscript_save(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: save <command line...>\r\n  e.g. save i2c init 0 4 5 100000\r\n"); return 1; }
    char line[SHELL_LINE_SIZE];
    argv_rejoin(argc, argv, 1, line, sizeof(line));
    hal_err_t r = bootscript_append(line);
    if (r == HAL_OK) { uint8_t n = bootscript_count();
        sh_puts("OK: Saved as #"); shell_put_uint32((uint32_t)(n - 1));
        sh_puts(" (total "); shell_put_uint32(n); sh_puts("/32, Remaining: ");
        shell_put_uint32(32u - (uint32_t)n); sh_puts(" slots). Persistent across reset.\r\n"); return 0; }
    if (r == HAL_ERR_FULL)  sh_puts("ERROR: bootscript full (max 32 entries). Use 'unsave <idx>'.\r\n");
    else if (r == HAL_ERR_PARAM) sh_puts("ERROR: command too long (>123 B) or empty.\r\n");
    else if (r == HAL_ERR_NOMEM) sh_puts("ERROR: kmalloc 4KB staging failed.\r\n");
    else { sh_puts("ERROR: Flash write failed (code="); shell_put_err((int)r); sh_puts(").\r\n"); }
    return 1;
}

static int cmd_bootscript_unsave(int argc, char **argv) {
    if (argc < 2) { sh_puts("Usage: unsave <idx> | unsave all\r\n"); return 1; }
    if (strcmp(argv[1], "all") == 0) {
        hal_err_t r = bootscript_clear_all();
        if (r == HAL_OK) { sh_puts("OK: All persistent commands erased. Remaining: 32/32 slots (free).\r\n"); return 0; }
        sh_puts("ERROR: Flash erase failed (code="); shell_put_err((int)r); sh_puts(").\r\n"); return 1;
    }
    uint32_t idx;
    extern int shell_parse_uint_auto(const char *s, uint32_t *out);   /* same file, defined earlier */
    if (shell_parse_uint_auto(argv[1], &idx) != 0) { sh_puts("ERROR: bad index\r\n"); return 1; }
    uint8_t count = bootscript_count();
    if (idx >= count) { sh_puts("ERROR: index out of range (have "); shell_put_uint32(count); sh_puts(" entries)\r\n"); return 1; }
    hal_err_t r = bootscript_remove((uint8_t)idx);
    if (r == HAL_OK) { uint8_t left = bootscript_count();
        sh_puts("OK: Removed #"); shell_put_uint32(idx);
        sh_puts(" (used "); shell_put_uint32(left); sh_puts("/32, Remaining: ");
        shell_put_uint32(32u - (uint32_t)left); sh_puts(" slots)\r\n"); return 0; }
    sh_puts("ERROR: Flash write failed (code="); shell_put_err((int)r); sh_puts(").\r\n"); return 1;
}

static int cmd_bootscript_list(int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t n = bootscript_count();
    uint32_t rem = 32u - (uint32_t)n;
    sh_puts("Persistent commands (used "); shell_put_uint32(n);
    sh_puts(", free "); shell_put_uint32(rem); sh_puts("/32 slots, Flash 2x backup):\r\n");
    for (uint8_t i = 0; i < n; i++) {
        char line[SHELL_LINE_SIZE];
        if (!bootscript_get(i, line, sizeof(line))) {
            sh_puts("  #"); shell_put_uint32(i); sh_puts(": <CRC CORRUPT>\r\n"); continue;
        }
        sh_puts("  #"); shell_put_uint32(i); sh_puts(": "); sh_puts(line); sh_crlf();
    }
    if (n == 0) sh_puts("  (empty. Try: save i2c init 0 4 5 100000)\r\n");
    return 0;
}

static int cmd_bootscript_boot_exec(int argc, char **argv) {
    (void)argc; (void)argv;
    (void)bootscript_run_all();
    return 0;
}

/* ================================================================
 * 出厂重置：factory_reset [confirm]
 *   · 无参数：打印警告和影响范围，要求二次确认；
 *   · confirm：真正执行
 *       1) 清空 bootscript 双备份 (末尾 2×4KB，SEC_A/B)  → 调用 bootscript_clear_all()
 *       2) 再往回擦 14 个扇区（末尾共 16 个扇区 = 64KB，
 *          0x1F0000..0x1FFFFF 共 64KB），彻底消除 "用户层/保留区 其他应用残留数据"，
 *          但绝对不碰 Flash 起始区域（固件 .text/.rodata 都在最开头，当前固件仅 83KB）。
 *   · 成功后提示重新上电，系统回到"刚刷完固件、无任何固化指令/用户数据"的出厂状态。
 * ================================================================ */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES   (2u * 1024u * 1024u)
#endif
#define FACTORY_RESET_SECTORS   (16u)   /* 擦末尾 16 × 4KB = 64KB */

static int cmd_factory_reset(int argc, char **argv) {
    /* 第 1 步：无 confirm → 只打印警告 */
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        sh_puts("****************************************************************\r\n");
        sh_puts("*                    FACTORY RESET WARNING                     *\r\n");
        sh_puts("****************************************************************\r\n");
        sh_puts("  此命令将把系统恢复到刚刷完固件的出厂状态：\r\n");
        sh_puts("   ✓ 保留：内核 + demo_app 固件本身（操作系统完整保留）\r\n");
        sh_puts("   ✗ 清除：所有固化指令 (save / ! 保存的 bootscript 全部条目)\r\n");
        sh_puts("   ✗ 清除：板载 SPI Flash 最后 64KB 保留区 (16 sectors)\r\n");
        sh_puts("   ✗ 清除：任何应用层写入的持久化用户数据\r\n");
        sh_puts("  操作不可恢复！请确保已备份重要配置。\r\n");
        sh_puts("  要继续，请输入:  factory_reset confirm\r\n");
        sh_puts("****************************************************************\r\n");
        return 0;
    }

    sh_puts("[FACTORY_RESET] Starting... Will erase tail 64KB of SPI Flash (keep kernel FW).\r\n");

    /* 先快速清 bootscript 双备份（带 CRC 的 2 个扇区），之后即使断电也不会残留 */
    hal_err_t r = bootscript_clear_all();
    if (r != HAL_OK) {
        sh_puts("[FACTORY_RESET] FAIL: bootscript_clear_all returned ");
        shell_put_err((int)r); sh_crlf();
        return 1;
    }
    sh_puts("[FACTORY_RESET] Bootscript (2×4KB) cleared OK.\r\n");

    /* 再擦剩余 14 个扇区（共 16 个）：从 FACTORY_RESET_BASE 往上到末尾 */
    uint32_t base = (uint32_t)PICO_FLASH_SIZE_BYTES - (uint32_t)FACTORY_RESET_SECTORS * HAL_FLASH_SECTOR_SIZE;
    for (uint32_t s = 0; s < FACTORY_RESET_SECTORS; s++) {
        uint32_t off = base + s * HAL_FLASH_SECTOR_SIZE;
        /* 跳过 SEC_A/B 位置 —— 其实 bootscript_clear_all 里 bs_commit_both 已经擦过了，
         *   再擦一遍也没问题；但为了"进度汇报"统一，这里统一都擦并计数。 */
        hal_err_t er = hal_flash_erase_sector(off);
        if (er != HAL_OK) {
            sh_puts("[FACTORY_RESET] FAIL at sector "); shell_put_uint32(s);
            sh_puts(" (offset="); shell_put_hex32(off);
            sh_puts(") code="); shell_put_err((int)er); sh_crlf();
            return 1;
        }
        if (((s + 1) % 4) == 0 || s == FACTORY_RESET_SECTORS - 1) {
            sh_puts("[FACTORY_RESET] Erased "); shell_put_uint32(s + 1);
            sh_puts("/"); shell_put_uint32(FACTORY_RESET_SECTORS);
            sh_puts(" sectors (");
            shell_put_uint32((s + 1) * HAL_FLASH_SECTOR_SIZE / 1024u);
            sh_puts(" KB)\r\n");
        }
    }

    sh_puts("[FACTORY_RESET] DONE. System restored to factory state (v");
    sh_puts(k_version());
    sh_puts(").\r\n");
    sh_puts("[FACTORY_RESET] Please POWER-CYCLE (拔插 USB) or press RUN 键重新上电以进入全新状态。\r\n");
    return 0;
}

/* ================================================================
 * reboot：软复位系统
 *   写 AIRCR.SYSRESETREQ 触发系统复位 → 重新执行完整启动流程，
 *   从而重现首次开机画面（[CONFIG] 超频档位 + boot banner + MSC 状态）。
 *   若已 `ovclk save` 固化档位/多核，也会在重启后应用。
 * ================================================================ */
static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Rebooting now... (system will re-run full boot sequence)\r\n");
    /* 排空 CDC 输出后复位。此函数不返回。 */
    hal_system_reset();
    return 0;   /* 不可达 */
}

/* ================================================================
 * 执行单条命令（新增："!" 前缀糖衣）
 *   "! gpio init 25 out 1" → 先执行 "gpio init 25 out 1"，
 *                             成功 (rc=0) 再 save 到 Flash；
 *                             失败 (rc≠0) 就不 save，避免保存错误命令。
 * ================================================================ */
static int shell_exec_line(char *line) {
    /* 容错：自动跳过行开头的提示符残留（用户粘贴整个会话时最常出现）
     *   匹配前缀: "mk>" "mk> " ">" "> " "shell>" "shell> "
     *   只要前缀命中，指针 line 前进到其后的第一个非空白字符。
     *   这样用户粘贴 "mk> led on" 也能正确解析为命令 "led on"。 */
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "mk>", 3) == 0) {
            p += 3;
            if (*p == ' ') p++;
            line = p;
        } else if (strncmp(p, "shell>", 6) == 0) {
            p += 6;
            if (*p == ' ') p++;
            line = p;
        } else if (*p == '>') {
            p++;
            if (*p == ' ') p++;
            line = p;
        }
    }

    /* "!" 前缀糖衣：先执行命令，成功再固化 */
    bool bang_append_on_success = false;
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '!') {
            bang_append_on_success = true;
            p++;
            if (*p == ' ') p++;
            line = p;
        }
    }

    /* 为了 !<cmd> 语法糖能把剩余命令重新拼成命令行字符串保存，
     *   先 copy 一份"剥了 '!' 后的原始 line"，执行成功后直接用。 */
    static char bang_original[SHELL_LINE_SIZE];
    if (bang_append_on_success) {
        size_t ll = strlen(line);
        if (ll >= sizeof(bang_original)) ll = sizeof(bang_original) - 1;
        memcpy(bang_original, line, ll); bang_original[ll] = '\0';
    }

    static char *argv[SHELL_MAX_ARGS];
    bool overflowed = false;
    int argc = shell_split_argv(line, argv, SHELL_MAX_ARGS, &overflowed);
    if (overflowed) {
        sh_puts("WARNING: Too many tokens! Only first ");
        shell_put_uint32(SHELL_MAX_ARGS);
        sh_puts(" parsed. Increase SHELL_MAX_ARGS.\r\n");
    }
    if (argc == 0) return 0;   /* 空行 */
    const char *name = argv[0];
    int rc = 1;
    bool found = false;
    for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
        if (strcmp(g_cmd_table[i].name, name) == 0) {
            found = true;
            rc = g_cmd_table[i].handler(argc, argv);
            break;
        }
    }
    /* v2.2 新增：如果静态表没找到，查"扩展命令表"（msc/ls/cd/pwd/mkdir/rmdir/rm/cat）
     *   扩展函数签名多一个 ctx 参数（我们永远传 NULL）。 */
    if (!found) {
        shell_cmd_fn_ext_t fn = NULL;
        if (shell_ext_lookup(name, &fn, NULL, NULL) >= 0) {
            found = true;
            rc = fn(argc, argv, NULL);
        }
    }
    /* 只有命令表里完全没命中才打印 "Unknown command"；handler 返回 1 只是表示失败（此时
     *   handler 自己应该已经打印了具体错误信息），不能误报"未知命令"。*/
    if (!found) {
        sh_puts("Unknown command: ");
        sh_puts(name);
        sh_puts(" (try 'help')\r\n");
    }

    /* ! 糖衣：执行成功 → 追加 Flash；成功/失败都汇报剩余 slots */
    if (bang_append_on_success && rc == 0) {
        hal_err_t br = bootscript_append(bang_original);
        if (br == HAL_OK) {
            uint8_t n = bootscript_count();
            uint8_t rem = (uint8_t)(32u - (uint32_t)n);
            sh_puts("  → Auto-saved as #"); shell_put_uint32((uint32_t)(n - 1));
            sh_puts(" (persistent on Flash. Remaining: "); shell_put_uint32(rem); sh_puts("/32 slots)\r\n");
        } else {
            sh_puts("  → FAILED to save to Flash (code=");
            shell_put_err((int)br); sh_puts(")\r\n");
        }
    }
    return rc;
}

/* ================================================================
 * vtest: 三任务嵌套调度稳定性验证 (v2.2.5)
 *   · 三个并发任务嵌套依赖：VT3 操作 VT2 的状态，VT2 长时间 I2C 阻塞
 *     期间 VT1 继续跑 → 完整验证时间片轮转 / SLEEP 队列 /
 *     SUSPEND↔READY 状态切换 / 堆零碎片 / TCB 队列不损坏
 * ================================================================ */

/* — VT1: LED 心跳任务（500ms 翻转 GPIO25，与固化的 led 命令用同一 PIN 同一 SIO 寄存器）
 *   不依赖 OS_CFG_PERIPH_SERVICE，任何裁剪开关下都能看到 LED 状态，
 *   是调度器仍在正常轮转的"心跳指示器"。*/
static void vt_task_led(void *arg) {
    (void)arg;
    register const uint32_t SIO_BASE = 0xD0000000u;
    register const uint32_t MASK25   = 0x02000000u;
    uint8_t on = 0;
    while (g_vtest_running) {
        on = !on;
        if (on) {
            *(volatile uint32_t *)(SIO_BASE + 0x014) = MASK25;  /* OUT_SET */
        } else {
            *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;  /* OUT_CLR */
        }
        g_vt1_beats++;
        task_sleep(500);   /* 500ms at 1kHz tick — SLEEP 队列轮转验证 */
    }
    /* 被 kill 前：灭灯，避免一直亮着 */
    *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;
    /* g_vtest_running=0 → 退出循环后必须自挂起，不能 return（LR=0 → HardFault 爆闪）。
     * task_suspend 触发 PendSV 切走自己，等 shell task_destroy 安全销毁。 */
    task_suspend(g_current_task);
    while (1) {}
}

/* — VT2: OLED 周期性刷新任务（每 2s 写一帧 1024B SSD1306 GDRAM）
 *   帧内容：8 行 × 128 列棋盘格，叠加 g_vt2_frames 计数 byte，
 *   这样肉眼能看到 OLED 在持续变化（即使没有逻辑分析仪）。
 *   I2C 用 5 个 chunk × 255B 发送（与 i2c fill 命令完全相同）。
 *   如果 OLED 初始化命令没跑（用户没 save 那 17 条 cmds），
 *   这里也会 NACK 但不会崩，用户从 g_vt2_errs 可看到。*/

/* ================================================================
 * 【v2.3.2 · 0.96" I2C OLED 最底层驱动 — 文件作用域 + 纯静态缓冲】
 *   v2.3.1 三个函数内部用 uint8_t txbuf[] / c[] 在栈上分配，在
 *   Cortex-M0+ 上偶发 HardFault。现在：
 *     · txbuf、cbuf 全部放 BSS（静态数组）
 *     · 所有调用方传入的数组也必须是静态或全局（不占栈）
 * ================================================================ */
#define VT2_MAX_CMD_BYTES  8   /* 单条命令 + 参数最多 8 字节 */

/* 全部缓冲放静态 BSS（16B + 3B = 19B，零栈开销）*/
static uint8_t g_vt2_txbuf[VT2_MAX_CMD_BYTES * 2];   /* 命令编码用：16B */
static uint8_t g_vt2_addrc[3];                        /* set_addr 编码用：3B */

/* I2C 引脚 PADS 寄存器（文件作用域，reinit 用）*/
#define VT2_PADS_BANK0_BASE   0x4001C000u
#define VT2_PADS_GPIO(n)      (*(volatile uint32_t *)(VT2_PADS_BANK0_BASE + 0x04u + 4u*(n)))
#define VT2_PADS_PUE          (1u << 3)
#define VT2_PADS_PDE          (1u << 2)

/* 【v2.3.3 · vt2_i2c_reinit : I2C 外设硬复位】
 *   VT2 在 i2c_write_timeout_us 传输中途被 VT3 suspend 3s：
 *     · 超时用绝对时间 time_us_32() → 恢复后立即超时返回；
 *     · I2C 状态机停在半途（START 已发/TX FIFO 残留/从机等字节流），
 *       后续所有传输 NACK，单纯重试永远失败（实测 page 1..7 全挂）。
 *   唯一可靠恢复：hal_i2c_init 重置外设（幂等，i2c_init 会 reset 外设 +
 *   重新使能）+ 重配上拉。重试失败路径都先调本函数。*/
static void vt2_i2c_reinit(void)
{
    uint32_t bus = (uint32_t)g_vt_oled_bus;
    uint8_t sda_pin = (g_vt_oled_bus == 0) ? 4 : 6;
    uint8_t scl_pin = (g_vt_oled_bus == 0) ? 5 : 7;
    hal_i2c_init(bus, 400000);
    VT2_PADS_GPIO(sda_pin) = (VT2_PADS_GPIO(sda_pin) | VT2_PADS_PUE) & ~VT2_PADS_PDE;
    VT2_PADS_GPIO(scl_pin) = (VT2_PADS_GPIO(scl_pin) | VT2_PADS_PUE) & ~VT2_PADS_PDE;
}

/* (1) oled_tx_cmds : 发送命令字节流（16b 编码 + Co=1/0 + 失败 reinit 重试）
 *   注意：cmds 可以是栈数组（本函数内部先复制到静态 txbuf，再发 I2C）*/
static hal_err_t vt2_oled_tx_cmds(const uint8_t *cmds, int ncmds)
{
    if (ncmds <= 0) return HAL_OK;
    if (ncmds > VT2_MAX_CMD_BYTES) return HAL_ERR_PARAM;
    for (int i = 0; i < ncmds; i++) {
        g_vt2_txbuf[2 * i    ] = (i == ncmds - 1) ? 0x00 : 0x80;
        g_vt2_txbuf[2 * i + 1] = cmds[i];
    }
    for (int retry = 0; retry < 3; retry++) {
        hal_err_t r;
        /* 【v2.3.6 · 单事务原子性修复 —— 根治 >30 轮轮询崩溃】
         *  VT3 每 10s 对 VT2 做 task_suspend/resume。若 VT2 恰好在
         *  hal_i2c_tx(→i2c_write_timeout_us) 的中途被挂起 3s，SDK 的
         *  超时是"调用起点算起的 200ms 绝对截止"，挂起期间截止已过，
         *  恢复后立即判超时 → HAL_ERR_IO → 触发 reinit 复位外设。
         *  每轮这样累积，约 30 轮后外设状态/堆被反复复位打乱 → HardFault。
         *  修复：用 PRIMASK 临界区包住"单次 I2C 事务"，使 PendSV 上下文
         *  切换无法打断一次 6~133B 的 START..STOP 事务（400kHz 下最多
         *  ~3ms 关中断，可接受）。VT3 的 suspend 在 VT2 完成本次事务、
         *  重新开中断后才生效，VT2 永远不会带着过期截止指针被挂起。*/
        uint32_t __pmask;
        __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
        __asm volatile ("cpsid i" ::: "memory");
        r = hal_i2c_tx((uint32_t)g_vt_oled_bus, (uint8_t)g_vt_oled_addr,
                       g_vt2_txbuf, (size_t)(2 * ncmds));
        __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
        if (r == HAL_OK) return HAL_OK;
        vt2_i2c_reinit();   /* 外设可能被 suspend 打坏，硬复位再试 */
        task_sleep(2);
    }
    return HAL_ERR_IO;
}

/* (2) oled_set_addr : 设置 GRAM 写指针 = (page, col) 【旧式寻址，偏移=0】
 *   【v2.3.4 · 列偏移修复】实测用户屏映射为 col 0..127（非 132 列屏的
 *     col 2..129），写死 +2 会导致最左 2 列永远写不到 → 左侧残留小黑点/
 *     小白点。通用做法：从 col 0 开始写满 132B（VT2_OLED_COLS），
 *     SH1106 132 列全覆盖、SSD1306 128 列 + 4B wrap 回本页 col 0（无害）。
 *     任何列映射下整屏都会被刷新。*/
#define VT2_OLED_COLS  132   /* 写满整行：SH1106=132 列全覆盖，SSD1306=128+4 wrap */
static hal_err_t vt2_oled_set_addr(uint8_t page, uint8_t col)
{
    g_vt2_addrc[0] = (uint8_t)(0xB0 | ((uint8_t)(page & 0x07)));
    g_vt2_addrc[1] = (uint8_t)(0x00 | ((uint8_t)(col  & 0x0F)));
    g_vt2_addrc[2] = (uint8_t)(0x10 | ((uint8_t)((col >> 4) & 0x0F)));
    return vt2_oled_tx_cmds(g_vt2_addrc, 3);
}

/* (3) oled_write_block : 写 GRAM 数据块（首字节=0x40 + 失败 reinit 重试）*/
static hal_err_t vt2_oled_write_block(const uint8_t *block, int block_len)
{
    if (block_len <= 0) return HAL_OK;
    for (int retry = 0; retry < 3; retry++) {
        hal_err_t r;
        /* 【v2.3.6 · 单事务原子性】同 vt2_oled_tx_cmds：PRIMASK 临界区防止
         *  VT3 的 suspend 在 i2c_write_timeout_us 中途打断，避免 200ms 绝对
         *  截止在 3s 挂起期间过期 → 恢复即超时 → 累积 reinit → HardFault。*/
        uint32_t __pmask;
        __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
        __asm volatile ("cpsid i" ::: "memory");
        r = hal_i2c_tx((uint32_t)g_vt_oled_bus, (uint8_t)g_vt_oled_addr,
                       block, (size_t)block_len);
        __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
        if (r == HAL_OK) return HAL_OK;
        vt2_i2c_reinit();   /* 同 vt2_oled_tx_cmds 的恢复策略 */
        task_sleep(2);
    }
    return HAL_ERR_IO;
}
#undef VT2_MAX_CMD_BYTES
#undef VT2_PADS_BANK0_BASE
#undef VT2_PADS_GPIO
#undef VT2_PADS_PUE
#undef VT2_PADS_PDE
/* ================================================================
 * End of 0.96" I2C OLED bottom driver
 * ================================================================ */
static void vt_task_oled(void *arg) {
    (void)arg;
    /* 【v2.2.10 修复】启动延迟：等 shell 打印完 "OK: vtest STARTED" +
     *   Validation checklist（约 500 字符）。否则 VT2 输出和 shell 输出
     *   严重交错（"ta[VTsks running"），且 console 竞争可能导致状态损坏。
     *   用 1500ms 错开 VT3 的 2000ms，避免两者同时唤醒争抢 async 锁。*/
    task_sleep(1500);

    /* 1024B frame buffer — 使用全局静态 g_vt2_block[]（不占栈）。
     * 256B chunk buffer，按 8 行 × 128 列 动态生成分块发送。SSD1306 的列指针在
     * horizontal addressing mode 下自动 wrap，不会因为分 5 次
     * START 断续发送 D/C=1 流而错位。*/
    g_vt2_block[0] = 0x40;        /* D/C=1: 后续为 GDRAM 数据 */

    /* 【v2.2.9 修复 · I2C 自初始化】
     *   旧版依赖 bootscript 里的 `i2c init 0 4 5 100000` 来初始化 I2C 外设。
     *   用户删了该固化命令后，I2C 外设未初始化、GPIO 未复用 →
     *   i2c_write_blocking 无超时地永久阻塞 → VT2 卡死 → VT3 suspend/resume
     *   一个卡死的任务导致状态混乱 → 崩溃。
     *
     *   修复：VT2 启动时自己调 hal_i2c_init + 手动配置 GPIO 上拉，
     *   不依赖 bootscript。init 是幂等的（重复调 i2c_init 只重置外设），
     *   所以即使 bootscript 也有 i2c init 也不会冲突。
     *
     * 【v2.2.11 · 栈溢出静默挂起排查】
     *   用户报告：VT2 打印完 "hal_i2c_init ret=0" 后静默死亡（无 HardFault 爆闪，
     *   shell 正常运行，说明是 tick_hook 的 STACK_OVF_CHECK 把 VT2 挂起了）。
     *   在每个 I/O 里程碑打印当前栈使用量，定位是哪一步踩了 4×MAGIC 金丝雀。*/
    hal_err_t ri = HAL_OK;
    {
        /* I2C0 默认引脚: GP4=SDA, GP5=SCL */
        uint8_t sda_pin = (g_vt_oled_bus == 0) ? 4 : 6;
        uint8_t scl_pin = (g_vt_oled_bus == 0) ? 5 : 7;
        /* 【v2.3.5 · 100k → 400kHz】SSD1306 官方支持 Fast-mode。
         *   100kHz 时一帧 8 页 × 139B ≈ 1112B × 9bit ≈ 100ms（纯总线时间），
         *   叠加时间片切换导致的 FIFO 停等（VT2 每片只喂一点），
         *   肉眼看就是"逐行慢慢刷"。400kHz 后总线时间降到 ~25ms，
         *   一帧几乎瞬间完成。上拉是 RP2040 内部 50kΩ，400k 下拉电流
         *   约 (3.3V/50k)=66µA，上升沿稍缓但对 10~20pF 负载足够。*/
        ri = hal_i2c_init((uint32_t)g_vt_oled_bus, 400000);
        /* 开内部上拉（同 cmd_i2c init 的 PADS_BANK0 逻辑）*/
        #define PADS_BANK0_BASE_   0x4001C000u
        #define PADS_GPIO_(n)      (*(volatile uint32_t *)(PADS_BANK0_BASE_ + 0x04u + 4u*(n)))
        #define PADS_PUE_          (1u << 3)
        #define PADS_PDE_          (1u << 2)
        PADS_GPIO_(sda_pin) = (PADS_GPIO_(sda_pin) | PADS_PUE_) & ~PADS_PDE_;
        PADS_GPIO_(scl_pin) = (PADS_GPIO_(scl_pin) | PADS_PUE_) & ~PADS_PDE_;
        #undef PADS_BANK0_BASE_
        #undef PADS_GPIO_
        #undef PADS_PUE_
        #undef PADS_PDE_
    }
    {
        shell_async_enter();
        sh_puts("[VT2] hal_i2c_init ret="); shell_put_uint32((uint32_t)ri);
        sh_puts("  stack_used=");
        shell_put_uint32((uint32_t)task_stack_used(g_current_task));
        sh_puts("B\r\n");
        shell_async_exit();
    }

    /* 底层驱动函数已移到文件作用域（v2.3.2），此处直接调用 */

    /* 【SSD1306 初始化命令序列】
     *   每条命令单独发一个 I2C 事务 (0x00 前缀 + 命令)。
     *   每 3 条命令汇总打印一次 ret 位图，避免 console 过度竞争；
     *   若某条命令返回非 0，立即单独打印该条，精准定位卡死/超时位置。*/
    {
        /* 【v2.2.12 修复：SSD1306 cmd[11] NACK → 对比度紧跟电荷泵 + STOP→START 延时】
         *   用户现场：cmds[0..8] 全部 OK，唯有 cmd[11] = Contrast(0x81,0xFF) NACK。
         *   原因有二：
         *     (a) 便宜 SSD1306 clone 对 STOP→START 间隔有严格要求（≥1.3μs bus free），
         *         原来组内 3 条命令连续调用 hal_i2c_tx 无任何间隔，第 3 条容易 NACK；
         *     (b) Contrast(0x81) 部分 clone 要求紧跟 Charge Pump(0x8D,0x14) 之后，
         *         不允许被 Addressing/Remap 命令隔开，否则偶发 NACK。
         *   修复：
         *     · (a) 每条命令后 busy-wait 200 cycles（RP2040 125MHz ≈ 1.6μs ≥ 1.3μs）
         *     · (b) Contrast 移到 cmd[6]，紧跟 Charge Pump(cmd[5])
         *     · (c) 保存 first_err_ret，打印不再用 last_ret(最后一条 cmd)做假阳性
         *   其他命令与 I2C 调用链完全不变。*/
        /* 【v2.3 · 初始化命令按网上通用 0.96" OLED 写法重排】
         *   移除 SH1106/便宜 clone 常 NACK 的 0x21(列范围)/0x22(页范围) 三字节命令，
         *   寻址全部交给 vt2_oled_set_addr 的旧式 0xB0/0x00/0x10 三条单命令。
         *   命令顺序严格按 Adafruit SSD1306 / U8g2 默认序列：
         *     OFF → 时钟 → MUX → 偏移 → StartLine → ChargePump → MemoryMode(页模式)
         *     → Segment/COM → COMpins → Contrast → PreCharge → VCOMH → Resume → Normal → ON
         *   page addressing mode (0x20, 0x02) 是最简单、兼容性最好的模式，
         *   也是 SH1106 默认模式，完全不依赖 0x21/0x22 范围命令。*/
        static const uint8_t cmds[][4] = {
            {1, 0xAE},              /* 0: Display OFF */
            {2, 0xD5, 0x80},        /* 1: Display clock divide */
            {2, 0xA8, 0x3F},        /* 2: Multiplex ratio: 64 (128x64) */
            {2, 0xD3, 0x00},        /* 3: Display offset: 0 */
            {1, 0x40},              /* 4: Display start line: 0 */
            {2, 0x8D, 0x14},        /* 5: Charge pump enable → 下一条必须 Contrast */
            {2, 0x81, 0xFF},        /* 6: Contrast max (紧跟 Charge Pump) */
            {2, 0x20, 0x02},        /* 7: Memory addressing: PAGE MODE (兼容性最高) */
            {1, 0xA1},              /* 8: Segment remap: 列镜像 (SEG127=列0) */
            {1, 0xC8},              /* 9: COM scan direction: 倒序 (COM63=行0) */
            {2, 0xDA, 0x12},        /* 10: COM pins: 128x64 = 0x12 (Alternative COM) */
            {2, 0xD9, 0xF1},        /* 11: Pre-charge period */
            {2, 0xDB, 0x40},        /* 12: VCOMH deselect: 0x40 = 0.77xVCC */
            {1, 0xA4},              /* 13: Display resume (from 0xA5 all-on) */
            {1, 0xA6},              /* 14: Normal display (非 0xA7 反色) */
            {1, 0xAF},              /* 15: Display ON */
        };
        const int ncmds = (int)(sizeof(cmds) / sizeof(cmds[0]));
        g_vt2_cbuf[0] = 0x00;  /* Co=0, D/C=0: 后续为命令（全局静态，不占栈）*/
        uint32_t err_bmp = 0u;  /* bit i = 1 表示第 i 条命令失败 */
        int first_err = -1;
        hal_err_t first_err_ret = HAL_OK;
        hal_err_t last_ret = HAL_OK;

        for (int i = 0; i < ncmds; i++) {
            int n = (int)cmds[i][0];
            /* 【v2.3 · 切到新驱动：vt2_oled_tx_cmds 自动 16b 编码 + Co=1/0
             *   不再手工拼 g_vt2_cbuf[0]=0x00 前缀，驱动层统一处理。*/
            hal_err_t r = vt2_oled_tx_cmds(&cmds[i][1], n);
            last_ret = r;
            if (r != HAL_OK) {
                err_bmp |= (1u << (i < 31 ? i : 31));
                if (first_err < 0) { first_err = i; first_err_ret = r; }
            }
            /* 【v2.2.13 回退：移除每条命令 200-cycle busy-wait。
             *   上一轮怀疑 clone 需要 BUS FREE ≥1.3μs，但实测 busy-wait 反而导致
             *   cmd[5]=0x8D charge pump 返回 HAL_ERR_IO(-7) 且 shell_async 日志
             *   出现 "] OK=" 字符丢失（cmds[0..21 / cmds[3..50 拼接）。改回 RP2040
             *   SDK i2c_write_timeout_us 自带的 STOP->START 间隔处理。】
             *
             *   若后续仍出现某条命令 NACK，则单条命令之间用 task_sleep(1) 来
             *   保证延时，而不是 busy-wait（busy-wait 会让 USB CDC tick 得不到
             *   处理导致 shell_async 字符丢失）。*/

            /* 每 3 条命令打印一次进度（如果有错误单独打印那条）*/
            if ((i + 1) % 3 == 0 || i == ncmds - 1) {
                shell_async_enter();
                sh_puts("[VT2] cmds[");
                shell_put_uint32((uint32_t)(i - ((i + 1) % 3 == 0 ? 2 : i % 3)));
                sh_puts(".."); shell_put_uint32((uint32_t)i);
                /* OK 位图语义修改：当前组内 3 条命令全部成功才 OK=1，
                 * 不再拿"累计 err_bmp!=0"标整组失败（旧逻辑会把后面无辜组全标 0）*/
                {
                    uint32_t group_mask = 0u;
                    int gstart = (i >= 2) ? (i - 2) : 0;
                    for (int k = gstart; k <= i; k++) group_mask |= (1u << k);
                    uint32_t group_err = err_bmp & group_mask;
                    shell_put_uint32((uint32_t)(group_err == 0u));
                }
                sh_puts("  stk=");
                shell_put_uint32((uint32_t)task_stack_used(g_current_task));
                sh_puts("B\r\n");
                shell_async_exit();
                /* 每组之间短暂 yield，避免长时间占着 CPU */
                task_sleep(5);
            }
        }
        task_sleep(50);

        if (first_err >= 0) {
            shell_async_enter();
            sh_puts("[VT2] FIRST FAIL at cmd[");
            shell_put_uint32((uint32_t)first_err);
            sh_puts("]="); shell_print_hex8(cmds[first_err][1]);
            sh_puts("  ret="); shell_put_uint32((uint32_t)first_err_ret);
            sh_puts("  err_bmp=0x"); shell_put_hex32(err_bmp);
            sh_puts("\r\n");
            shell_async_exit();
        } else {
            shell_async_enter();
            sh_puts("[VT2] SSD1306 init OK (16 cmds)  stk=");
            shell_put_uint32((uint32_t)task_stack_used(g_current_task));
            sh_puts("B\r\n");
            shell_async_exit();
        }
    }

    /* 第一帧先发全白测试 */
    hal_err_t r0 = HAL_OK;
    uint32_t white_errs = 0u;
    {
        /* 【v2.3.4 · 写满 132 列】0x40 + 132B×0xFF，覆盖任何列映射 */
        g_vt2_block[0] = 0x40;
        for (int i = 1; i <= VT2_OLED_COLS; i++) g_vt2_block[i] = 0xFF;

        shell_async_enter();
        sh_puts("[VT2] white frame: start (8 pages)  stk=");
        shell_put_uint32((uint32_t)task_stack_used(g_current_task));
        sh_puts("B\r\n");
        shell_async_exit();

        for (uint8_t p = 0; p < 8; p++) {
            hal_err_t r1 = vt2_oled_set_addr(p, 0);
            if (r1 != HAL_OK) { white_errs++; r0 = r1; continue; }
            hal_err_t r4 = vt2_oled_write_block(g_vt2_block, VT2_OLED_COLS + 1);
            if (r4 != HAL_OK) { white_errs++; r0 = r4; }
        }
    }
    {
        shell_async_enter();
        sh_puts("[VT2] white frame: reset_addr ret=");
        shell_put_uint32((uint32_t)r0);
        sh_puts("  chunk_errs="); shell_put_uint32(white_errs);
        sh_puts("  stk=");
        shell_put_uint32((uint32_t)task_stack_used(g_current_task));
        sh_puts("B — screen should be ALL WHITE\r\n");
        shell_async_exit();
    }

    while (g_vtest_running) {
        uint32_t frame = g_vt2_frames;  /* 当前帧号（决定图案）*/

        /* 【I2C 刷新优化 · 1: 按 SSD1306 页边界分块】
         *   旧版分 5 块 (255/255/255/255/4)，第 5 块仅 4B，START/STOP 协议开销
         *   与 256B 块相同，浪费；且块边界不与 OLED 8×128 的 8 页对齐，
         *   部分便宜 SSD1306 clone 在跨页 STOP 后指针 wrap 偶发错位，
         *   表现为"某些行没更新/刷新慢"。
         *   修复：8 页 × 每页 128B = 8 个块，每块正好对应 OLED 一页。
         *   每次事务：SET PAGE (page_addr_cmd 3B+控制前缀) + DATA (129B)
         *   = 2 次 START/STOP/页，共 16 次事务（旧版是 5+reset_addr=6 次，
         *   反而多了？但每页独立设页地址完全避免 clone 指针错位 bug，
         *   对 128×64 = 1024B 总数据量来说开销可忽略：
         *     16 次 START/STOP × 40us ≈ 640us / 帧，占 2s 周期的 0.03%。*/

        /* 【v2.2.11 · OLED 自检帧：判断分辨率与硬件映射】
         *   用户："OLED 显示真的有问题吧？"——先给 3 帧确定性诊断图案，
         *   每一页（8 行 = 8 Page）写入完全不同的 1 字节 pattern：
         *     · Page 0 (最顶部): 0x01 = 每 8 像素仅最顶上 1bit 亮 = 最顶一条细线
         *     · Page 1: 0x02 = 第 2 条细线  …
         *     · Page 7 (最底部): 0x80 = 最底一条细线
         *   现象对应关系（用户肉眼可判断）：
         *     · 看到 8 条等距横线贯穿整个屏幕宽度 → 128×64 屏幕、配置正确 ✓
         *     · 只看到 4 条线（上面 4 根）、下面一半全黑 → 屏幕是 128×32，
         *       当前的 MUX=0x3F(64) / COM=0x12 不对，需改 0x1F/0x02
         *     · 某几 Page 不亮/雪花/乱码 → 该页 I2C 写入失败或指针错
         *     · 完全黑屏 → 初始化(电荷泵/Display ON) 没生效 */
        /* 【v2.3.2 · page_fill 移到静态 BSS（原栈数组）】*/
        static uint8_t page_fill[8];
        if (frame < 3u) {
            /* 前 3 帧 = 自检 8 条线：每 Page 一个 bit pattern */
            page_fill[0] = 0x01; page_fill[1] = 0x02;
            page_fill[2] = 0x04; page_fill[3] = 0x08;
            page_fill[4] = 0x10; page_fill[5] = 0x20;
            page_fill[6] = 0x40; page_fill[7] = 0x80;
        } else {
            /* 第 4 帧起：偶数帧全白(0xFF)，奇数帧全黑(0x00)，做 2s 切换验证 */
            uint8_t fill = (frame & 1u) ? 0x00u : 0xFFu;
            for (int p = 0; p < 8; p++) page_fill[p] = fill;
        }

        /* 【I2C 刷新优化 · 2: 每页独立旧式寻址（SSD1306 clone/SH1106 兼容）
         *   v2.2.14 因用户屏对 0x21/0x22 双参数命令 NACK，改回老式单参数寻址：
         *     · SET PAGE=p     : 0xB0 + p
         *     · SET COL LOW=0  : 0x00 + (0 & 0x0F) = 0x00
         *     · SET COL HIGH=0 : 0x10 + (0 >> 4)  = 0x10
         *   每页独立写地址+数据，不依赖 STOP 间指针连续性，也不依赖
         *   0x20/0x21/0x22 扩展命令是否被识别。
         *   帧首 col_rng 一次性设置已删除（现在每页设一次 COL=0，共 8 次）。*/
        /* 准备页发送缓冲：g_vt2_block[0] = 0x40 (D/C=1, Co=0 → 后续全是数据)
         *   【v2.3.4】写满 VT2_OLED_COLS=132B，覆盖任何列映射（见 set_addr 注释）。*/
        g_vt2_block[0] = 0x40;

        for (uint8_t p = 0; p < 8; p++) {
            if (!g_vtest_running) break;

            /* 步骤 A: 旧式寻址 — 完全复用 vt2_oled_set_addr（col=0 起）
             *   自动 16 位编码 + Co=1/0，应用层不拼 0x80/0x00 前缀。*/
            {
                hal_err_t r1 = vt2_oled_set_addr(p, 0);
                if (r1 != HAL_OK) g_vt2_errs++;
            }
            /* 步骤 B: 本页 132B 数据 — 完全复用 vt2_oled_write_block
             *   g_vt2_block[1..132] = page_fill[p]（前 3 帧每页 1bit 做自检线；
             *                              之后统一全白/全黑）*/
            {
                for (int i = 1; i <= VT2_OLED_COLS; i++) g_vt2_block[i] = page_fill[p];
                hal_err_t r2 = vt2_oled_write_block(g_vt2_block, VT2_OLED_COLS + 1);
                if (r2 != HAL_OK) g_vt2_errs++;
            }
        }

        g_vt2_frames++;
        /* 【I2C 刷新优化 · 3: 帧间隔保持 2000ms（vtest checklist 要求"每 2s 变化"）
         *   若想更快刷新，改成 500 即可，下方一行是唯一要改的位置。*/
        task_sleep(2000);   /* 2s at 1kHz tick — SLEEP 队列轮转验证；改这里加速 */
    }
    /* g_vtest_running=0 → 自挂起，等 shell task_destroy 销毁（不能 return） */
    task_suspend(g_current_task);
    while (1) {}
}

/* — VT3: 压力 + 嵌套控制任务（每 10s 一轮）
 *   每一轮：
 *     1) kmalloc 8 块不同大小 → 写数据 → 乱序 kfree（验证堆零碎片）
 *     2) 如果 VT2 还在跑：task_suspend(VT2) → 等 3s → resume(VT2)
 *        (VT2 被 suspend 的这 3s，OLED 应该停止刷新，帧号不涨，但
 *         VT1 心跳还在继续 —— 这就是"嵌套控制 + SUSPEND 不影响其他任务")
 *     3) 所有计数器 +1，进下一轮
 *
 *   ⚠️ 运行日志直接打印到串口（因为这是专门的验证任务，用户手动 start 触发的），
 *   与"普通交互模式下仅命令触发输出"不冲突。 */
static void vt_task_ctrl(void *arg) {
    (void)arg;
    #define VT3_NMEM 8
    /* 3 tasks 建立后剩余 heap≈800~950B，必须保证 8 块总和 + header 留 200B 余量。
     * 旧版 864B 太紧 → Round #1 kmalloc 失败 + p[] 未清零 = kfree 野指针 = HardFault 爆闪。*/
    size_t sizes[VT3_NMEM] = { 16, 32, 64, 32, 96, 24, 48, 32 };  /* 总和 = 344B，永远够 */
    int    order[VT3_NMEM] = { 3,0,7,2,5,1,6,4 };

    /* 【v2.2.10 修复 · 启动延迟】
     *   VT3 创建后 shell 还在打印 "OK: vtest STARTED..." + Validation checklist
     *   （约 500 字符）。若 VT3 立即跑 Round #1，shell_async_enter 会清掉
     *   shell 未打完的行 → 输出交错（"free_before=7TARTED"）。
     *   用 2000ms 错开 VT2 的 1500ms：VT2 此时已完成 I2C 初始化和首帧，
     *   VT3 才开始 Round #1，避免两者同时争抢 async 锁导致状态损坏。*/
    task_sleep(2000);

    while (g_vtest_running) {
        g_vt3_rounds++;

        /* ── Phase 1: 计算（可被安全抢占，不持锁）── */
        size_t k_used = task_kernel_stack_used();
        size_t u_used = task_user_stack_used();
        size_t pool   = task_total_stack_pool();
        uint8_t t_pct = task_total_stack_used_pct();
        int    stack_low = task_total_stack_is_low();

        void *p[VT3_NMEM] = {0};
        size_t free_before = kmem_free_size();
        size_t max_before  = kmem_max_free_block();
        int ok = 1;
        int fail_at = -1;
        for (int i = 0; i < VT3_NMEM; i++) {
            p[i] = kmalloc(sizes[i]);
            if (!p[i]) { ok = 0; fail_at = i; break; }
            memset(p[i], (int)(g_vt3_rounds + i + 0x33u), sizes[i]);
        }
        for (int i = 0; i < VT3_NMEM; i++) {
            if (p[order[i]]) kfree(p[order[i]]);
        }
        size_t free_after = kmem_free_size();
        size_t max_after  = kmem_max_free_block();

        /* ── Phase 2: 原子打印（console 自旋锁，不关中断）
         *   【v2.2.9 修复 · USB CDC FIFO 死锁根因】
         *   旧版用 PRIMASK 关中断 + shell_async_enter/exit 包裹整段输出。
         *   TinyUSB CDC TX FIFO（64B）满时，tud_cdc_write_char busy-wait
         *   等 USB IN ACK 中断清 FIFO → **关中断下永远等不到** → 卡死。
         *
         *   修复：去掉外层 PRIMASK，只靠 hal_console_putc 的单核自旋锁
         *   保证"每次 putc 原子"。shell_async_enter/exit 用于把
         *   readline 的 prompt + 用户输入先清掉再恢复，防止视觉上的
         *   行级错乱。即使中途被抢占（其他任务的 putc 会被自旋拦在外面），
         *   整段 VT3 输出仍然是字符级连续的。*/
        {
            shell_async_enter();
            sh_puts("[VT3] Round #"); shell_put_uint32(g_vt3_rounds); sh_crlf();

            sh_puts("[VT3]   Stack: kernel=");
            shell_put_uint32((uint32_t)k_used);
            sh_puts("B  user=");
            shell_put_uint32((uint32_t)u_used);
            sh_puts("B  total=");
            shell_put_uint32((uint32_t)(k_used + u_used));
            sh_puts("/");
            shell_put_uint32((uint32_t)pool);
            sh_puts("B (");
            shell_put_uint32((uint32_t)t_pct);
            sh_puts("%)");
            if (stack_low) {
                sh_puts("  !! STACK LOW — total exceeds ");
                shell_put_uint32((uint32_t)OS_CFG_USER_STACK_WARN_PCT);
                sh_puts("% !!");
            }
            sh_crlf();

            sh_puts("[VT3]   Heap: free_before=");
            shell_put_uint32(free_before);
            sh_puts("B → after="); shell_put_uint32(free_after); sh_puts("B ");
            if (ok && free_before == free_after && max_before == max_after) {
                sh_puts("[ZERO-FRAGMENT OK]");
            } else {
                sh_puts("[WARN: diff=");
                shell_put_uint32((free_after > free_before) ?
                                 (free_after - free_before) :
                                 (free_before - free_after));
                sh_puts("B");
                if (!ok) {
                    sh_puts(" (NOMEM at block #");
                    shell_put_uint32((uint32_t)fail_at);
                    sh_puts(", size="); shell_put_uint32(sizes[fail_at]);
                    sh_puts("B)");
                }
                sh_puts("]");
            }
            sh_crlf();
            shell_async_exit();
        }

        /* ── ② 嵌套 suspend/resume VT2（如果 VT2 还存活） ──────── */
        if (g_vt2) {
            /* 短消息同样：只靠 shell_async 清/恢复 prompt，不再关中断 */
            shell_async_enter();
            sh_puts("[VT3]   Suspend VT2 (oled_refresh) — OLED should stop for 3s.\r\n");
            shell_async_exit();

            task_suspend(g_vt2);
            g_vt3_susps++;
            /* 3s 内：VT2 是 SUSPEND，OLED 不应再刷新新帧；VT1 必须继续跳 */
            task_sleep(3000);
            /* 【v2.2.7】停止复查：stop_all 已唤醒本任务，此时 g_vt2 可能已被
             * 销毁（g_vt2=NULL）或正在销毁——绝不能再 task_resume(悬空句柄)。
             * 直接跳出循环走自挂起停靠。 */
            if (!g_vtest_running) break;

            shell_async_enter();
            sh_puts("[VT3]   Resume VT2 (oled_refresh) — OLED should continue.\r\n");
            shell_async_exit();

            task_resume(g_vt2);
            g_vt3_rsms++;
        } else {
            shell_async_enter();
            sh_puts("[VT3]   VT2 not alive, skip suspend/resume (check OLED init commands).\r\n");
            shell_async_exit();
        }

        /* 下一轮：离上一轮起点约 10s（不含上面 suspend 已占的 3s，再等 7s）*/
        task_sleep(7000);
    }
    /* g_vtest_running=0 → 自挂起，等 shell task_destroy 销毁（不能 return） */
    task_suspend(g_current_task);
    while (1) {}
    #undef VT3_NMEM
}

/* vtest_stop_all: 提取的停止+清理逻辑，供 cmd_vtest stop 和 Ctrl+C 共用
 *
 *   【v2.2.7 修复 · vtest stop 爆闪根因】
 *   旧版：置 flag → 唤醒 → 盲等 5ms → 直接 destroy。
 *   问题：被唤醒的任务并不保证 5ms 内停靠——
 *     · VT2 恢复在 hal_i2c_tx 的 chunk 循环里，剩余 4 chunk ≈ 90ms；
 *     · VT3 恢复在 task_sleep(3000) 内，还会执行 task_resume(g_vt2)
 *       （读可能已释放的 TCB）再睡 7s。
 *   于是 destroy 时任务可能在任意状态，且与 VT3 垂死代码形成 use-after-free
 *   竞态窗口。改为：轮询等待三任务全部停靠（SUSPEND/不存在），上限 200ms；
 *   超时则放弃 destroy（宁可泄漏 TCB 也不冒险踩 freed 内存）。 */
static void vtest_stop_all(void) {
    g_vtest_running = 0;
    if (g_vt2 && g_vt2->state == TASK_STATE_SUSPEND) task_resume(g_vt2);
    if (g_vt1) task_wakeup(g_vt1);
    if (g_vt2) task_wakeup(g_vt2);
    if (g_vt3) task_wakeup(g_vt3);

    /* 等三个任务都停靠（自挂起到 SUSPEND 或句柄已空），最多 500ms。
     *
     * 【v2.2.10 修复 · Ctrl+C 爆闪根因】
     *   旧版用 hal_systick_delay_us(1000) 忙等 — 不让出 CPU 给调度器。
     *   被唤醒的 VT1/VT2/VT3 根本没机会运行到 while(g_vtest_running)
     *   检查 → 200ms 超时 → 强行 destroy 仍在运行的任务 → use-after-free
     *   → HardFault → LED 爆闪。
     *
     *   修复：用 task_sleep(1) 代替忙等。task_sleep 会把 shell 放入 SLEEP
     *   队列，PendSV 切到其他任务（VT1/VT2/VT3），它们能执行到
     *   while(g_vtest_running) 检查 → break → task_suspend(自己) → 停靠。
     *   shell 1ms 后被唤醒回来继续轮询。
     *
     *   超时从 200ms 增加到 500ms：VT2 可能卡在 I2C chunk 传输中，
     *   5 chunks × ~20ms = ~100ms，加上调度延迟 200ms 可能不够。*/
    int parked_ok = 1;
    for (int i = 0; i < 500; i++) {
        int parked =
            (!g_vt1 || g_vt1->state == TASK_STATE_SUSPEND) &&
            (!g_vt2 || g_vt2->state == TASK_STATE_SUSPEND) &&
            (!g_vt3 || g_vt3->state == TASK_STATE_SUSPEND);
        if (parked) break;
        if (i == 499) parked_ok = 0;
        task_sleep(1);  /* 让出 CPU 给 VT 任务，让它们跑到 while 检查后自挂起 */
    }

    if (!parked_ok) {
        /* 超时未停靠：强制 suspend 所有未停靠的任务。
         *   VT2 可能卡在 hal_i2c_tx（忙等 I2C 硬件）中，task_suspend
         *   会把它从就绪/睡眠队列移除并改成 SUSPEND → 不会再被调度。
         *   I2C 硬件会自行 timeout 释放，不会永久锁死。
         *   强制 suspend 后可以安全 destroy，不会 use-after-free。*/
        sh_puts("vtest: WARN - tasks not parked in 500ms, force suspend + destroy.\r\n");
        if (g_vt1 && g_vt1->state != TASK_STATE_SUSPEND) task_suspend(g_vt1);
        if (g_vt2 && g_vt2->state != TASK_STATE_SUSPEND) task_suspend(g_vt2);
        if (g_vt3 && g_vt3->state != TASK_STATE_SUSPEND) task_suspend(g_vt3);
    }

    if (g_vt1) { task_destroy(g_vt1); g_vt1 = NULL; }
    if (g_vt2) { task_destroy(g_vt2); g_vt2 = NULL; }
    if (g_vt3) { task_destroy(g_vt3); g_vt3 = NULL; }
    {
        register const uint32_t SIO_BASE = 0xD0000000u;
        register const uint32_t MASK25   = 0x02000000u;
        *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;
    }
    g_vt1_beats = g_vt2_frames = g_vt2_errs = 0;
    g_vt3_rounds = g_vt3_susps = g_vt3_rsms = 0;
}

/* — cmd_jobs: 列出用户创建的后台任务（类似 Linux jobs）
 *   跳过内核任务（idle/boot_setup/shell），只显示 ID >= 3 的用户任务 */
static int cmd_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_crlf();
    sh_puts("=== Background Jobs ===\r\n");
    int found = 0;
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        tcb_t *t = g_task_pool[i];
        if (!t) continue;
        /* 跳过内核任务：idle(0) / boot_setup(1) / shell(2) */
        if (t->id <= 2) continue;
        found++;
        sh_puts("  [");
        shell_put_uint32((uint32_t)found);
        sh_puts("] ID="); shell_put_uint32(t->id);
        sh_puts("  "); sh_puts(t->name);
        int pad = 12 - (int)strlen(t->name);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        sh_puts(task_state_str(t->state));
        sh_puts("\r\n");
    }
    if (!found) {
        sh_puts("  (no background jobs)\r\n");
    }
    sh_puts("Tip: Ctrl+C stops vtest; 'kill <id>' destroys a single task.\r\n");
    return 0;
}

static int cmd_vtest(int argc, char **argv) {
    if (argc < 2) {
        sh_puts("Usage: vtest start [bus] [addr] | status | stop\r\n");
        sh_puts("  start  — Create 3 validation tasks & run concurrently.\r\n");
        sh_puts("           bus  = I2C bus for OLED (default=0).\r\n");
        sh_puts("           addr = OLED 7-bit I2C addr (default=0x3C).\r\n");
        sh_puts("  status — Print 3-task counters + heap snapshot.\r\n");
        sh_puts("  stop   — Destroy 3 validation tasks & restore LED OFF.\r\n");
        return 1;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        sh_crlf();
        sh_puts("=== vtest Status ===\r\n");
        sh_puts("Running : ");
        sh_puts(g_vtest_running ? "YES" : "NO");
        sh_puts("  OLED bus="); shell_put_uint32((uint32_t)g_vt_oled_bus);
        sh_puts(" addr=0x");    shell_print_hex8(g_vt_oled_addr); sh_crlf();
        sh_puts("VT1(led_beat)      : beats   = "); shell_put_uint32(g_vt1_beats);   sh_crlf();
        sh_puts("VT2(oled_refresh) : frames  = "); shell_put_uint32(g_vt2_frames);
        sh_puts(" (i2c_err=");       shell_put_uint32(g_vt2_errs);  sh_puts(")"); sh_crlf();
        sh_puts("VT3(ctrl_pressure): rounds  = "); shell_put_uint32(g_vt3_rounds);
        sh_puts(" (suspend=");       shell_put_uint32(g_vt3_susps);
        sh_puts(" resume=");         shell_put_uint32(g_vt3_rsms);  sh_puts(")"); sh_crlf();
        /* 【v2.2.9】栈限额统计（内核/用户分离，总量超 80% 告警） */
        {
            size_t k_used = task_kernel_stack_used();
            size_t u_used = task_user_stack_used();
            size_t pool   = task_total_stack_pool();
            uint8_t t_pct = task_total_stack_used_pct();
            sh_puts("Stack: kernel=");  shell_put_uint32((uint32_t)k_used);
            sh_puts("B user=");         shell_put_uint32((uint32_t)u_used);
            sh_puts("B total=");        shell_put_uint32((uint32_t)(k_used + u_used));
            sh_puts("/");               shell_put_uint32((uint32_t)pool);
            sh_puts("B (");             shell_put_uint32((uint32_t)t_pct);
            sh_puts("%)");
            if (task_total_stack_is_low()) {
                sh_puts("  [LOW! >=");
                shell_put_uint32((uint32_t)OS_CFG_USER_STACK_WARN_PCT);
                sh_puts("%]");
            }
            sh_crlf();
        }
        sh_puts("Heap free=");       shell_put_uint32(kmem_free_size());
        sh_puts("B max_block=");     shell_put_uint32(kmem_max_free_block()); sh_puts("B\r\n");
        sh_puts("Tick=");            shell_put_uint32(hal_systick_get_tick()); sh_crlf();
        sh_puts("Tip: Run 'ps' to confirm all 3 tasks appear + states OK.\r\n");
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (!g_vtest_running) {
            sh_puts("vtest: not running. Try 'vtest start'.\r\n");
            return 1;
        }
        vtest_stop_all();
        sh_puts("OK: vtest stopped. 3 tasks destroyed, LED OFF.\r\n");
        sh_puts("Tip: Run 'ps' + 'heap' to confirm tasks gone + heap recovered fully.\r\n");
        return 0;
    }

    if (strcmp(sub, "start") == 0) {
        if (g_vtest_running) {
            sh_puts("vtest: already running. Run 'vtest stop' first.\r\n");
            return 1;
        }

        /* 解析可选 bus / addr 参数（用户的 OLED 默认 I2C0@0x3C）*/
        uint32_t bus = 0, addr = 0x3C;
        if (argc >= 3) {
            if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) {
                sh_puts("vtest: Invalid bus (0 or 1)\r\n"); return 1;
            }
        }
        if (argc >= 4) {
            if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) {
                sh_puts("vtest: Invalid 7-bit OLED addr\r\n"); return 1;
            }
        }
        g_vt_oled_bus  = (uint8_t)bus;
        g_vt_oled_addr = (uint8_t)addr;

        /* 计数器复位（每轮 start 从"干净 0 基准"开始，便于前后对比）*/
        g_vt1_beats = g_vt2_frames = g_vt2_errs = 0;
        g_vt3_rounds = g_vt3_susps = g_vt3_rsms = 0;

        /* 先把 running 标记置 1（任务 while 循环里读它，必须先置）*/
        g_vtest_running = 1;
        __asm volatile ("" ::: "memory");   /* 编译器屏障，防止任务创建后乱序 */

        sh_puts("vtest: Creating 3 validation tasks...\r\n");
        sh_puts("  VT1 (led_beat)      : stack=512,  weight=1 — LED flip every 500ms\r\n");
        sh_puts("  VT2 (oled_refresh) : stack=1536, weight=8 — 1024B GDRAM every 2s via I2C");
        sh_puts(" bus="); shell_put_uint32(bus); sh_puts(" addr=0x"); shell_print_hex8((uint8_t)addr); sh_crlf();
        sh_puts("  VT3 (ctrl_pressure): stack=2048, weight=2 — heap stress + suspend/resume VT2 every 10s\r\n");

        g_vt1 = task_create("vt_led",   vt_task_led,   NULL, 512, 1);
        /* 【v2.3.1 · 栈池溢出修复】3072 → 1536。
         *   根因：用户栈池 = 4096B（总 8192B 对半分），VT1(512)+VT2(3072)+
         *   VT3(2048) = 5632B > 4096B → 栈区域相互覆盖 → TCB/数据损坏 → HardFault。
         *   VT2 实测 stack_used=348B，1536B 有 4.4 倍余量，完全足够。
         *   1536+512+2048 = 4096B = 100% 用户栈池，刚好不溢出。
         * 【v2.3.5 · weight 1 → 8（时间片 5ms → 40ms）】
         *   RP2040 I2C FIFO 仅 16B，VT2 被切走时总线 stall → 肉眼逐行扫描。
         *   400kHz 下整帧 8 页 ≈ 25ms，weight=8 的 40ms 时间片可让整帧
         *   在一个时间片内连续跑完 → 视觉上瞬间整屏刷新。VT1/VT3 大部分
         *   时间在 sleep，公平性不受影响（weight 只影响就绪队列时长）。*/
        g_vt2 = task_create("vt_oled",  vt_task_oled,  NULL, 1536, 8);
        /* v2.2.9: 保持 2048（用户要求不大范围改栈）。同上，靠 STACK_OVF_CHECK
         *        的 4×MAGIC 提前挂起自保；配合新增的"用户栈 8KB 限额 80% 告警"
         *        在 VT3 串口输出中明确提示栈不足，由用户决定自行增改栈大小。*/
        g_vt3 = task_create("vt_ctrl",  vt_task_ctrl,  NULL, 2048, 2);

        if (!g_vt1 || !g_vt2 || !g_vt3) {
            sh_puts("vtest: task_create FAILED (heap exhausted?)\r\n");
            sh_puts("  vt1="); shell_put_uint32(g_vt1 ? 1 : 0);
            sh_puts(" vt2="); shell_put_uint32(g_vt2 ? 1 : 0);
            sh_puts(" vt3="); shell_put_uint32(g_vt3 ? 1 : 0);
            sh_puts(" heap_free="); shell_put_uint32(kmem_free_size()); sh_crlf();
            /* 紧急清理：创建成功的先销毁，避免半残状态 */
            g_vtest_running = 0;
            if (g_vt1) { task_destroy(g_vt1); g_vt1 = NULL; }
            if (g_vt2) { task_destroy(g_vt2); g_vt2 = NULL; }
            if (g_vt3) { task_destroy(g_vt3); g_vt3 = NULL; }
            return 1;
        }
        sh_puts("OK: vtest STARTED. 3 tasks running concurrently.\r\n");
        sh_puts("Validation checklist:\r\n");
        sh_puts("  [ ] LED (GPIO25) blinks ~2Hz (heartbeat) at ALL times — proof of round-robin.\r\n");
        sh_puts("  [ ] OLED shows changing pattern every 2s — proof VT2 scheduled.\r\n");
        sh_puts("  [ ] Every ~13s: OLED FREEZES for 3s (VT2 suspended) but LED keeps blinking!\r\n");
        sh_puts("  [ ] 'vtest status' — all 3 counters monotonically increasing.\r\n");
        sh_puts("  [ ] 'ps'  — shows 6 tasks (idle/boot_setup/shell + vt_led/vt_oled/vt_ctrl), NO garbage IDs.\r\n");
        sh_puts("  [ ] 'vtest stop'  → 'ps' shows back to 3 tasks; 'heap' matches pre-start bytes.\r\n");
        sh_puts("  [ ] Long-run (5+ min): no LED flicker/stuck, no 'Unknown state ?' in ps, heap recovers 100%.\r\n");
        return 0;
    }

    sh_puts("vtest: unknown subcmd '"); sh_puts(sub); sh_puts("'. Try 'vtest status'.\r\n");
    return 1;
}

/* ================================================================
 * Shell 主任务：非阻塞拼行 + 解析 + 打印提示符
 * ================================================================ */
static void task_shell(void *arg) {
    (void)arg;
    /* v2.2.5: line_buf/pos 改为模块级全局（g_shell_line/g_shell_pos），
     *         供 shell_async_enter/exit 保护后台输出时的前台输入行。 */
    g_shell_pos = 0;

    /* v2.2 ① 先注册扩展命令（msc/ls/cd/pwd/mkdir/rmdir/rm/cat）到 shell_register
     *        动态命令表。这样 help + dispatch 都能立即看到它们。 */
    shell_fs_register();
    /* v2.4 ② 注册 ovclk（超频 / 多核固化）命令 */
    shell_ovclk_register();
    /* v2.4 ③ 注册 mcore（多核调度测试）命令 */
    shell_mcore_register();

#if OS_CFG_FATFS
    /* v2.2 ② 挂载 FatFs（空片时会自动 f_mkfs FAT16）
     *   fatfs_init_and_mount 自己会处理 ejected 状态互斥（格式化完会 ejected=true）。 */
    {
        extern int fatfs_init_and_mount(void);   /* 原型在 fatfs_api.h，但 fatfs_api.h 含 ff.h → 避免此处 ff.h 依赖 */
        extern bool fatfs_is_mounted(void);
        extern bool fatfs_mkfs_done_this_boot(void);
        extern bool msc_blockdev_is_ejected(void);
        int fr = fatfs_init_and_mount();
        if (fr == 0 /* FR_OK */) {
            sh_crlf();
            if (fatfs_mkfs_done_this_boot()) {
                sh_puts("[FS   ] Fresh FAT16 formatted (blank chip). Data disk now 1016KiB.\r\n");
            } else {
                sh_puts("[FS   ] FatFs FAT16 mounted OK.\r\n");
            }
            sh_puts("[FS   ] Shell R/W EXCLUSIVE (host USB sees ejected).\r\n");
            sh_puts("[FS   ] Run `msc mount` to let host see the drive, then `msc eject` to write files from shell.\r\n");
        } else {
            sh_crlf();
            sh_puts("[FS   ] FatFs mount failed (code=");
            /* 用 shell_put_err 打印 —— FatFs FRESULT 也是 0=ok <0 错误码相同约定 */
            extern void shell_put_err(int);
            shell_put_err((int)fr);
            sh_puts("). Run `msc format` if disk is blank or corrupted.\r\n");
        }
    }
#endif /* OS_CFG_FATFS */

    sh_crlf();
    sh_puts("========================================\r\n");
    sh_puts("  Mini Kernel Interactive Shell Ready\r\n");
    sh_puts("  Version: "); sh_puts(k_version()); sh_crlf();
    sh_puts("  Type 'help' to list commands.\r\n");
    sh_puts("========================================\r\n");
    sh_crlf();
    sh_puts(SHELL_PROMPT_STR);

    while (1) {
        char c = 0;

        /* hal_console_getc 内部已调用 _usb_force_poll（含 dcd_int_handler +
         * tud_task_ext + EP1 OUT 搬运），无需再单独 poll。
         * 此处只补 CDC IN flush，确保打印字符立即到达主机。 */
        _tud_flush_only();

        /* 方案 0（最高优先级，内核硬件旁路）：HAL 层私有 ring buffer
         *   内核环境下 TinyUSB 的 INTE/ISER 会被抢占破坏，
         *   hal_port.c 实现了 EP1 OUT 硬件直接读取旁路，把数据塞进私有 ring。
         *   必须优先从这里读 —— 其他 API 都依赖 TinyUSB 内部软件层状态。 */
        if (c == 0) {
            char hc = 0;
            if (hal_console_getc(&hc) == 1) {
                c = hc;
            }
        }
        /* 方案 1（备选）：TinyUSB 直读 CDC FIFO */
        if (c == 0 && tud_cdc_n_available(0)) {
            uint8_t b = 0;
            uint32_t got = tud_cdc_n_read(0, &b, 1);
            if (got == 1) c = (char)b;
        }
        /* 方案 2（兜底）：SDK stdio ring buffer */
        if (c == 0) {
            int ch = getchar_timeout_us(0);
            if (ch > 0) c = (char)(ch & 0xFF);
        }

        if (c != 0) {
            /* 【已移除字符 LED 短闪副作用】
             *   旧代码每收到一个字符都会做：SIO_OUT_SET(GPIO25) → 2 nop → SIO_OUT_CLR(GPIO25)，
             *   目的是诊断 USB 字符是否到达；但副作用是：
             *     · 用户使用 `led on` 或 `gpio write 25 1` 把 GPIO25 设为高电平后，
             *       每输入一个字母都会被无条件 SIO_OUT_CLR(GPIO25) 强制拉低 → LED 熄灭。
             *     · 用户命令 `gpio write 25 0` 设为低后，每输入一个字母也会被 SET → 闪干扰。
             *
             *   现在 USB 双向链路已完全稳定（hal_console_getc + EP1 旁路都工作正常），
             *   不再需要这种破坏性的硬件级字符指示，直接移除。
             *
             *   如需"收到字符"指示，应用层应通过 shell 回显字符来感知（本 switch 已处理），
             *   绝不能直接操纵可能被用户业务使用的 GPIO。 */
        } else {
            /* 无字符 → 让渡 CPU 1ms（提高轮询频率） */
            task_sleep(1);
            continue;
        }

        switch (c) {
            case 0x03:  /* Ctrl+C — Linux 风格中断：停止后台任务 + 清空输入行 */
                sh_puts("^C");
                sh_crlf();
                if (g_vtest_running) {
                    sh_puts("[Interrupt] Stopping vtest...\r\n");
                    vtest_stop_all();
                    sh_puts("[Interrupt] vtest stopped. LED OFF.\r\n");
                }
                g_shell_pos = 0;
                sh_puts(SHELL_PROMPT_STR);
                break;

            case '\r':   /* 回车 → Mac/Windows 终端行结束 */
            case '\n':   /* 换行 → Unix 终端行结束 */
                sh_crlf();
                g_shell_line[g_shell_pos] = '\0';
                shell_exec_line(g_shell_line);
                g_shell_pos = 0;
                sh_puts(SHELL_PROMPT_STR);
                break;

            case '\b':   /* 退格：VT100 序列：打印 \b 空格 \b */
            case 0x7F:   /* DEL：也当作退格 */
                if (g_shell_pos > 0) {
                    g_shell_pos--;
                    sh_putc('\b');
                    sh_putc(' ');
                    sh_putc('\b');
                }
                break;

            default:
                /* 过滤掉不可见字符（除 Tab，也允许） */
                if ((c >= 0x20 && c < 0x7F) || c == '\t') {
                    if (g_shell_pos + 1 < SHELL_LINE_SIZE) {
                        g_shell_line[g_shell_pos++] = c;
                        sh_putc(c);   /* 本地回显（仅此一次） */
                    }
                }
                break;
        }
    }
}

/* ================================================================
 * 对外入口：创建 Shell 任务（由 demo_app_init 调用）
 *
 * 开机固化命令执行时机：
 *   在进入 shell 主循环 (task_shell) 之前，立即执行 bootscript_run_all()。
 *   这样每次开机 / reset 后，用户 save / ! 前缀固化的 I2C init / led on /
 *   gpio init 等指令都会自动跑一遍，然后才进入交互式等待。
 *   bootscript_run_all 内部会 "No persistent commands" 友好提示 0 条的情况。
 * ================================================================ */
void shell_start(void) {
    /* 先跑固化指令（如果有） */
    (void)bootscript_run_all();
    /* 再启动交互式 shell */
    tcb_t *t = task_create(SHELL_TASK_NAME, task_shell, NULL,
                           SHELL_TASK_STACK, SHELL_TASK_WEIGHT);
    (void)t;   /* 失败也不 panic：demo 阶段仅尽力启动 */
}

#else /* OS_CFG_SHELL 关闭时的空桩，满足链接器对 shell_start 的外部引用 */

void shell_start(void) { /* no-op */ }

#endif /* OS_CFG_SHELL */
