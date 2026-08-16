/**
 * @file    core1_worker.c
 * @brief   v2.6 Core1 worker —— 裸循环 WFE 事件驱动，SRAM 镜像执行
 *
 * 【铁律】整个编译单元链接进 .core1_text（VMA=0x20019000，LMA=Flash），
 * 由 ipc_start() 拷入条带 RAM CORE1 区后执行。因此严禁：
 *   · 任何 libc 调用（memcpy/printf → 引入 Flash 符号，XIP 争用 + 镜像外跳转）
 *   · 除法/取模（M0+ 无硬件除法 → __aeabi_uidiv 在 Flash）
 *   · 中断（不开 NVIC，纯 WFE 事件驱动；入口先 cpsid i）
 *   · 静态变量（避免依赖 .core1_bss 清零；状态全放 ctrl / 寄存器）
 *
 * 变换协议（与 shmem_ipc.c verify_out 严格一致）：
 *   out[i]  = in[i] + i   （i 为 32bit 字下标）
 *   out[末] = in[] 全体 XOR 折叠
 */
#include "shmem_ipc.h"

/* SIO FIFO 直接寄存器（不引 SDK 头，保持镜像自包含）
 * 【v2.6.6 根因修复】偏移 0x40/0x44 → 0x54/0x58（原值是 GPIO_HI_OE/
 * OE_SET，读恒 0；真 FIFO 见 shmem_ipc.c 同步注释） */
#define W_SIO_BASE  0xD0000000u
#define W_FIFO_WR   (*(volatile uint32_t *)(W_SIO_BASE + 0x54u))
#define W_FIFO_RD   (*(volatile uint32_t *)(W_SIO_BASE + 0x58u))
#define W_FIFO_ST   (*(volatile uint32_t *)(W_SIO_BASE + 0x50u))
#define W_ST_VLD    1u

#define W_DMB()     __asm volatile ("dmb" ::: "memory")
#define W_WFE()     __asm volatile ("wfe")

__attribute__((section(".core1_text"), noinline, used))
static void core1_body(void) {
    volatile ipc_ctrl_t *ctrl = (volatile ipc_ctrl_t *)IPC_CTRL_BASE;

    ctrl->c1_hb = 0u;
    ctrl->c1_frames = 0u;
    ctrl->c1_err = 0u;
    W_DMB();
    W_FIFO_WR = IPC_MSG_MAKE(IPC_MSG_HELLO, 0u, 0u);   /* 通知 Core0：worker 已跑起来 */

    for (;;) {
        ctrl->c1_hb++;                                  /* 心跳：单字原子写 */

        /* 先清空 FIFO 再 WFE：event register 粘滞语义保证不丢唤醒 */
        while ((W_FIFO_ST & W_ST_VLD) != 0u) {
            uint32_t m = W_FIFO_RD;
            W_DMB();                                    /* 先取通知再读数据 */

            if (IPC_MSG_TYPE(m) == IPC_MSG_READY) {
                uint32_t n = IPC_MSG_BUF(m) & 1u;
                volatile uint32_t *in  = IPC_IN(n);     /* READY 送达后所有权在我 */
                volatile uint32_t *out = IPC_OUT(n);
                uint32_t fold = 0u;

                for (uint32_t i = 0u; i < IPC_BUF_WORDS; i++) {
                    uint32_t v = in[i];
                    out[i] = v + i;
                    fold ^= v;
                }
                out[IPC_BUF_WORDS - 1u] = fold;         /* 末字覆盖为 XOR 折叠 */

                ctrl->c1_frames++;
                ctrl->last_frame_done = IPC_MSG_FID(m);
                W_DMB();                                /* 先写数据再还所有权 */
                W_FIFO_WR = IPC_MSG_MAKE(IPC_MSG_DONE, IPC_MSG_FID(m), n);
            }
        }
        W_WFE();                                        /* 等 Core0 的 SEV */
    }
}

/* 入口：multicore_launch_core1_with_stack 跳到这里（SP 已设好） */
__attribute__((section(".core1_text"), used))
void core1_worker_entry(void) {
    __asm volatile ("cpsid i");                         /* 不接受任何中断 */
    core1_body();
    for (;;) { W_WFE(); }                               /* noreturn 守卫 */
}
