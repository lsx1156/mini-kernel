/**
 * @file    context_switch.S
 * @brief   Cortex-M0+ (RP2040) 上下文切换裸汇编实现
 * @note    使用 PendSV 实现任务切换，栈帧布局与 hal_context_t 严格对应
 *          编译器：arm-none-eabi-gcc / clang
 */
.syntax unified
.cpu cortex-m0plus
.thumb

/* C 侧回调：由 PendSV 调用，完成 from/to 调度与 SP 交换
 *   uint32_t *sched_do_switch(uint32_t *old_sp);
 *     参数：old_sp = 已 push r4-r11 后的 PSP
 *     返回：new_sp = 要切到的任务 SP（汇编侧再 pop r4-r11） */
.extern sched_do_switch

/* ================================================================
 * hal_context_t 布局（必须与 kernel/core/task.h 保持一致）
 * struct hal_context {
 *     uint32_t *sp;           // 偏移 0: 栈指针（仅保存 SP，其余由硬件自动入栈）
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
    /* Cortex-M0+ 异常返回自动弹出: xPSR, PC, LR, R12, R3, R2, R1, R0 */

    /* 【AAPCS 修复 · 保存 r4】
     *
     *   旧版用 r4 做 scratch 但不保存/恢复 → 违反 AAPCS 调用约定。
     *   调用者 task_module_init 里 r4=&g_idle_task 被覆盖为 stack_top-64
     *   → g_idle_task.sp 写到错误地址（永远是 0）→ 调度器切到 idle 时
     *   从 sp=0 恢复上下文 → HardFault → LED 爆闪。
     *
     *   修复：push {r4} 保存，用完 pop {r4} 恢复。 */
    push    {r4}

    /* 栈向下增长，先对齐到 8 字节（M0+ 无 bic 立即数，用 lsrs/lsls 清低 3 位） */
    mov     r4, r1                  /* r4 = stack_top */
    lsrs    r4, r4, #3              /* r4 >>= 3 */
    lsls    r4, r4, #3              /* r4 <<= 3 （低 3 位被清零，8 字节对齐） */

    /* 【关键修复 · 栈帧布局必须与 PendSV handler 匹配】
     *
     *   PendSV handler 进入时：
     *     1. 硬件自动 push r0-r3/r12/lr/pc/xpsr 到 PSP (32 字节)
     *     2. PendSV 手动 push r4-r11 (再 32 字节)
     *     3. old_sp = PSP - 32 (指向 r4-r11 区域)
     *     4. old_sp + 32 = 指向硬件栈帧
     *
     *   PendSV 退出时（切换到 to 任务）：
     *     1. 从 to->sp 恢复 r4-r11
     *     2. PSP = to->sp + 32 (指向硬件栈帧)
     *     3. bx lr → 硬件从 PSP pop r0-r3/r12/lr/pc/xpsr
     *
     *   所以 to->sp 必须指向 r4-r11 保存区域，to->sp + 32 必须指向硬件栈帧。
     *
     *   旧版 Bug：只预留 32 字节(硬件栈帧)，to->sp = stack_top - 32。
     *   首次 PendSV 切换时 PSP = to->sp + 32 = stack_top → 超出栈帧范围 →
     *   硬件从垃圾地址弹栈 → PC 乱飞 → HardFault → LED 爆闪/快闪。
     *
     *   修复：额外预留 32 字节给 r4-r11（初始值不初始化，任务 entry 会
     *   自己设置 callee-saved 寄存器）。ctx->sp 指向 r4-r11 区域起始。 */
    subs    r4, r4, #32             /* 给 r4-r11 预留空间（不初始化） */
    subs    r4, r4, #32             /* 给硬件栈帧预留空间 */

    /* 硬件栈帧从 r4 + 32 开始写入（r4 + 32 ~ r4 + 63）：
     * R0, R1, R2, R3, R12, LR, PC, xPSR
     *
     * 【Bug Fix #2 · PC 强制 Thumb LSB=1】 */
    movs    r1, r2                  /* r1 = entry (原始) */
    movs    r2, #1
    orrs    r1, r1, r2              /* r1 = entry | 1（Thumb 强制） */

    /* 【Bug Fix #3 · LR 不写 EXC_RETURN】 */
    str     r3, [r4, #32]           /* R0 = arg (传给 entry 的唯一参数) */
    movs    r3, #0
    str     r3, [r4, #36]           /* R1 = 0 */
    str     r3, [r4, #40]           /* R2 = 0 */
    str     r3, [r4, #44]           /* R3 = 0 */
    str     r3, [r4, #48]           /* R12 = 0 */
    str     r3, [r4, #52]           /* LR = 0 (noreturn 任务不会返回) */
    str     r1, [r4, #56]           /* PC = entry | 1 (Thumb) */
    ldr     r3, =0x01000000         /* xPSR: Thumb T-bit (bit24) = 1 */
    str     r3, [r4, #60]           /* xPSR */

    /* 保存初始 SP 到 ctx->sp（指向 r4-r11 区域，PendSV 从这里恢复 r4-r11） */
    str     r4, [r0, #0]

    pop     {r4}
    bx      lr
.size   hal_context_init, .-hal_context_init

/* ================================================================
 * void hal_yield_trigger(void);
 * 仅触发 PendSV，不保存/恢复 SP（用于时间片轮转主动让出）
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
 * 【CRITICAL · 最终向量方案：主实现直接用 SDK crt0 需要的 isr_xxx 命名】
 * ----------------------------------------------------------------
 * 踩坑血泪史：
 *   之前用 `.set isr_pendsv, PendSV_Handler` 做符号别名，结果 GAS
 *   不保证把 PendSV_Handler 的 Thumb LSB=1 传递给 isr_pendsv。链接器
 *   把 SDK 向量表填上 LSB=0 的地址 → 异常入口 CPU 看到 bit0=0 以为
 *   要切 ARM 状态 → 立即 INVSTATE UsageFault → 升级为 HardFault →
 *   HardFault 向量也 LSB=0 → 双 fault → CPU **进入 Lockup 状态**
 *   （LED 永远灭，连 HardFault 快闪的 Handler 都进不去）。
 *
 * 最终修复（100% 可靠）：
 *   (1) 主函数标签 **直接就是 SDK 向量表里 .weak 占位的同名符号**
 *       (isr_pendsv / isr_svcall / isr_systick / isr_hardfault / isr_nmi)
 *       且 `.thumb_func` + `.type %function` 直接贴在这些 isr_xxx
 *       身上 → 汇编器 100% 给这些符号值 LSB=1 → 链接器填入向量表
 *       的条目肯定合法。
 *   (2) ARM CMSIS 标准名（PendSV_Handler 等）在 isr_xxx 标签之后
 *       再写一个别名标签，放在同一地址紧跟其后，复用同一 .thumb_func
 *       标记范围。这样任何 C 代码通过标准名引用（如 kernel.c 里的
 *       weak SysTick_Handler 兼容符号）依旧正常工作，且彻底避免了
 *       `.set` 造成的 Thumb 位丢失问题。
 * ================================================================ */

/* ================================================================
 * isr_pendsv / PendSV_Handler：真正的上下文切换（★ 修复：保存/恢复 r4-r11）
 *
 * 【之前实现的致命漏洞】：
 *   旧版本只有 `bx lr` —— 硬件仅自动恢复 r0-r3/r12/lr/pc/xpsr（8 个），
 *   **Callee-saved 寄存器 r4-r11 完全没保存/恢复**。任务 A 被切出时，
 *   r4-r11 残留在寄存器里；切到任务 B 执行（B 编译器会使用 r4-r11 存放
 *   局部变量/循环计数等）→ 再切回 A 时 r4-r11 = B 的垃圾值 → A 跑飞崩溃
 *   （表现：随机 HardFault / 死循环 / 内存踩坏）。
 *
 * 【本实现修复】：严格按照 ARM AAPCS 调用约定保存 Callee-saved：
 *   1. 进 handler → mrs psp → 手动在任务栈上"压" r4-r11（Cortex-M0+
 *      push/pop 指令仅支持 r0-r7，高 4 寄存器 r8-r11 用 sub+str 模拟）
 *   2. 调 C `sched_do_switch(old_sp)`：选下一个任务 + 交换 SP
 *   3. 从返回的 new_sp 手动"弹" r4-r11
 *   4. msr psp + bx lr → 硬件自动弹剩下 8 个寄存器并跳入新任务
 * ================================================================ */
.global isr_pendsv
.global PendSV_Handler
.type   isr_pendsv,     %function
.type   PendSV_Handler, %function
.thumb_func
isr_pendsv:
PendSV_Handler:
    /* ---------- Step 1: 保存当前任务的 r4-r11（Callee-saved） ---------- */
    mrs     r0, psp                /* r0 = PSP (PendSV 硬件已 push 8 栈帧: PSP = 原 PSP - 32) */
    subs    r0, r0, #32            /* 再向下预留 32 字节存 r4~r11 → 到达 r4-r11 区域 */
    str     r4, [r0, #0]           /* 低 4 个 r4~r7：直接 str */
    str     r5, [r0, #4]
    str     r6, [r0, #8]
    str     r7, [r0, #12]
    mov     r1, r8                 /* 高 4 个 r8~r11：先 mov 到低寄存器 r1 再 str */
    str     r1, [r0, #16]
    mov     r1, r9
    str     r1, [r0, #20]
    mov     r1, r10
    str     r1, [r0, #24]
    mov     r1, r11
    str     r1, [r0, #28]

    /* ---------- Step 2: 交给 C 调度器选下一个任务并交换 SP ---------- */
    /* r0 = old_sp（保存完 r4-r11 后的 PSP，就是 from 任务的完整栈顶）
     *
     * 【关键修复 · EXC_RETURN 保存】
     *   bl 指令会覆盖 LR。进入 PendSV 时 LR = EXC_RETURN(0xFFFFFFFD)，
     *   bl sched_do_switch 后 LR = bl 的返回地址 → bx lr 无法异常返回！
     *   修复：bl 前把 EXC_RETURN 存到 r4（r4 已在上方保存到栈，可重用）。
     *   sched_do_switch 遵守 AAPCS，保持 callee-saved r4 不变。 */
    mov     r4, lr                 /* r4 = EXC_RETURN */
    bl      sched_do_switch        /* r0 <- sched_do_switch(old_sp) = new_sp */

    /* ---------- Step 3: 恢复 to 任务的 r4-r11 ---------- */
    ldr     r1, [r0, #16]          /* 先恢复高 4 个 r8~r11（ldr 到低寄存器 + mov 到高寄存器） */
    mov     r8, r1
    ldr     r1, [r0, #20]
    mov     r9, r1
    ldr     r1, [r0, #24]
    mov     r10, r1
    ldr     r1, [r0, #28]
    mov     r11, r1

    mov     lr, r4                 /* 恢复 EXC_RETURN 到 LR（在覆盖 r4 之前） */

    ldr     r4, [r0, #0]           /* 再恢复低 4 个 r4~r7 */
    ldr     r5, [r0, #4]
    ldr     r6, [r0, #8]
    ldr     r7, [r0, #12]
    adds    r0, r0, #32            /* 恢复 PSP 到"未 push r4-r11 时的位置"，硬件接下来会从这 pop 8 栈帧 */

    /* ---------- Step 4: 切换 PSP + bx lr 退出 handler ---------- */
    msr     psp, r0
    bx      lr                     /* LR = EXC_RETURN：从 PSP 弹栈 + 返回线程模式 */
.size   isr_pendsv,     .-isr_pendsv
.size   PendSV_Handler, .-isr_pendsv

/* ================================================================
 * isr_systick / SysTick_Handler：兼容保留（真实 tick 走 TIMER_IRQ_n）
 * SDK 向量表默认 isr_systick = bkpt#0，覆盖以防万一。
 * ================================================================ */
.global isr_systick
.global SysTick_Handler
.type   isr_systick,    %function
.type   SysTick_Handler, %function
.thumb_func
isr_systick:
SysTick_Handler:
    bx      lr
.size   isr_systick,    .-isr_systick
.size   SysTick_Handler, .-isr_systick

/* ================================================================
 * isr_svcall / SVC_Handler：启动首个任务（sched_start 里 svc #0）
 *
 *   【关键修复 · SVC 异常帧不覆盖任务初始栈帧】
 *
 *   sched_start 设置 PSP = task->sp（r4-r11 区域），svc #0 触发时硬件
 *   把异常帧（32 字节）压到 PSP 上 → r4-r11 区域被覆盖，但 task->sp+32
 *   的硬件栈帧（r0=arg, pc=entry|1 等）保持完整。
 *
 *   此处将 PSP 加 32 回到硬件栈帧起始位置，再 bx EXC_RETURN → 硬件
 *   从正确位置弹出任务初始上下文 → 跳入 entry(arg)。
 * ================================================================ */
.global isr_svcall
.global SVC_Handler
.type   isr_svcall,   %function
.type   SVC_Handler,  %function
.thumb_func
isr_svcall:
SVC_Handler:
    /* 【关键 · SVC 入口时 PSP 的状态】
     *
     *   sched_start 执行时 CPU 在 Thread 模式 + CONTROL.SPSEL=0（用 MSP）。
     *   svc #0 触发异常时，硬件将异常帧压入 **MSP**（不是 PSP！），
     *   PSP 保持不变 = sched_start 里 msr psp 设置的 task->sp。
     *
     *   task->sp 指向 r4-r11 区域（ctx->sp = stack_top - 64），
     *   硬件栈帧在 task->sp + 32（ctx->sp + 32 = stack_top - 32）。
     *
     *   所以只需 PSP += 32 即可指向硬件栈帧，然后 bx EXC_RETURN
     *   让硬件从 PSP 弹出 {R0,R1,R2,R3,R12,LR,PC,xPSR} 启动首任务。
     *
     *   之前误用 +64（假设 SVC 压入 PSP）→ PSP 越过硬件栈帧 →
     *   弹出垃圾 PC → HardFault → LED 爆闪。 */
    mrs     r0, psp                 /* r0 = PSP = task->sp（SVC 未修改 PSP） */
    adds    r0, r0, #32             /* r0 = task->sp + 32 = 硬件栈帧起始 */
    msr     psp, r0                 /* PSP = 硬件栈帧起始 */
    ldr     r0, =0xFFFFFFFD         /* EXC_RETURN: Thread mode + PSP 弹栈 */
    bx      r0
.size   isr_svcall,   .-isr_svcall
.size   SVC_Handler,  .-isr_svcall

/* ================================================================
 * isr_hardfault / HardFault_Handler
 *
 * 【关键修复 · Lockup 风险消除】
 *   之前版本在此 handler 开头再次写 IO_BANK0 / PADS_BANK0 兜底，
 *   但写 APB 外设 → 若在此之前总线已经处于异常状态 → 精确总线错 →
 *   "在 HardFault 压栈阶段再 Fault" 双 Fault 直接 CPU Lockup (LED 灭)。
 *   改为：main() 开头 SDK gpio_init() 已经把 Mux/Pads/OE 全配好了，
 *   此处不碰任何 APB 寄存器，**只写 SIO_OUT_SET/CLR**（0xD0000014/18，
 *   AHB-Lite 单周期端口，任何情况下都不会触发总线错误）。
 *
 * 节奏：100ms/100ms 快闪（5Hz）死循环。
 * ================================================================ */
.global isr_hardfault
.global HardFault_Handler
.type   isr_hardfault,    %function
.type   HardFault_Handler, %function
.thumb_func
isr_hardfault:
HardFault_Handler:
    ldr     r0, =0xD0000000         /* SIO base (AHB-Lite, 安全) */
    movs    r1, #1
    lsls    r1, r1, #25             /* r1 = GPIO25 mask */
HF_loop:
    str     r1, [r0, #0x014]        /* OUT_SET = ON  */
    ldr     r2, =1250000            /* ~100ms ON @ 125MHz */
HF_on:
    subs    r2, r2, #1; bne HF_on
    str     r1, [r0, #0x018]        /* OUT_CLR = OFF */
    ldr     r2, =1250000            /* ~100ms OFF */
HF_off:
    subs    r2, r2, #1; bne HF_off
    b       HF_loop
.size   isr_hardfault,    .-isr_hardfault
.size   HardFault_Handler, .-isr_hardfault

/* ================================================================
 * isr_nmi / NMI_Handler：3 连闪 + 长停顿 循环
 * 同样：不碰 IO_BANK0/PADS_BANK0，只写 SIO 寄存器。
 * ================================================================ */
.global isr_nmi
.global NMI_Handler
.type   isr_nmi,      %function
.type   NMI_Handler,  %function
.thumb_func
isr_nmi:
NMI_Handler:
    ldr     r0, =0xD0000000         /* SIO base */
    movs    r1, #1
    lsls    r1, r1, #25             /* GPIO25 mask */
NMI_loop:
    movs    r3, #3
NMI_burst:
    str     r1, [r0, #0x014]        /* ON */
    ldr     r2, =2000000            /* ~160ms ON */
NMI_on:
    subs    r2, r2, #1; bne NMI_on
    str     r1, [r0, #0x018]        /* OFF */
    ldr     r2, =2000000            /* ~160ms OFF */
NMI_off:
    subs    r2, r2, #1; bne NMI_off
    subs    r3, r3, #1
    bne     NMI_burst
    ldr     r2, =6000000            /* ~480ms 长停顿，肉眼分组清晰 */
NMI_pause:
    subs    r2, r2, #1; bne NMI_pause
    b       NMI_loop
.size   isr_nmi,      .-isr_nmi
.size   NMI_Handler,  .-isr_nmi

/* ================================================================
 * isr_invalid：覆盖 SDK 默认的 weak isr_invalid（= bkpt #0）
 *
 * SDK 的 isr_irq0 ~ isr_irq31 全部通过 `.weak isr_irq0` +
 * `.set isr_irq0, isr_invalid` 指向本符号。如果某个外设 IRQ
 * 被使能但未注册 handler（比如 sleep_ms 的 alarm pool 抢到了
 * alarm 但没正确连接 callback），默认实现就是 `bkpt #0`，
 * 在无调试器时触发 HardFault → 但 HardFault 向量里若又因
 * BKPT 嵌套重入 → CPU 进入 Lockup（LED 永远灭，连 HardFault
 * 快闪都看不到）。
 *
 * 解决：把 isr_invalid 直接替换成 HardFault 同款 5Hz 快闪，
 *       这样任何"未注册 handler 的外设 IRQ"都给出明确可见的
 *       诊断反馈而不是静默 Lockup 灭灯。
 * ================================================================ */
.global isr_invalid
.type   isr_invalid,    %function
.thumb_func
isr_invalid:
    b       isr_hardfault           /* 直接跳到 HardFault 快闪，不必重复代码 */
.size   isr_invalid,    .-isr_invalid

.end
