/**
 * @file    startup_rp2040.S
 * @brief   RP2040 启动代码：向量表、复位处理、.data/.bss 初始化、跳转 main
 */
.syntax unified
.cpu cortex-m0plus
.thumb

/* 向量表 */
.section .vector_table, "ax", %progbits
.global _vector_table
_vector_table:
    .word   __stack_top__           /* 初始 SP */
    .word   _start                  /* Reset Handler */
    .word   NMI_Handler
    .word   HardFault_Handler
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   SVC_Handler
    .word   0                       /* Reserved */
    .word   0                       /* Reserved */
    .word   PendSV_Handler
    .word   SysTick_Handler
    /* 外设中断（RP2040 共 26 个外设中断） */
    .word   TIMER_IRQ_0_Handler
    .word   TIMER_IRQ_1_Handler
    .word   TIMER_IRQ_2_Handler
    .word   TIMER_IRQ_3_Handler
    .word   PWM_IRQ_WRAP_Handler
    .word   USBCTRL_IRQ_Handler
    .word   XIP_IRQ_Handler
    .word   PIO0_IRQ_0_Handler
    .word   PIO0_IRQ_1_Handler
    .word   PIO1_IRQ_0_Handler
    .word   PIO1_IRQ_1_Handler
    .word   DMA_IRQ_0_Handler
    .word   DMA_IRQ_1_Handler
    .word   IO_IRQ_BANK0_Handler
    .word   IO_IRQ_QSPI_Handler
    .word   SIO_IRQ_PROC0_Handler
    .word   SIO_IRQ_PROC1_Handler
    .word   CLOCKS_IRQ_Handler
    .word   SPI0_IRQ_Handler
    .word   SPI1_IRQ_Handler
    .word   UART0_IRQ_Handler
    .word   UART1_IRQ_Handler
    .word   ADC_IRQ_FIFO_Handler
    .word   I2C0_IRQ_Handler
    .word   I2C1_IRQ_Handler
    .word   RTC_IRQ_Handler

/* ===== 复位处理 ===== */
.section .text.startup, "ax", %progbits
.global _start
.type   _start, %function
_start:
    /* 1. 设置向量表偏移 (VTOR) */
    ldr     r0, =_vector_table
    ldr     r1, =0xE000ED08         /* VTOR */
    str     r0, [r1]

    /* 2. 使能 FPU (Cortex-M0+ 无 FPU，跳过) */

    /* 3. 拷贝 .data 段 (Flash -> RAM) */
    ldr     r0, =__data_load_start__
    ldr     r1, =__data_start__
    ldr     r2, =__data_end__
    cmp     r1, r2
    beq     2f
1:  ldr     r3, [r0]
    adds    r0, r0, #4
    str     r3, [r1]
    adds    r1, r1, #4
    cmp     r1, r2
    bne     1b

    /* 4. 清零 .bss 段 */
2:  ldr     r1, =__bss_start__
    ldr     r2, =__bss_end__
    cmp     r1, r2
    beq     4f
    movs    r3, #0
3:  str     r3, [r1]
    adds    r1, r1, #4
    cmp     r1, r2
    bne     3b
4:

    /* 5. 初始化 C 运行时（可选，newlib 等需要） */
    /* bl      __libc_init_array */

    /* 6. 调用内核入口 */
    bl      kernel_main

    /* 7. 不应返回 */
    b       .

/* ===== 默认异常处理器 ===== */
.weak   NMI_Handler
.weak   HardFault_Handler
.weak   SVC_Handler
.weak   PendSV_Handler
.weak   SysTick_Handler

/* 占位外设中断处理器 */
.weak   TIMER_IRQ_0_Handler
.weak   TIMER_IRQ_1_Handler
.weak   TIMER_IRQ_2_Handler
.weak   TIMER_IRQ_3_Handler
.weak   PWM_IRQ_WRAP_Handler
.weak   USBCTRL_IRQ_Handler
.weak   XIP_IRQ_Handler
.weak   PIO0_IRQ_0_Handler
.weak   PIO0_IRQ_1_Handler
.weak   PIO1_IRQ_0_Handler
.weak   PIO1_IRQ_1_Handler
.weak   DMA_IRQ_0_Handler
.weak   DMA_IRQ_1_Handler
.weak   IO_IRQ_BANK0_Handler
.weak   IO_IRQ_QSPI_Handler
.weak   SIO_IRQ_PROC0_Handler
.weak   SIO_IRQ_PROC1_Handler
.weak   CLOCKS_IRQ_Handler
.weak   SPI0_IRQ_Handler
.weak   SPI1_IRQ_Handler
.weak   UART0_IRQ_Handler
.weak   UART1_IRQ_Handler
.weak   ADC_IRQ_FIFO_Handler
.weak   I2C0_IRQ_Handler
.weak   I2C1_IRQ_Handler
.weak   RTC_IRQ_Handler

/* 默认处理器实现：死循环 */
.section .text.default_handler, "ax", %progbits
Default_Handler:
    b       Default_Handler

/* 为所有弱符号提供默认实现 */
#define WEAK_HANDLER(name) \
    .weak name; \
    .thumb_set name, Default_Handler

WEAK_HANDLER(NMI_Handler)
WEAK_HANDLER(HardFault_Handler)
WEAK_HANDLER(SVC_Handler)
WEAK_HANDLER(PendSV_Handler)
WEAK_HANDLER(SysTick_Handler)
WEAK_HANDLER(TIMER_IRQ_0_Handler)
WEAK_HANDLER(TIMER_IRQ_1_Handler)
WEAK_HANDLER(TIMER_IRQ_2_Handler)
WEAK_HANDLER(TIMER_IRQ_3_Handler)
WEAK_HANDLER(PWM_IRQ_WRAP_Handler)
WEAK_HANDLER(USBCTRL_IRQ_Handler)
WEAK_HANDLER(XIP_IRQ_Handler)
WEAK_HANDLER(PIO0_IRQ_0_Handler)
WEAK_HANDLER(PIO0_IRQ_1_Handler)
WEAK_HANDLER(PIO1_IRQ_0_Handler)
WEAK_HANDLER(PIO1_IRQ_1_Handler)
WEAK_HANDLER(DMA_IRQ_0_Handler)
WEAK_HANDLER(DMA_IRQ_1_Handler)
WEAK_HANDLER(IO_IRQ_BANK0_Handler)
WEAK_HANDLER(IO_IRQ_QSPI_Handler)
WEAK_HANDLER(SIO_IRQ_PROC0_Handler)
WEAK_HANDLER(SIO_IRQ_PROC1_Handler)
WEAK_HANDLER(CLOCKS_IRQ_Handler)
WEAK_HANDLER(SPI0_IRQ_Handler)
WEAK_HANDLER(SPI1_IRQ_Handler)
WEAK_HANDLER(UART0_IRQ_Handler)
WEAK_HANDLER(UART1_IRQ_Handler)
WEAK_HANDLER(ADC_IRQ_FIFO_Handler)
WEAK_HANDLER(I2C0_IRQ_Handler)
WEAK_HANDLER(I2C1_IRQ_Handler)
WEAK_HANDLER(RTC_IRQ_Handler)

.end