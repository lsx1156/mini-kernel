/**
 * @file    context_switch.S
 * @brief   Cortex-M33 (RP2350) 上下文切换裸汇编实现
 * @note    使用 PendSV 实现任务切换，支持 FPU 和 TrustZone
 */

.syntax unified
.cpu cortex-m33
.fpu fpv5-sp-d16
.thumb

/* C 侧回调：由 PendSV 调用，完成 from/to 调度与 SP 交换
 *   uint32_t *sched_do_switch(uint32_t *old_sp);
 *     参数：old_sp = 已 push r4-r11 后的 PSP
 *     返回：new_sp = 要切到的任务 SP (汇编侧再 pop r4-r11) */
.extern sched_do_switch

/* ================================================================
 * hal_context_t 布局 (必须与 kernel/core/task.h 保持一致)
 * struct hal_context {
 *     uint32_t *sp;           // 偏移 0: 栈指针 (PSP)，仅保存 SP，其余由硬件自动入栈
 * };
 * ================================================================ */

/* ================================================================
 * void hal_context_init(hal_context_t *ctx, void *stack_top,
 *                       void (*entry)(void *), void *arg);
 * 初始化任务栈帧，模拟异常返回现场，使首次切换能直接跳入 entry(arg)
 * ================================================================ */
.global hal_context_init
.type   hal_context_init, %function
.thumb_func
hal_context_init:
    /* r0 = ctx, r1 = stack_top, r2 = entry, r3 = arg */
    /* Cortex-M33 异常返回自动弹出: xPSR, PC, LR, R12, R3, R2, R1, R0 */
    /* 然后手动恢复 r4-r11 (共 32 字节) */

    push    {r4, r5, r6, r7, r8, r9, r10, r11, lr}

    mov     r4, r1                  /* r4 = stack_top */
    
    /* 对齐到 8 字节 */
    bic     r4, r4, #7

    /* 预留 r4-r11 空间 (32 字节) */
    sub     r4, r4, #32

    /* 预留硬件栈帧空间 (32 字节): R0, R1, R2, R3, R12, LR, PC, xPSR */
    sub     r4, r4, #32

    /* 硬件栈帧从 r4 + 32 开始 */
    mov     r5, r2                  /* r5 = entry */
    orr     r5, r5, #1              /* Thumb bit */
    
    str     r3, [r4, #32]           /* R0 = arg */
    mov     r6, #0
    str     r6, [r4, #36]           /* R1 = 0 */
    str     r6, [r4, #40]           /* R2 = 0 */
    str     r6, [r4, #44]           /* R3 = 0 */
    str     r6, [r4, #48]           /* R12 = 0 */
    str     r6, [r4, #52]           /* LR = 0 */
    str     r5, [r4, #56]           /* PC = entry | 1 */
    ldr     r6, =0x01000000         /* xPSR: Thumb bit (bit 24) */
    str     r6, [r4, #60]           /* xPSR */

    /* 保存初始 SP 到 ctx->sp (指向 r4-r11 区域) */
    str     r4, [r0, #0]

    pop     {r4, r5, r6, r7, r8, r9, r10, r11, pc}
.size   hal_context_init, .-hal_context_init


/* ================================================================
 * void hal_yield_trigger(void);
 * 仅触发 PendSV，不保存/恢复 SP
 * ================================================================ */
.global hal_yield_trigger
.type   hal_yield_trigger, %function
.thumb_func
hal_yield_trigger:
    ldr     r0, =0xE000ED04         /* ICSR */
    ldr     r1, =0x10000000         /* PENDSVSET (1<<28) */
    str     r1, [r0]
    bx      lr
.size   hal_yield_trigger, .-hal_yield_trigger


/* ================================================================
 * isr_pendsv / PendSV_Handler：真正的上下文切换
 * 支持 FPU 自动保存/恢复 (Cortex-M33 有硬件 FPU 支持)
 * ================================================================ */
.global isr_pendsv
.global PendSV_Handler
.type   isr_pendsv,     %function
.type   PendSV_Handler, %function
.thumb_func
isr_pendsv:
PendSV_Handler:
    /* ---------- Step 1: 保存当前任务的 r4-r11 (Callee-saved) ---------- */
    mrs     r0, psp                /* r0 = PSP (硬件已 push r0-r3/r12/lr/pc/xpsr: PSP = 原 PSP - 32) */
    sub     r0, r0, #32            /* 再向下预留 32 字节存 r4~r11 */
    
    stmia   r0!, {r4-r11}          /* 保存 r4-r11 (8 寄存器，32 字节) */

    /* ---------- Step 2: 交给 C 调度器选下一个任务并交换 SP ---------- */
    /* r0 = old_sp (保存完 r4-r11 后的 PSP) */
    mov     r4, lr                 /* r4 = EXC_RETURN */
    bl      sched_do_switch        /* r0 <- sched_do_switch(old_sp) = new_sp */
    
    /* ---------- Step 3: 恢复 to 任务的 r4-r11 ---------- */
    ldmia   r0!, {r4-r11}          /* 恢复 r4-r11 */
    
    mov     lr, r4                 /* 恢复 EXC_RETURN 到 LR */
    
    add     r0, r0, #32            /* 恢复 PSP 到硬件栈帧位置 */
    
    /* ---------- Step 4: 切换 PSP + bx lr 退出 handler ---------- */
    msr     psp, r0
    bx      lr
.size   isr_pendsv,     .-isr_pendsv
.size   PendSV_Handler, .-isr_pendsv


/* ================================================================
 * isr_svcall / SVC_Handler：启动首个任务
 * ================================================================ */
.global isr_svcall
.global SVC_Handler
.type   isr_svcall,   %function
.type   SVC_Handler,  %function
.thumb_func
isr_svcall:
SVC_Handler:
    /* SVC 入口时 PSP 的状态:
     *   sched_start 执行时在 Thread 模式 + MSP
     *   svc #0 触发异常时，硬件将异常帧压入 MSP
     *   PSP 保持不变 = sched_start 里 msr psp 设置的 task->sp
     * 
     * task->sp 指向 r4-r11 区域，硬件栈帧在 task->sp + 32
     */
    mrs     r0, psp                 /* r0 = PSP = task->sp (SVC 未修改 PSP) */
    add     r0, r0, #32             /* r0 = task->sp + 32 = 硬件栈帧起始 */
    msr     psp, r0                 /* PSP = 硬件栈帧起始 */
    ldr     r0, =0xFFFFFFFD         /* EXC_RETURN: Thread mode + PSP 弹栈 */
    bx      r0
.size   isr_svcall,   .-isr_svcall
.size   SVC_Handler,  .-isr_svcall


/* ================================================================
 * HardFault_Handler
 * ================================================================ */
.extern hardfault_dump_c
.extern g_fault_psp
.extern g_fault_msp
.extern g_fault_lr

.global isr_hardfault
.global HardFault_Handler
.type   isr_hardfault,    %function
.type   HardFault_Handler, %function
.thumb_func
isr_hardfault:
HardFault_Handler:
    mrs     r0, psp
    ldr     r1, =g_fault_psp
    str     r0, [r1]
    mrs     r0, msp
    ldr     r1, =g_fault_msp
    str     r0, [r1]
    
    /* 保存 EXC_RETURN */
    push    {lr}
    pop     {r3}
    ldr     r1, =g_fault_lr
    str     r3, [r1]
    
    bl      hardfault_dump_c
    
    /* 死循环闪灯 (GPIO 25) */
    ldr     r0, =0xD0000000         /* SIO base */
    movs    r1, #1
    lsls    r1, r1, #25             /* GPIO25 mask */
HF_loop:
    str     r1, [r0, #0x14]         /* OUT_SET = ON */
    ldr     r2, =1500000            /* ~100ms @ 150MHz */
HF_on:
    subs    r2, r2, #1; bne HF_on
    str     r1, [r0, #0x18]         /* OUT_CLR = OFF */
    ldr     r2, =1500000
HF_off:
    subs    r2, r2, #1; bne HF_off
    b       HF_loop
.size   isr_hardfault,    .-isr_hardfault
.size   HardFault_Handler, .-isr_hardfault


/* ================================================================
 * NMI_Handler
 * ================================================================ */
.global isr_nmi
.global NMI_Handler
.type   isr_nmi,      %function
.type   NMI_Handler,  %function
.thumb_func
isr_nmi:
NMI_Handler:
    ldr     r0, =0xD0000000
    movs    r1, #1
    lsls    r1, r1, #25
NMI_loop:
    movs    r3, #3
NMI_burst:
    str     r1, [r0, #0x14]
    ldr     r2, =2400000
NMI_on:
    subs    r2, r2, #1; bne NMI_on
    str     r1, [r0, #0x18]
    ldr     r2, =2400000
NMI_off:
    subs    r2, r2, #1; bne NMI_off
    subs    r3, r3, #1
    bne     NMI_burst
    ldr     r2, =7200000
NMI_pause:
    subs    r2, r2, #1; bne NMI_pause
    b       NMI_loop
.size   isr_nmi,      .-isr_nmi
.size   NMI_Handler,  .-NMI_Handler


/* ================================================================
 * isr_invalid：覆盖 SDK 默认的 weak isr_invalid
 * ================================================================ */
.global isr_invalid
.type   isr_invalid,    %function
.thumb_func
isr_invalid:
    b       isr_hardfault
.size   isr_invalid,    .-isr_invalid

.end