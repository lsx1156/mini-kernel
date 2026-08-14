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
    const hal_gpio_ops_t  *gpio;
    const hal_spi_ops_t   *spi;
    const hal_i2c_ops_t   *i2c;
    const hal_uart_ops_t  *uart;
#endif
#if OS_CFG_VFS && OS_CFG_FATFS
    const hal_sdcard_ops_t *sdcard;
#endif
} hal_export_t;

/* 由移植层 hal_port.c 定义并导出 */
extern const hal_export_t hal_export;

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

#define hal_uart_init(i,b,p,s)     hal_export.uart->init(i,b,p,s)
#define hal_uart_write(i,b,l,t)    hal_export.uart->write(i,b,l,t)
#define hal_uart_read(i,b,l,t)     hal_export.uart->read(i,b,l,t)
#endif

#if OS_CFG_VFS && OS_CFG_FATFS
#define hal_sd_init()              hal_export.sdcard->init()
#define hal_sd_read(b,s,c)         hal_export.sdcard->read_sectors(b,s,c)
#define hal_sd_write(b,s,c)        hal_export.sdcard->write_sectors(b,s,c)
#endif

#endif /* HAL_INTERFACE_H */