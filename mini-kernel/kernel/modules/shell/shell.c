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
#define SHELL_MAX_ARGS        16    /* 最大参数个数（含命令本身）。
                                     *   之前 10 会静默截断复合子命令（如 i2c cmds
                                     *   0 0x3C + 8 字节 = 12 token）导致末尾参数丢失
                                     *   且无任何报错。16 足够 I2C/SPI/UART 长命令。 */
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
static int cmd_i2c(int argc, char **argv);

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
    { "i2c",     cmd_i2c,     "i2c help | init ... | scan | wr | rd | memwr | memrd",
                                                    "I2C 主机子命令：扫描总线/读写/寄存器访问" },
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
    shell_puts("I2C subcommands (7-bit device addresses, no auto-left-shift):\r\n");
    shell_puts("  i2c init <bus> <sda> <scl> <hz>      Init + pinmux I2C bus (bus=0|1)\r\n");
    shell_puts("         RP2040 AF: bus0 pins=GP4(SDA)/GP5(SCL), bus1=GP6/GP7\r\n");
    shell_puts("         Typical hz: 100000 / 400000 / 1000000\r\n");
    shell_puts("  i2c scan <bus>                      Scan 7-bit addresses 0x08..0x77\r\n");
    shell_puts("  i2c wr   <bus> <addr> <b1> [..bn]   Raw write 1..N bytes\r\n");
    shell_puts("  i2c rd   <bus> <addr> <len>         Raw read len bytes\r\n");
    shell_puts("  i2c cmds <bus> <addr> <c1> [..cn]   SSD1306-style: 0x00 (Co=0,D/C=0) + N command bytes\r\n");
    shell_puts("  i2c fill <bus> <addr> <byte> <cnt>  SSD1306-style: 0x40 (Co=0,D/C=1) + cnt×byte (GDRAM fill)\r\n");
    shell_puts("  i2c memwr <bus> <addr> <reg> <b1> [..]   Write device register (mem 16-bit)\r\n");
    shell_puts("  i2c memrd <bus> <addr> <reg> <len>       Read device register (mem 16-bit)\r\n");
    shell_puts("Examples:\r\n");
    shell_puts("  i2c init 0 4 5 100000                # Standard-mode on default pins\r\n");
    shell_puts("  i2c scan 0                           # Show connected devices\r\n");
    shell_puts("  i2c wr 0 0x3C 0x00 0xAF              # Write 2 bytes to OLED at 0x3C\r\n");
    shell_puts("  i2c memrd 0 0x50 0x00 16             # Read 16 bytes from AT24Cxx EEPROM @ 0\r\n");
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
        if (argc < 6) { shell_puts("Usage: i2c init <bus> <sda_pin> <scl_pin> <hz>\r\n"); return 1; }
        uint32_t bus, sda, scl, hz;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &sda) != 0 || sda > 29) { shell_puts("Invalid SDA pin (0..29)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &scl) != 0 || scl > 29) { shell_puts("Invalid SCL pin (0..29)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &hz)  != 0 || hz == 0)  { shell_puts("Invalid hz (non-zero)\r\n"); return 1; }

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
        if (r1 != HAL_OK || r2 != HAL_OK) { shell_puts("I2C pinmux FAILED\r\n"); return 1; }
        if (r3 != HAL_OK) { shell_puts("I2C init FAILED\r\n"); return 1; }

        shell_puts("OK: I2C"); shell_put_uint32(bus);
        shell_puts(" SDA=GP"); shell_put_uint32(sda);
        shell_puts(" SCL=GP"); shell_put_uint32(scl);
        shell_puts(" @"); shell_put_uint32(hz); shell_puts("Hz\r\n");
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        if (argc < 3) { shell_puts("Usage: i2c scan <bus>\r\n"); return 1; }
        uint32_t bus;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        shell_puts("Scanning I2C"); shell_put_uint32(bus);
        shell_puts(" (7-bit addresses 0x08..0x77):\r\n   ");
        for (uint8_t col = 0; col < 16; col++) {
            shell_print_hex8(col); shell_putc(' ');
        }
        shell_crlf();
        for (uint8_t row = 0; row < 8; row++) {
            shell_print_hex8(row << 4);
            shell_putc(':'); shell_putc(' ');
            for (uint8_t col = 0; col < 16; col++) {
                uint8_t addr = (uint8_t)((row << 4) | col);
                if (addr < 0x08 || addr > 0x77) {
                    shell_puts("-- ");
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
                    shell_print_hex8(addr); shell_putc(' ');
                } else {
                    shell_puts("-- ");
                }
            }
            shell_crlf();
        }
        shell_puts("Done.\r\n");
        return 0;
    }

    if (strcmp(sub, "wr") == 0) {
        if (argc < 5) { shell_puts("Usage: i2c wr <bus> <addr> <B1> [B2 ...]\r\n"); return 1; }
        uint32_t bus, addr;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr (0x00..0x7F)\r\n"); return 1; }
        int nbytes = argc - 4;
        if (nbytes > 256) { shell_puts("Too many bytes (>256)\r\n"); return 1; }
        uint8_t buf[256];
        for (int i = 0; i < nbytes; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[4 + i], &v) != 0 || v > 0xFF) {
                shell_puts("Invalid byte at pos "); shell_put_uint32(i); shell_puts(": "); shell_puts(argv[4 + i]); shell_crlf();
                return 1;
            }
            buf[i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_tx(bus, (uint8_t)addr, buf, (size_t)nbytes);
        if (r != HAL_OK) { shell_puts("I2C WR NACK or ERROR at addr=0x"); shell_print_hex8((uint8_t)addr); shell_crlf(); return 1; }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" WR 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts(" ["); for (int i = 0; i < nbytes; i++) { if (i) shell_putc(' '); shell_print_hex8(buf[i]); }
        shell_puts("] ("); shell_put_uint32(nbytes); shell_puts(" bytes)\r\n");
        return 0;
    }

    /* i2c cmds — SSD1306 风格：首字节固定 0x00 (Co=0 D/C#=0 后续全是命令字节) + N 个命令参数
     *   SHELL_MAX_ARGS=10 时：cmd=cmds + bus + addr + 最多 7 个命令字节 / 次。
     *   长初始化序列多分几次 i2c cmds 调用即可。 */
    if (strcmp(sub, "cmds") == 0) {
        if (argc < 5) { shell_puts("Usage: i2c cmds <bus> <addr> <C1> [C2 ... C7]\r\n"); return 1; }
        uint32_t bus, addr;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr\r\n"); return 1; }
        int ncmd = argc - 4;
        if (ncmd > 255) { shell_puts("Too many commands (>255)\r\n"); return 1; }
        /* buf[0] = 0x00 控制前缀，buf[1..ncmd] = 命令字节 */
        uint8_t buf[257];
        buf[0] = 0x00;
        for (int i = 0; i < ncmd; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[4 + i], &v) != 0 || v > 0xFF) {
                shell_puts("Invalid cmd byte at pos "); shell_put_uint32(i); shell_crlf();
                return 1;
            }
            buf[1 + i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_tx(bus, (uint8_t)addr, buf, (size_t)(ncmd + 1));
        if (r != HAL_OK) { shell_puts("I2C CMDS NACK/ERROR @0x"); shell_print_hex8((uint8_t)addr); shell_crlf(); return 1; }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" CMDS 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts(" prefix=0x00 + [");
        for (int i = 0; i < ncmd; i++) { if (i) shell_putc(' '); shell_print_hex8(buf[1 + i]); }
        shell_puts("] ("); shell_put_uint32(ncmd); shell_puts(" commands)\r\n");
        return 0;
    }

    /* i2c fill — SSD1306 风格：首字节 0x40 (Co=0 D/C#=1 后续全是 GDRAM 数据) + cnt × 同一个 byte
     *   典型用途：OLED 全屏点亮 = fill 0x3C 0xFF 1024（128×64 bits / 8）
     *            OLED 清屏      = fill 0x3C 0x00 1024
     *            OLED 条纹     = fill 0x3C 0xAA 1024 */
    if (strcmp(sub, "fill") == 0) {
        if (argc < 6) { shell_puts("Usage: i2c fill <bus> <addr> <byte> <cnt>\r\n"); return 1; }
        uint32_t bus, addr, bytev, count;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &bytev) != 0 || bytev > 0xFF) { shell_puts("Invalid fill byte (0x00..0xFF)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &count) != 0 || count == 0 || count > 8192) { shell_puts("Invalid cnt (1..8192)\r\n"); return 1; }

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
                shell_puts("I2C FILL NACK/ERROR @chunk #");
                shell_put_uint32(chunks);
                shell_puts(" bytes left="); shell_put_uint32(remaining); shell_crlf();
                return 1;
            }
            remaining -= chunk;
            chunks++;
        }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" FILL 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts(" byte=0x"); shell_print_hex8((uint8_t)bytev);
        shell_puts(" ×"); shell_put_uint32(count);
        shell_puts(" bytes ("); shell_put_uint32(chunks); shell_puts(" chunks)\r\n");
        return 0;
    }

    if (strcmp(sub, "rd") == 0) {
        if (argc < 5) { shell_puts("Usage: i2c rd <bus> <addr> <len>\r\n"); return 1; }
        uint32_t bus, addr, len;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus (0 or 1)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &len) != 0 || len == 0 || len > 256) { shell_puts("Invalid len (1..256)\r\n"); return 1; }
        uint8_t buf[256];
        hal_err_t r = hal_i2c_rx(bus, (uint8_t)addr, buf, (size_t)len);
        if (r != HAL_OK) { shell_puts("I2C RD NACK or ERROR at addr=0x"); shell_print_hex8((uint8_t)addr); shell_crlf(); return 1; }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" RD 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts(" -> ["); for (size_t i = 0; i < len; i++) { if (i) shell_putc(' '); shell_print_hex8(buf[i]); }
        shell_puts("] ("); shell_put_uint32(len); shell_puts(" bytes)\r\n");
        return 0;
    }

    if (strcmp(sub, "memwr") == 0) {
        if (argc < 6) { shell_puts("Usage: i2c memwr <bus> <addr> <reg> <B1> [..Bn]\r\n"); return 1; }
        uint32_t bus, addr, reg;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &reg) != 0 || reg > 0xFFFF) { shell_puts("Invalid reg (0..0xFFFF, 16-bit mem addr)\r\n"); return 1; }
        int nbytes = argc - 5;
        if (nbytes <= 0 || nbytes > 254) { shell_puts("Need 1..254 data bytes\r\n"); return 1; }
        uint8_t buf[254];
        for (int i = 0; i < nbytes; i++) {
            uint32_t v;
            if (shell_parse_uint_auto(argv[5 + i], &v) != 0 || v > 0xFF) {
                shell_puts("Invalid byte at pos "); shell_put_uint32(i); shell_crlf();
                return 1;
            }
            buf[i] = (uint8_t)v;
        }
        hal_err_t r = hal_i2c_mem_write(bus, (uint8_t)addr, (uint16_t)reg, buf, (size_t)nbytes);
        if (r != HAL_OK) { shell_puts("I2C MEMWR ERROR at addr=0x"); shell_print_hex8((uint8_t)addr);
                           shell_puts(" reg=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg); shell_crlf(); return 1; }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" MEMWR 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts("@REG=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg);
        shell_puts(" ["); for (int i = 0; i < nbytes; i++) { if (i) shell_putc(' '); shell_print_hex8(buf[i]); }
        shell_puts("]\r\n");
        return 0;
    }

    if (strcmp(sub, "memrd") == 0) {
        if (argc < 6) { shell_puts("Usage: i2c memrd <bus> <addr> <reg> <len>\r\n"); return 1; }
        uint32_t bus, addr, reg, len;
        if (shell_parse_uint_auto(argv[2], &bus) != 0 || bus > 1) { shell_puts("Invalid bus\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[3], &addr) != 0 || addr > 0x7F) { shell_puts("Invalid 7-bit addr\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[4], &reg) != 0 || reg > 0xFFFF) { shell_puts("Invalid reg (0..0xFFFF)\r\n"); return 1; }
        if (shell_parse_uint_auto(argv[5], &len) != 0 || len == 0 || len > 256) { shell_puts("Invalid len (1..256)\r\n"); return 1; }
        uint8_t buf[256];
        hal_err_t r = hal_i2c_mem_read(bus, (uint8_t)addr, (uint16_t)reg, buf, (size_t)len);
        if (r != HAL_OK) { shell_puts("I2C MEMRD ERROR at addr=0x"); shell_print_hex8((uint8_t)addr);
                           shell_puts(" reg=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg); shell_crlf(); return 1; }
        shell_puts("OK: I2C"); shell_put_uint32(bus); shell_puts(" MEMRD 0x"); shell_print_hex8((uint8_t)addr);
        shell_puts("@REG=0x"); shell_print_hex8((uint8_t)(reg >> 8)); shell_print_hex8((uint8_t)reg);
        shell_puts(" -> ["); for (size_t i = 0; i < len; i++) { if (i) shell_putc(' '); shell_print_hex8(buf[i]); }
        shell_puts("]\r\n");
        return 0;
    }

    shell_puts("Unknown i2c subcommand: '"); shell_puts(sub); shell_puts("' (try 'i2c help')\r\n");
    return 1;

#else /* !PERIPH_SERVICE */
    (void)argc; (void)argv;
    shell_puts("ERROR: OS_CFG_PERIPH_SERVICE=0, I2C HAL not linked.\r\n");
    return 1;
#endif
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
    bool overflowed = false;
    int argc = shell_split_argv(line, argv, SHELL_MAX_ARGS, &overflowed);
    if (overflowed) {
        shell_puts("WARNING: Too many tokens! Only first ");
        shell_put_uint32(SHELL_MAX_ARGS);
        shell_puts(" parsed. Increase SHELL_MAX_ARGS.\r\n");
    }
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
