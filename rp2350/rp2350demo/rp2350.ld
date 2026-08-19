/**
 * @file    rp2350.ld
 * @brief   RP2350 (Cortex-M33) 链接器脚本
 * 
 * 内存映射：
 *   Flash: 4MB (0x10000000 - 0x10400000) 
 *   RAM: 520KB (0x20000000 - 0x20082000)
 *   其中：
 *     - 核心 0 栈/堆: 低地址区
 *     - 核心 1 栈/堆: 高地址区  
 *     - 共享内存 IPC: 预留区域
 */

MEMORY
{
    /* Flash: 4MB = 4096KB = 0x400000 */
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4M
    
    /* RAM: 520KB = 532480 bytes = 0x82000 */
    /* 0x20000000 - 0x20082000 */
    RAM (rwx)   : ORIGIN = 0x20000000, LENGTH = 520K
    
    /* 核心 1 专用 RAM 区域 (高 128KB) */
    /* 0x20062000 - 0x20082000 (128KB) */
    CORE1_RAM (rwx) : ORIGIN = 0x20062000, LENGTH = 128K
    
    /* 共享内存 IPC 区域 (Core 0/1 通信) */
    /* 0x20060000 - 0x20062000 (8KB) */
    SHARED_RAM (rwx) : ORIGIN = 0x20060000, LENGTH = 8K
}

/* 栈顶符号 */
PROVIDE(_estack = ORIGIN(RAM) + LENGTH(RAM));
PROVIDE(_core1_stack_top = ORIGIN(CORE1_RAM) + LENGTH(CORE1_RAM));
PROVIDE(_shared_ram_start = ORIGIN(SHARED_RAM));
PROVIDE(_shared_ram_end = ORIGIN(SHARED_RAM) + LENGTH(SHARED_RAM));

/* ================================================================
 * 段定义
 * ================================================================ */

ENTRY(Reset_Handler)

SECTIONS
{
    /* ================================================================
     * 中断向量表 + 启动代码 + 只读代码段
     * ================================================================ */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > FLASH

    /* ================================================================
     * 只读数据
     * ================================================================ */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ================================================================
     * ARM 重定位支持
     * ================================================================ */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH

    /* ================================================================
     * 初始化数据段 (.data) - 从 Flash 复制到 RAM
     * ================================================================ */
    .data : AT (_sidata)
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    /* ================================================================
     * BSS 段 - 零初始化
     * ================================================================ */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* ================================================================
     * 核心 0 栈区域 (RAM 低地址部分)
     * ================================================================ */
    .stack_core0 (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_core0_start = .;
        . = ORIGIN(RAM) + 32K;  /* 32KB 栈 */
        _stack_core0_end = .;
    } > RAM

    /* ================================================================
     * 核心 1 栈区域 (CORE1_RAM 区域)
     * ================================================================ */
    .stack_core1 (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_core1_start = .;
        . = ORIGIN(CORE1_RAM) + LENGTH(CORE1_RAM);
        _stack_core1_end = .;
    } > CORE1_RAM

    /* ================================================================
     * 共享内存 IPC (SHARED_RAM)
     * ================================================================ */
    .ipc_shared (NOLOAD) :
    {
        . = ALIGN(4);
        _ipc_shared_start = .;
        _ipc_ctrl = .;           /* IPC 控制结构 */
        . += 256;
        _ipc_in_buf[0] = .;      /* Core0 -> Core1 缓冲区 0 */
        . += 8K;
        _ipc_in_buf[1] = .;      /* Core0 -> Core1 缓冲区 1 */
        . += 8K;
        _ipc_out_buf[0] = .;     /* Core1 -> Core0 缓冲区 0 */
        . += 8K;
        _ipc_out_buf[1] = .;     /* Core1 -> Core0 缓冲区 1 */
        . += 8K;
        . = ALIGN(4);
        _ipc_shared_end = .;
    } > SHARED_RAM

    /* ================================================================
     * Core 1 私有数据段 (CORE1_RAM)
     * ================================================================ */
    .data_core1 :
    {
        . = ALIGN(4);
        _sdata_core1 = .;
        *(.data_core1*)
        . = ALIGN(4);
        _edata_core1 = .;
    } > CORE1_RAM AT > FLASH

    .bss_core1 (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss_core1 = .;
        *(.bss_core1*)
        . = ALIGN(4);
        _ebss_core1 = .;
    } > CORE1_RAM

    /* ================================================================
     * 核心 1 启动代码 (放在 Flash, 运行时复制到 CORE1_RAM)
     * ================================================================ */
    .text_core1 :
    {
        . = ALIGN(4);
        *(.text_core1*)
        . = ALIGN(4);
    } > FLASH

    /* ================================================================
     * 调试信息
     * ================================================================ */
    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment*)
        *(.note.gnu.build-id)
    }

    /* ================================================================
     * 符号导出
     * ================================================================ */
    PROVIDE(_sidata = LOADADDR(.data));
    PROVIDE(_sdata = _sdata);
    PROVIDE(_edata = _edata);
    PROVIDE(_sbss = _sbss);
    PROVIDE(_ebss = _ebss);
    
    PROVIDE(_sdata_core1 = _sdata_core1);
    PROVIDE(_edata_core1 = _edata_core1);
    PROVIDE(_sbss_core1 = _sbss_core1);
    PROVIDE(_ebss_core1 = _ebss_core1);
    
    PROVIDE(_stack_core0_start = _stack_core0_start);
    PROVIDE(_stack_core0_end = _stack_core0_end);
    PROVIDE(_stack_core1_start = _stack_core1_start);
    PROVIDE(_stack_core1_end = _stack_core1_end);
    
    PROVIDE(_ipc_shared_start = _ipc_shared_start);
    PROVIDE(_ipc_shared_end = _ipc_shared_end);
    PROVIDE(_ipc_ctrl = _ipc_ctrl);
    PROVIDE(_ipc_in_buf = _ipc_in_buf);
    PROVIDE(_ipc_out_buf = _ipc_out_buf);
    
    PROVIDE(_core1_stack_top = ORIGIN(CORE1_RAM) + LENGTH(CORE1_RAM));
}