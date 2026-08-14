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

/* 版本字符串（由 demo_app.c 提供的公共实现） */
extern const char *k_version(void);

/* ================================================================
 * Shell 基本常量
 * ================================================================ */
#define SHELL_LINE_SIZE       128   /* 单条命令最大长度 */
#define SHELL_MAX_ARGS        10    /* 最大参数个数（含命令本身，gpio init af 等需要 5+） */
#define SHELL_PROMPT_STR      "mk> "
#define SHELL_TASK_NAME       "shell"
#define SHELL_TASK_STACK      768   /* 行解析 + 打印任务列表足够 */
#define SHELL_TASK_WEIGHT     1

/* ================================================================
 * 控制台输出辅助（避免依赖 k_printf 的完整实现）
 * ================================================================ */
static void shell_putc(char c)          { hal_console_putc(c); }
static void shell_puts(const char *s)   { while (*s) hal_console_putc(*s++); }
static void shell_crlf(void)            { shell_puts("\r\n"); }

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

/* ================================================================
 * 命令表（驱动扩展：新增命令只需加一行）
 * ================================================================ */
static const shell_cmd_t g_cmd_table[] = {
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
    { "syscalls",cmd_syscalls,"syscalls",              "列出系统调用契约表" },
    { "ver",     cmd_version, NULL, NULL },            /* version 的别名 */
    { "cls",     cmd_clear,   NULL, NULL },            /* clear 的别名 */
};
#define SHELL_CMD_NUM  (sizeof(g_cmd_table) / sizeof(g_cmd_table[0]))

/* ================================================================
 * 各命令实现
 * ================================================================ */

/* help [cmd] */
static int cmd_help(int argc, char **argv) {
    if (argc >= 2) {
        /* 过滤具体命令的帮助 */
        const char *name = argv[1];
        for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
            if (!g_cmd_table[i].usage) continue;   /* 跳过别名 */
            if (strcmp(g_cmd_table[i].name, name) == 0) {
                shell_puts("Usage  : "); shell_puts(g_cmd_table[i].usage); shell_crlf();
                shell_puts("Summary: "); shell_puts(g_cmd_table[i].help ? g_cmd_table[i].help : ""); shell_crlf();
                return 0;
            }
        }
        shell_puts("Unknown command: "); shell_puts(name); shell_crlf();
        return 1;
    }
    shell_puts("=== Mini Kernel Shell Help ===\r\n");
    shell_puts("Command          Description\r\n");
    shell_puts("----------------------------------------\r\n");
    for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
        if (!g_cmd_table[i].usage) continue;
        shell_puts(g_cmd_table[i].name);
        int pad = 17 - (int)strlen(g_cmd_table[i].name);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        shell_puts(g_cmd_table[i].help ? g_cmd_table[i].help : "");
        shell_crlf();
    }
    shell_puts("----------------------------------------\r\n");
    shell_puts("Tips: 支持退格键(\\b)。任务ID用 'ps' 命令查询。\r\n");
    return 0;
}

/* ps */
static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_crlf();
    shell_puts("--- Task List ---\r\n");
    shell_puts("ID  Name        State     TicksLeft  StackBase  StackSize  StackOK\r\n");
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        tcb_t *t = g_task_pool[i];
        if (!t) continue;
        /* ID */
        shell_put_uint32(t->id);
        shell_puts("   ");
        /* Name + pad */
        shell_puts(t->name);
        int pad = 12 - (int)strlen(t->name);
        if (pad < 0) pad = 0;
        shell_pad_spaces(pad);
        /* State */
        shell_puts(task_state_str(t->state));
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
        shell_crlf();
    }
    /* Current running task */
    shell_puts("Cur: ");
    shell_puts(g_current_task ? g_current_task->name : "<null>");
    shell_crlf();
    shell_puts("-----------------\r\n");
    return 0;
}

/* heap */
static int cmd_heap(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Heap free     = "); shell_put_uint32(kmem_free_size());         shell_puts(" B\r\n");
    shell_puts("Heap max blk  = "); shell_put_uint32(kmem_max_free_block());    shell_puts(" B\r\n");
    return 0;
}

/* tick */
static int cmd_tick(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Tick = ");
    shell_put_uint32(hal_systick_get_tick());
    shell_puts("  (@"); shell_put_uint32(OS_CFG_TICK_HZ); shell_puts(" Hz)\r\n");
    return 0;
}

/* version / ver */
static int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Mini Kernel version: ");
    shell_puts(k_version());
    shell_crlf();
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
    if (argc < 2) { shell_puts("Usage: suspend <id> (use 'ps' for IDs)\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { shell_puts("Invalid ID (not a number)\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { shell_puts("Task id="); shell_put_uint32(id); shell_puts(" not found\r\n"); return 1; }
    if (t == g_current_task) { shell_puts("Cannot suspend the running shell task itself\r\n"); return 1; }
    task_suspend(t);
    shell_puts("Task id="); shell_put_uint32(id);
    shell_puts(" (" ); shell_puts(t->name); shell_puts(") SUSPENDed\r\n");
    return 0;
}

/* resume <id> */
static int cmd_resume(int argc, char **argv) {
    if (argc < 2) { shell_puts("Usage: resume <id>\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { shell_puts("Invalid ID\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { shell_puts("Task id="); shell_put_uint32(id); shell_puts(" not found\r\n"); return 1; }
    task_resume(t);
    shell_puts("Task id="); shell_put_uint32(id);
    shell_puts(" (" ); shell_puts(t->name); shell_puts(") RESUMEd\r\n");
    return 0;
}

/* kill <id> */
static int cmd_kill(int argc, char **argv) {
    if (argc < 2) { shell_puts("Usage: kill <id>\r\n"); return 1; }
    uint32_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { shell_puts("Invalid ID\r\n"); return 1; }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    tcb_t *t = find_task_by_id(id);
    if (!t) { shell_puts("Task id="); shell_put_uint32(id); shell_puts(" not found\r\n"); return 1; }
    if (t == g_current_task) { shell_puts("Cannot kill the shell task itself\r\n"); return 1; }
    char name_buf[16];
    memcpy(name_buf, t->name, 12); name_buf[12] = 0;
    task_destroy(t);
    shell_puts("Task id="); shell_put_uint32(id);
    shell_puts(" (" ); shell_puts(name_buf); shell_puts(") KILLed (TCB returned to pool)\r\n");
    return 0;
}

/* clear / cls */
static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    /* VT100 兼容：Putty, TeraTerm, cmder, screen, minicom, Windows Terminal 全部支持 */
    shell_puts("\033[2J");    /* ESC[2J 清屏 */
    shell_puts("\033[H");     /* ESC[H  光标回左上角 (row1,col1) */
    return 0;
}

/* led on|off|toggle */
static int cmd_led(int argc, char **argv) {
#if OS_CFG_PERIPH_SERVICE
    if (argc < 2) { shell_puts("Usage: led on | off | toggle\r\n"); return 1; }
    /* RP2040 板载 LED 固定是 GPIO25（Pico非W版），这里直接用 hal_gpio 接口 */
    const uint32_t pin = 25;
    if (strcmp(argv[1], "on") == 0) {
        hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
        hal_gpio_write(pin, HAL_GPIO_HIGH);
        shell_puts("LED GPIO25 ON\r\n");
    } else if (strcmp(argv[1], "off") == 0) {
        hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
        hal_gpio_write(pin, HAL_GPIO_LOW);
        shell_puts("LED GPIO25 OFF\r\n");
    } else if (strcmp(argv[1], "toggle") == 0) {
        hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
        hal_gpio_toggle(pin);
        shell_puts("LED GPIO25 toggled\r\n");
    } else {
        shell_puts("Unknown led op (use: on | off | toggle)\r\n");
        return 1;
    }
    return 0;
#else
    (void)argc; (void)argv;
    shell_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, hal_gpio not linked\r\n");
    return 1;
#endif
}

/* syscalls — 列出系统调用契约表 */
static int cmd_syscalls(int argc, char **argv) {
    (void)argc; (void)argv;
    size_t total = syscall_table_size();
    shell_puts("\r\n=== Syscall Contract Table (");
    shell_put_uint32((uint32_t)total);
    shell_puts(" entries) ===\r\n");
    shell_puts("ID    Name            Params  Return     Signature\r\n");
    shell_puts("----------------------------------------------------------\r\n");
    for (size_t i = 0; i < total; i++) {
        const syscall_entry_t *e = syscall_get_entry(i);
        if (!e) break;
        /* ID */
        shell_put_uint32((uint32_t)e->id);
        shell_pad_spaces(6 - (int)(e->id / 10 + 1));
        /* Name */
        shell_puts(e->name);
        shell_pad_spaces(16 - (int)strlen(e->name));
        /* Params */
        shell_put_uint32(e->param_count);
        shell_pad_spaces(8);
        /* Return type */
        shell_puts(e->return_type);
        shell_pad_spaces(11 - (int)strlen(e->return_type));
        /* Signature */
        shell_puts(e->signature);
        shell_crlf();
    }
    shell_puts("----------------------------------------------------------\r\n");
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
    shell_puts("GPIO subcommands (pin=0..29 for RP2040, 30+ = QSPI/SWCLK reserved):\r\n");
    shell_puts("  gpio help                        打印本帮助\r\n");
    shell_puts("  gpio init  <pin> in              初始化: 浮空输入\r\n");
    shell_puts("  gpio init  <pin> out   [0|1]     初始化: 推挽输出，可选初始电平(默认0)\r\n");
    shell_puts("  gpio init  <pin> out_od [0|1]    初始化: 开漏输出，可选初始电平\r\n");
    shell_puts("  gpio init  <pin> af <af_num>     初始化: 复用功能(0=FUNC0/SPI .. 5=FUNC5/SIO)\r\n");
    shell_puts("  gpio read  <pin>                 读引脚电平 -> 打印 0 或 1\r\n");
    shell_puts("  gpio write <pin> <0|1>           设置输出电平\r\n");
    shell_puts("  gpio toggle <pin>                翻转输出电平\r\n");
    shell_puts("Examples:\r\n");
    shell_puts("  gpio init 25 out 1        # GPIO25(板载LED) 推挽输出 初始高电平\r\n");
    shell_puts("  gpio toggle 25            # 翻转 LED\r\n");
    shell_puts("  gpio init 0 af 2          # GPIO0=FUNC2(UART0_TX)\r\n");
    shell_puts("  gpio read 5               # 读 GPIO5 电平\r\n");
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
        if (argc < 4) { shell_puts("Usage: gpio init <pin> in | out [0|1] | out_od [0|1] | af <af_num>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) {
            shell_puts("Invalid pin number (must be 0..29)\r\n"); return 1;
        }
        const char *mode = argv[3];

        if (strcmp(mode, "in") == 0) {
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_IN, 0);
            if (r != HAL_OK) { shell_puts("GPIO"); shell_put_uint32(pin); shell_puts(" init IN failed (pin invalid)\r\n"); return 1; }
            shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" = INPUT (floating)\r\n");
            return 0;
        }
        if (strcmp(mode, "out") == 0) {
            uint32_t init_val = 0;
            if (argc >= 5) {
                if (shell_parse_uint(argv[4], &init_val) != 0 || init_val > 1) {
                    shell_puts("Invalid initial value (must be 0 or 1)\r\n"); return 1;
                }
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_OUT_PP, 0);
            if (r != HAL_OK) { shell_puts("GPIO"); shell_put_uint32(pin); shell_puts(" init OUT failed\r\n"); return 1; }
            hal_gpio_write(pin, init_val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
            shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" = OUTPUT-PP, level="); shell_put_uint32(init_val); shell_puts("\r\n");
            return 0;
        }
        if (strcmp(mode, "out_od") == 0) {
            uint32_t init_val = 0;
            if (argc >= 5) {
                if (shell_parse_uint(argv[4], &init_val) != 0 || init_val > 1) {
                    shell_puts("Invalid initial value (must be 0 or 1)\r\n"); return 1;
                }
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_OUT_OD, 0);
            if (r != HAL_OK) { shell_puts("GPIO"); shell_put_uint32(pin); shell_puts(" init OUT_OD failed\r\n"); return 1; }
            hal_gpio_write(pin, init_val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
            shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" = OUTPUT-OD, level="); shell_put_uint32(init_val); shell_puts("\r\n");
            return 0;
        }
        if (strcmp(mode, "af") == 0) {
            if (argc < 5) { shell_puts("Usage: gpio init <pin> af <af_num> (0..9 for FUNC0..FUNC9)\r\n"); return 1; }
            uint32_t af_num = 0;
            if (shell_parse_uint(argv[4], &af_num) != 0 || af_num > 9) {
                shell_puts("Invalid af_num (must be 0..9). RP2040: 0=SPI,1=UART0,2=UART1,3=I2C0,4=I2C1,5=SIO,6=PWM,7=SIO/PIO,...\r\n");
                return 1;
            }
            hal_err_t r = hal_gpio_init(pin, HAL_GPIO_AF, af_num);
            if (r != HAL_OK) { shell_puts("GPIO"); shell_put_uint32(pin); shell_puts(" init AF failed\r\n"); return 1; }
            shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" = ALT-FUNC"); shell_put_uint32(af_num); shell_puts("\r\n");
            return 0;
        }
        shell_puts("Unknown gpio mode: '"); shell_puts(mode); shell_puts("' (expected: in|out|out_od|af)\r\n");
        return 1;
    }

    if (strcmp(sub, "read") == 0) {
        if (argc < 3) { shell_puts("Usage: gpio read <pin>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { shell_puts("Invalid pin number\r\n"); return 1; }
        hal_gpio_level_t lv = hal_gpio_read(pin);
        shell_puts("GPIO"); shell_put_uint32(pin); shell_puts(" = ");
        shell_puts((lv == HAL_GPIO_HIGH) ? "1 (HIGH)" : "0 (LOW)");
        shell_puts("\r\n");
        return 0;
    }

    if (strcmp(sub, "write") == 0) {
        if (argc < 4) { shell_puts("Usage: gpio write <pin> <0|1>\r\n"); return 1; }
        uint32_t pin = 0, val = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { shell_puts("Invalid pin number\r\n"); return 1; }
        if (shell_parse_uint(argv[3], &val) != 0 || val > 1) { shell_puts("Invalid value (must be 0 or 1)\r\n"); return 1; }
        hal_gpio_write(pin, val ? HAL_GPIO_HIGH : HAL_GPIO_LOW);
        shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" = "); shell_put_uint32(val); shell_puts("\r\n");
        return 0;
    }

    if (strcmp(sub, "toggle") == 0) {
        if (argc < 3) { shell_puts("Usage: gpio toggle <pin>\r\n"); return 1; }
        uint32_t pin = 0;
        if (shell_parse_uint(argv[2], &pin) != 0) { shell_puts("Invalid pin number\r\n"); return 1; }
        hal_gpio_toggle(pin);
        shell_puts("OK: GPIO"); shell_put_uint32(pin); shell_puts(" toggled. Now = ");
        shell_puts((hal_gpio_read(pin) == HAL_GPIO_HIGH) ? "1 (HIGH)" : "0 (LOW)");
        shell_puts("\r\n");
        return 0;
    }

    shell_puts("Unknown gpio subcommand: '"); shell_puts(sub); shell_puts("' (try 'gpio help')\r\n");
    return 1;

#else /* !PERIPH_SERVICE */
    (void)argc; (void)argv;
    shell_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, GPIO HAL not linked. Set =1 in os_config.h.\r\n");
    return 1;
#endif
}

/* ================================================================
 * 行解析器：按"空格"拆分 argv[0..argc-1]
 * ================================================================ */
static int shell_split_argv(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p) {
        /* 跳过前导空白（空格/Tab） */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (argc >= max_args) break;
        argv[argc++] = p;
        /* 走到本段 token 末尾 */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* ================================================================
 * 执行单条命令
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
    static char *argv[SHELL_MAX_ARGS];
    int argc = shell_split_argv(line, argv, SHELL_MAX_ARGS);
    if (argc == 0) return 0;   /* 空行 */
    const char *name = argv[0];
    for (size_t i = 0; i < SHELL_CMD_NUM; i++) {
        if (strcmp(g_cmd_table[i].name, name) == 0) {
            return g_cmd_table[i].handler(argc, argv);
        }
    }
    shell_puts("Unknown command: ");
    shell_puts(name);
    shell_puts(" (try 'help')\r\n");
    return 1;
}

/* ================================================================
 * Shell 主任务：非阻塞拼行 + 解析 + 打印提示符
 * ================================================================ */
static void task_shell(void *arg) {
    (void)arg;
    static char line_buf[SHELL_LINE_SIZE];
    int pos = 0;

    shell_crlf();
    shell_puts("========================================\r\n");
    shell_puts("  Mini Kernel Interactive Shell Ready\r\n");
    shell_puts("  Version: "); shell_puts(k_version()); shell_crlf();
    shell_puts("  Type 'help' to list commands.\r\n");
    shell_puts("========================================\r\n");
    shell_crlf();
    shell_puts(SHELL_PROMPT_STR);

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
            case '\r':   /* 回车 → Mac/Windows 终端行结束 */
            case '\n':   /* 换行 → Unix 终端行结束 */
                shell_crlf();
                line_buf[pos] = '\0';
                shell_exec_line(line_buf);
                pos = 0;
                shell_puts(SHELL_PROMPT_STR);
                break;

            case '\b':   /* 退格：VT100 序列：打印 \b 空格 \b */
            case 0x7F:   /* DEL：也当作退格 */
                if (pos > 0) {
                    pos--;
                    shell_putc('\b');
                    shell_putc(' ');
                    shell_putc('\b');
                }
                break;

            default:
                /* 过滤掉不可见字符（除 Tab，也允许） */
                if ((c >= 0x20 && c < 0x7F) || c == '\t') {
                    if (pos + 1 < SHELL_LINE_SIZE) {
                        line_buf[pos++] = c;
                        shell_putc(c);   /* 本地回显（仅此一次） */
                    }
                }
                break;
        }
    }
}

/* ================================================================
 * 对外入口：创建 Shell 任务（由 demo_app_init 调用）
 * ================================================================ */
void shell_start(void) {
    tcb_t *t = task_create(SHELL_TASK_NAME, task_shell, NULL,
                           SHELL_TASK_STACK, SHELL_TASK_WEIGHT);
    (void)t;   /* 失败也不 panic：demo 阶段仅尽力启动 */
}

#else /* OS_CFG_SHELL 关闭时的空桩，满足链接器对 shell_start 的外部引用 */

void shell_start(void) { /* no-op */ }

#endif /* OS_CFG_SHELL */
