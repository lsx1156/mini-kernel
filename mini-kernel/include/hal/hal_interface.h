/**
 * @file    hal_interface.h
 * @brief   硬件抽象层统一接口定义（硬件无关，所有平台通用）
 * @note    移植新 MCU 仅需实现 hal_port.c，内核核心只包含本头文件
 */
#ifndef HAL_INTERFACE_H
#define HAL_INTERFACE_H

#include <stdint.h>
#include <stddef.h>
#include "os_config.h"

/* ================================================================
 * 通用错误码（内核、HAL、用户态三层共用）
 * ================================================================ */
typedef enum {
    HAL_OK            =  0,
    HAL_ERR           = -1,   /* 通用失败 */
    HAL_ERR_INVAL     = -2,   /* 参数非法 */
    HAL_ERR_NOMEM     = -3,   /* 内存不足 */
    HAL_ERR_BUSY      = -4,   /* 资源忙/锁未释放 */
    HAL_ERR_TIMEOUT   = -5,   /* 超时 */
    HAL_ERR_NOTSUP    = -6,   /* 功能未实现/未使能 */
    HAL_ERR_IO        = -7,   /* 物理层错误（ACK失败、CRC错等） */
    HAL_ERR_PARAM     = -8,   /* 参数超范围/非法组合（替代 INVAL 的精确版） */
    HAL_ERR_FULL      = -9,   /* 存储/队列满（如 bootscript 32 条上限） */
} hal_err_t;

/* ================================================================
 * 1. 必选接口 —— 内核启动最低依赖（移植必须实现）
 * ================================================================ */

/* ---- 1.1 系统滴答定时器 ---- */
typedef struct {
    void  (*init)(uint32_t tick_hz);          /* 初始化并启动，配置中断优先级最低 */
    uint32_t (*get_tick)(void);               /* 读取当前 tick 计数（单调递增） */
    void  (*delay_us)(uint32_t us);           /* 精确 us 级阻塞延时（轮询实现） */
} hal_systick_ops_t;

/* ---- 1.2 调试串口（单字节阻塞收发，用于日志/Shell） ---- */
typedef struct {
    void  (*init)(uint32_t baudrate);         /* 初始化 UART，8N1 无流控 */
    int   (*putc)(char c);                    /* 发送单字节，阻塞直到发完，返回字节数/错误码 */
    int   (*getc)(char *c);                   /* 接收单字节，非阻塞，有数据返回 1 并写 *c，无数据返回 0 */
} hal_console_ops_t;

/* ---- 1.3 上下文切换（汇编实现，Cortex-M 使用 PendSV） ---- */
/* TCB 定义在内核核心，HAL 只需知道栈指针偏移 */
struct hal_context {
    uint32_t sp;  /* 栈指针（PSP），由 hal_context_init 填充 */
};
typedef struct hal_context hal_context_t;
void hal_context_init(hal_context_t *ctx, void *stack_top, void (*entry)(void *), void *arg);
void hal_yield_trigger(void);                 /* 触发调度（如 PendSVSET） */

/* ================================================================
 * 2. 可选接口 —— 按 OS_CONFIG 裁剪编译（未使能时弱符号为 NULL）
 * ================================================================ */

#if OS_CFG_PERIPH_SERVICE

/* ---- 2.1 GPIO ---- */
typedef enum { HAL_GPIO_IN, HAL_GPIO_OUT_PP, HAL_GPIO_OUT_OD, HAL_GPIO_AF } hal_gpio_mode_t;
typedef enum { HAL_GPIO_LOW=0, HAL_GPIO_HIGH=1 } hal_gpio_level_t;

typedef struct {
    hal_err_t (*init)(uint32_t pin, hal_gpio_mode_t mode, uint32_t af); /* af: 复用功能编号 */
    void      (*write)(uint32_t pin, hal_gpio_level_t level);
    hal_gpio_level_t (*read)(uint32_t pin);
    void      (*toggle)(uint32_t pin);
    hal_err_t (*lock)(uint32_t pin);       /* 可选：锁定配置防误改 */
} hal_gpio_ops_t;

/* ---- 2.2 SPI（主机模式，阻塞/轮询实现，上层由内核排队串行化） ---- */
typedef struct {
    hal_err_t (*init)(uint32_t bus_id, uint32_t hz, uint8_t mode, uint8_t bits);
    hal_err_t (*xfer)(uint32_t bus_id, const uint8_t *tx, uint8_t *rx, size_t len);
    hal_err_t (*cs_ctrl)(uint32_t bus_id, uint8_t cs_pin, uint8_t active);
} hal_spi_ops_t;

/* ---- 2.3 I2C（主机模式，7-bit 地址） ---- */
typedef struct {
    hal_err_t (*init)(uint32_t bus_id, uint32_t hz);
    hal_err_t (*master_tx)(uint32_t bus_id, uint8_t addr, const uint8_t *buf, size_t len);
    hal_err_t (*master_rx)(uint32_t bus_id, uint8_t addr, uint8_t *buf, size_t len);
    hal_err_t (*mem_read)(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, uint8_t *buf, size_t len);
    hal_err_t (*mem_write)(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, const uint8_t *buf, size_t len);
} hal_i2c_ops_t;

/* ---- 2.4 UART（异步/缓冲，供 Shell/用户数据流） ---- */
typedef struct {
    hal_err_t (*init)(uint32_t uart_id, uint32_t baud, uint8_t parity, uint8_t stop_bits);
    hal_err_t (*write)(uint32_t uart_id, const uint8_t *buf, size_t len, uint32_t timeout_ms);
    hal_err_t (*read)(uint32_t uart_id, uint8_t *buf, size_t len, uint32_t timeout_ms);
    hal_err_t (*flush)(uint32_t uart_id);
    hal_err_t (*set_rx_cb)(uint32_t uart_id, void (*cb)(uint8_t byte, void *arg), void *arg);
} hal_uart_ops_t;

/* ---- 2.5 板载 Flash（RP2040 的 QSPI 外挂 W25Q16JV，2MB） ----
 *   · erase/program 必须扇区对齐：4096B，写前必须先擦（1→0 只能擦恢复 0→1）
 *   · offset = 从 Flash 起始地址 (PICO_FLASH = 0x10000000) 的字节偏移
 *   · Pico SDK 限制：写/擦阶段必须暂停 XIP，故 erase/program 内部会关中断
 *     并从 RAM 执行函数，执行期间不会响应任何调度 */
#define HAL_FLASH_SECTOR_SIZE   4096u
#define HAL_FLASH_PAGE_SIZE     256u
typedef struct {
    hal_err_t (*erase_sector)(uint32_t offset);                   /* offset 必须 4096 对齐 */
    hal_err_t (*program_page)(uint32_t offset, const uint8_t *data, size_t len);
    const uint8_t *(*map_read)(uint32_t offset);                  /* 返回 XIP 映射的只读指针，直接读 */
    hal_err_t (*crc8_range)(uint32_t offset, size_t len, uint8_t *out_crc);
} hal_flash_ops_t;

#endif /* OS_CFG_PERIPH_SERVICE */

#if OS_CFG_VFS && OS_CFG_FATFS
/* ---- 2.5 SD 卡块设备（扇区 512B，对接 FatFs） ---- */
typedef struct {
    hal_err_t (*init)(void);
    hal_err_t (*read_sectors)(uint8_t *buf, uint32_t sector, uint32_t count);
    hal_err_t (*write_sectors)(const uint8_t *buf, uint32_t sector, uint32_t count);
    hal_err_t (*get_capacity)(uint32_t *sector_count);
} hal_sdcard_ops_t;
#endif

/* ================================================================
 * 3. HAL 导出表（内核启动时由 hal_port.c 填充，内核只用指针调用）
 * ================================================================ */
typedef struct {
    /* 必选 */
    const hal_systick_ops_t *systick;
    const hal_console_ops_t *console;
    /* 上下文切换不走表，直接链接符号 */

    /* 可选（未实现置 NULL，内核运行时检查） */
#if OS_CFG_PERIPH_SERVICE
    const hal_gpio_ops_t   *gpio;
    const hal_spi_ops_t    *spi;
    const hal_i2c_ops_t    *i2c;
    const hal_uart_ops_t   *uart;
    const hal_flash_ops_t  *flash;   /* 板载 Flash（W25Q16JV），供 bootscript / ENV 固化 */
#endif
#if OS_CFG_VFS && OS_CFG_FATFS
    const hal_sdcard_ops_t *sdcard;
#endif
} hal_export_t;

/* 由移植层 hal_port.c 定义并导出 */
extern const hal_export_t hal_export;

/* 软复位系统（重启后重新执行完整启动流程，重现开机 banner）。
 * 由移植层 hal_port.c 强符号实现，shell `reboot` 命令调用。 */
void hal_system_reset(void) __attribute__((noreturn));

/* ================================================================
 * 1.5 板级基础功能（移植必须实现强符号，让内核核心保持平台无关）
 *
 *  这些接口把内核启动/诊断阶段对"板子"的依赖（LED、延时、早期诊断
 *  串口、控制台就绪、全局中断开关）全部收敛到移植层。新 MCU 只要在
 *  对应的 hal_port.c 实现它们，内核核心（kernel/core）无需改动即可
 *  编译运行，从而保证内核库纯净、可移植到任意 MCU。
 * ================================================================ */
void hal_led_init(void);              /* 初始化板载状态 LED */
void hal_led_set(int on);             /* 设置 LED 状态（1=亮 0=灭） */
void hal_led_on(void);                /* 点亮板载 LED */
void hal_led_off(void);               /* 熄灭板载 LED */
void hal_delay_ms(uint32_t ms);       /* 阻塞延时（诊断/空闲循环用，轮询实现，不依赖调度器） */
int  hal_console_ready(void);         /* 控制台是否就绪（如 USB CDC 已枚举），非阻塞 */
void hal_irq_enable(void);            /* 开全局中断 */
void hal_irq_disable(void);           /* 关全局中断 */
uint32_t hal_core_id(void);           /* 当前运行核心号（0/1） */
void hal_mcore_start(void);           /* 若已固化多核，启动 core1（由 boot_setup 在 core0 稳定后调用） */
uint32_t hal_mcore_core1_ticks(void); /* 诊断：core1 产生的 tick 计数 */

/* 开机日志缓存回放（v2.4.3）：
 *   启动阶段 hal_console_putc 的输出会被移植层同步捕获进 RAM 缓冲；
 *   内核在启动流程结束（sched_start 前）调用 hal_bootlog_end() 停止捕获。
 *   之后若主机（如 USB 终端）迟于启动才连接，移植层检测到连接时会把
 *   缓存的完整开机日志回放一遍，保证"无论何时打开终端都能看到开机画面"。 */
void hal_bootlog_end(void);

/* 早期诊断输出：在调度器启动前（sched_start 之前）也必须可用。
 *   · hal_diag_init()  初始化早期诊断通道（如 UART0 115200）
 *   · hal_diag_putc()  直接写硬件寄存器，绝不触发调度/中断（PendSV 安全）
 * 用于打印 [CONFIG] 等冷启动关键信息，移植层用 UART 或其它早期通道实现。 */
void hal_diag_init(void);
void hal_diag_putc(char c);
void hal_diag_puts(const char *s);
void hal_diag_put_u32(uint32_t v);

/* ================================================================
 * 4. 便捷宏（内核内部使用，上层应用勿直接调用）
 * ================================================================ */
#define hal_systick_init(hz)       hal_export.systick->init(hz)
#define hal_systick_get_tick()     hal_export.systick->get_tick()
#define hal_systick_delay_us(us)   hal_export.systick->delay_us(us)

#define hal_console_init(b)        hal_export.console->init(b)
#define hal_console_putc(c)        hal_export.console->putc(c)
#define hal_console_getc(c)        hal_export.console->getc(c)

#if OS_CFG_PERIPH_SERVICE
#define hal_gpio_init(p,m,af)      hal_export.gpio->init(p,m,af)
#define hal_gpio_write(p,l)        hal_export.gpio->write(p,l)
#define hal_gpio_read(p)           hal_export.gpio->read(p)
#define hal_gpio_toggle(p)         hal_export.gpio->toggle(p)

#define hal_spi_init(id,hz,mo,bi)  hal_export.spi->init(id,hz,mo,bi)
#define hal_spi_xfer(id,tx,rx,len) hal_export.spi->xfer(id,tx,rx,len)
#define hal_spi_cs(id,pin,act)     hal_export.spi->cs_ctrl(id,pin,act)

#define hal_i2c_init(id,hz)        hal_export.i2c->init(id,hz)
#define hal_i2c_tx(id,a,b,l)       hal_export.i2c->master_tx(id,a,b,l)
#define hal_i2c_rx(id,a,b,l)       hal_export.i2c->master_rx(id,a,b,l)
#define hal_i2c_mem_read(id,a,m,b,l)  hal_export.i2c->mem_read(id,a,m,b,l)
#define hal_i2c_mem_write(id,a,m,b,l) hal_export.i2c->mem_write(id,a,m,b,l)

#define hal_uart_init(i,b,p,s)     hal_export.uart->init(i,b,p,s)
#define hal_uart_write(i,b,l,t)    hal_export.uart->write(i,b,l,t)
#define hal_uart_read(i,b,l,t)     hal_export.uart->read(i,b,l,t)

/* 板载 Flash：offset = 相对 Flash 基址 (0x10000000) 的字节偏移
 *   · hal_flash_erase_sector : 擦 4KB 扇区（必须 sector 对齐）
 *   · hal_flash_program      : 写任意 len 字节（内部按 256B page 分块，offset 无需对齐）
 *   · hal_flash_map_read     : 返回 XIP 映射只读指针，直接 memcpy 读
 *   · hal_flash_crc8         : 只读区域 CRC8（x^8+x^5+x^4+1 多项式，和 crc8_smbus 相同） */
#define hal_flash_erase_sector(o)      hal_export.flash->erase_sector(o)
#define hal_flash_program(o,d,l)       hal_export.flash->program_page(o,d,l)
#define hal_flash_map_read(o)          hal_export.flash->map_read(o)
#define hal_flash_crc8(o,l,c)          hal_export.flash->crc8_range(o,l,c)
#endif

#if OS_CFG_VFS && OS_CFG_FATFS
#define hal_sd_init()              hal_export.sdcard->init()
#define hal_sd_read(b,s,c)         hal_export.sdcard->read_sectors(b,s,c)
#define hal_sd_write(b,s,c)        hal_export.sdcard->write_sectors(b,s,c)
#endif

#endif /* HAL_INTERFACE_H */