/**
 * @file    rp2040.ld
 * @brief   RP2040 链接脚本（适配最小内核体积约束）
 * @note    Flash: 2MB (0x10000000), RAM: 264KB (0x20000000)
 *          保留前 4KB 用于 Bootrom 二阶段加载器
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 264K
}

/* 栈/堆配置 */
__stack_size__ = 2K;
__heap_size__  = 4K;   /* 内核堆，由 kmalloc 管理 */

/* 入口点 */
ENTRY(_start)

SECTIONS
{
    /* ===== 二阶段引导加载器（RP2040 闪存启动必需，固定 256 字节）===== */
    .boot2 : {
        __boot2_start__ = .;
        KEEP(*(.boot2))
        . = ORIGIN(FLASH) + 256;
        __boot2_end__ = .;
    } > FLASH

    /* ===== 只读段 ===== */
    .text :
    {
        __logical_binary_start = .;
        KEEP(*(.vector_table))           /* 中断向量表 */
        *(.text*)                        /* 代码 */
        *(.rodata*)                      /* 只读数据 */
        *(.eh_frame*)                    /* 异常处理帧 */
        . = ALIGN(4);
    } > FLASH

    /* ===== 只读数据（Flash 中，运行时不拷贝）===== */
    .rodata :
    {
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ===== 初始化数据（Flash 中存初值，启动时拷贝到 RAM）===== */
    .data : AT (ADDR(.text) + SIZEOF(.text) + SIZEOF(.rodata))
    {
        __data_load_start__ = LOADADDR(.data);
        __etext             = LOADADDR(.data);  /* SDK 兼容别名 */
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM

    /* ===== BSS 段（零初始化）===== */
    .bss :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    /* ===== 栈（向下增长）===== */
    .stack (NOLOAD) :
    {
        __stack_top__ = ORIGIN(RAM) + LENGTH(RAM);
        __stack_bottom__ = __stack_top__ - __stack_size__;
        . = __stack_bottom__;
    } > RAM

    /* ===== 堆（向上增长，供 kmalloc 使用）===== */
    .heap (NOLOAD) :
    {
        __heap_start__ = ALIGN(8);
        __heap_end__ = __heap_start__ + __heap_size__;
        . = __heap_end__;
    } > RAM

    /* ===== 断言：体积红线检查（链接后由 CMake 解析 map 文件二次校验）===== */
    ASSERT((__heap_end__ - __bss_start__) <= 8192, "RAM 使用超标：最小内核 ≤ 4KB")
    ASSERT(SIZEOF(.text) + SIZEOF(.rodata) <= 10240, "Flash 使用超标：最小内核 ≤ 10KB")

    /* 导出符号供启动代码/内核使用 */
    PROVIDE(__flash_start__ = ORIGIN(FLASH));
    PROVIDE(__flash_end__   = ORIGIN(FLASH) + LENGTH(FLASH));
    PROVIDE(__ram_start__   = ORIGIN(RAM));
    PROVIDE(__ram_end__     = ORIGIN(RAM) + LENGTH(RAM));
}