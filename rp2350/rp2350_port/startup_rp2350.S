/**
 * @file    startup_rp2350.S
 * @brief   RP2350 (Cortex-M33 + RISC-V) 启动代码
 */

.syntax unified
.cpu cortex-m33
.fpu fpv5-sp-d16
.thumb

/* ================================================================
 * 向量表
 * ================================================================ */

.section .isr_vector,"a",%progbits
.global __Vectors
__Vectors:
    .word   _estack                    /* 初始栈指针 */
    .word   Reset_Handler             /* 复位向量 */
    .word   NMI_Handler               /* NMI */
    .word   HardFault_Handler         /* HardFault */
    .word   MemManage_Handler         /* MemManage */
    .word   BusFault_Handler          /* BusFault */
    .word   UsageFault_Handler        /* UsageFault */
    .word   SecureFault_Handler       /* SecureFault (TrustZone) */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   SVC_Handler               /* SVCall */
    .word   DebugMon_Handler          /* DebugMon */
    .word   0                         /* 保留 */
    .word   PendSV_Handler            /* PendSV */
    .word   SysTick_Handler           /* SysTick */
    
    /* 外部中断向量 */
    .word   TIMER0_IRQHandler         /* Timer 0 */
    .word   TIMER1_IRQHandler         /* Timer 1 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   IO_IRQ_BANK0              /* GPIO Bank 0 */
    .word   IO_IRQ_QSPI               /* QSPI */
    .word   SIO_IRQ_FIFO              /* SIO FIFO */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   ADC0_IRQ_FIFO             /* ADC */
    .word   0                         /* 保留 */
    .word   USBCTRL_IRQ               /* USB */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   RTC_IRQ                   /* RTC */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   DMA_IRQ_0                 /* DMA 0 */
    .word   DMA_IRQ_1                 /* DMA 1 */
    .word   DMA_IRQ_2                 /* DMA 2 */
    .word   DMA_IRQ_3                 /* DMA 3 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   IO_IRQ_BANK0_NS           /* GPIO Bank 0 Non-secure */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   PIO0_IRQ_0                /* PIO 0 */
    .word   PIO0_IRQ_1                /* PIO 0 */
    .word   PIO1_IRQ_0                /* PIO 1 */
    .word   PIO1_IRQ_1                /* PIO 1 */
    .word   PIO2_IRQ_0                /* PIO 2 (新增) */
    .word   PIO2_IRQ_1                /* PIO 2 (新增) */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   0                         /* 保留 */
    .word   USBCTRL_IRQ_NS            /* USB Non-secure */
    .word   0                         /* 保留 */

.global Reset_Handler
Reset_Handler:
    /* 1. 设置中断向量表偏移 (VTOR / VT0R) */
    ldr     r0, =0xE000ED08           /* VTOR (VT0R) */
    ldr     r1, =__Vectors
    str     r1, [r0]
    
    /* 2. 启用 FPU (Cortex-M33) */
    ldr     r0, =0xE000ED88           /* CPACR */
    ldr     r1, [r0]
    orr     r1, r1, #0xF00000         /* 使能 CP10/CP11 (FPU) */
    str     r1, [r0]
    dsb
    isb
    
    /* 3. 启用 TrustZone (如果需要) */
#ifdef ENABLE_TRUSTZONE
    ldr     r0, =0xE000E000           /* SAU_CTRL */
    movs    r1, #1
    str     r1, [r0]
#endif
    
    /* 4. 初始化 .data 和 .bss 段 */
    ldr     r0, =_sidata
    ldr     r1, =_sdata
    ldr     r2, =_edata
    cmp     r1, r2
    it      eq
    beq     copy_bss
copy_data:
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    cmp     r1, r2
    bne     copy_data
    
copy_bss:
    ldr     r1, =_sbss
    ldr     r2, =_ebss
    cmp     r1, r2
    it      eq
    beq     call_main
zero_bss:
    movs    r3, #0
    str     r3, [r1], #4
    cmp     r1, r2
    bne     zero_bss
    
    /* 5. 调用 main */
call_main:
    bl      main
    
    /* 不应返回 */
    b       .

/* 启动 Core 1 (如果定义了 ENABLE_TRUSTZONE 且需要双核) */
#ifdef ENABLE_TRUSTZONE
    bl      multicore_launch_core1
#endif

    /* 不应返回 */
    b       .

/* 默认中断处理 */
.weak   NMI_Handler
.weak   HardFault_Handler
.weak   MemManage_Handler
.weak   BusFault_Handler
.weak   UsageFault_Handler
.weak   SecureFault_Handler
.weak   SVC_Handler
.weak   DebugMon_Handler
.weak   PendSV_Handler
.weak   SysTick_Handler

NMI_Handler:
HardFault_Handler:
MemManage_Handler:
BusFault_Handler:
UsageFault_Handler:
SecureFault_Handler:
SVC_Handler:
DebugMon_Handler:
PendSV_Handler:
SysTick_Handler:
    b       .

/* 外部中断弱定义 */
.weak   TIMER0_IRQHandler
.weak   TIMER1_IRQHandler
.weak   IO_IRQ_BANK0
.weak   IO_IRQ_QSPI
.weak   SIO_IRQ_FIFO
.weak   ADC0_IRQ_FIFO
.weak   USBCTRL_IRQ
.weak   RTC_IRQ
.weak   DMA_IRQ_0
.weak   DMA_IRQ_1
.weak   DMA_IRQ_2
.weak   DMA_IRQ_3
.weak   IO_IRQ_BANK0_NS
.weak   PIO0_IRQ_0
.weak   PIO0_IRQ_1
.weak   PIO1_IRQ_0
.weak   PIO1_IRQ_1
.weak   PIO2_IRQ_0
.weak   PIO2_IRQ_1
.weak   USBCTRL_IRQ_NS

TIMER0_IRQHandler:
TIMER1_IRQHandler:
IO_IRQ_BANK0:
IO_IRQ_QSPI:
SIO_IRQ_FIFO:
ADC0_IRQ_FIFO:
USBCTRL_IRQ:
RTC_IRQ:
DMA_IRQ_0:
DMA_IRQ_1:
DMA_IRQ_2:
DMA_IRQ_3:
IO_IRQ_BANK0_NS:
PIO0_IRQ_0:
PIO0_IRQ_1:
PIO1_IRQ_0:
PIO1_IRQ_1:
PIO2_IRQ_0:
PIO2_IRQ_1:
USBCTRL_IRQ_NS:
    b       .

/* 栈顶符号 */
.global _estack
.weak   _estack