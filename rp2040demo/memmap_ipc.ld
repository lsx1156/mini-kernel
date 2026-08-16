/* memmap_ipc.ld — v2.6.1 双核 IPC 内存映射（基于 SDK memmap_default.ld 修改）
 *
 * 【v2.6.1 关键修正：放弃 bank 别名区】
 *   v2.6 把 IPC 区放在非条带别名（0x2101xxxx），基于"条带区只占每 bank
 *   前 32KB"的推算。实测该推算不成立：写 0x21018000（ctrl 清零）直接
 *   打爆内核 .data（hal_systick_ops.delay_us 被清零 → idle blx 0 →
 *   HardFault PC=0）。别名区与条带区的实际物理映射以硅片为准，不再假设。
 *
 *   改为 linker 亲自裁剪的常规条带地址：RAM 缩到 96k（内核现用 ~26KB，
 *   余量充足），0x20018000 起的 IPC 区不属于任何可分配 region →
 *   构造上保证内核不可能踩到：
 *     0x20018000 - 0x20018FFF  IPC ctrl   4KB（无 region，裸地址）
 *     0x20019000 - 0x2001FFFF  CORE1     28KB（镜像 + 顶 8KB 栈）
 *     0x20020000 - 0x2002FFFF  IPCBUF    64KB（IN-A/B + OUT-A/B 各 16KB）
 *
 * 其余改动：
 *   .core1_text/.core1_data（LMA 在 Flash，VMA 在 CORE1，由 ipc_start()
 *   手动拷贝镜像）与 .core1_bss（NOLOAD，手动清零）。
 */

MEMORY
{
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 2048k
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 96k
    SCRATCH_X(rwx) : ORIGIN = 0x20040000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20041000, LENGTH = 4k
    CORE1(rwx) : ORIGIN = 0x20019000, LENGTH = 28k
    IPCBUF(rwx) : ORIGIN = 0x20020000, LENGTH = 64k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    .boot2 : {
        __boot2_start__ = .;
        KEEP (*(.boot2))
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ == 256,
        "ERROR: Pico second stage bootloader must be 256 bytes in size")

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.reset))
        *(.init)
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

   .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.jcr)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM AT> FLASH
    __etext = LOADADDR(.data);

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* ================================================================
     * v2.6 Core1 镜像段：LMA 在 Flash（随固件烧录），VMA 在条带 RAM
     * CORE1 区。ipc_start() 用下面的符号手动 memcpy 过去再拉起 Core1。
     * 注意：crt0 只搬 .data，这几段必须手工搬运 / 清零。
     * ================================================================ */
    .core1_text : {
        __core1_start__ = .;
        KEEP(*(.core1_text*))
        . = ALIGN(4);
    } > CORE1 AT > FLASH
    __core1_load_start__ = LOADADDR(.core1_text);

    .core1_data : {
        *(.core1_data*)
        . = ALIGN(4);
        __core1_end__ = .;
    } > CORE1 AT > FLASH
    __core1_load_end__ = LOADADDR(.core1_data) + SIZEOF(.core1_data);

    .core1_bss (NOLOAD) : {
        __core1_bss_start__ = .;
        *(.core1_bss*)
        . = ALIGN(4);
        __core1_bss_end__ = .;
    } > CORE1
    __core1_stack_top = ORIGIN(CORE1) + LENGTH(CORE1);

    /* 顶部 8KB+256B 预留给 Core1 栈（multicore_launch_core1_with_stack 用） */
    ASSERT(__core1_bss_end__ <= ORIGIN(CORE1) + LENGTH(CORE1) - 0x2100,
        "ERROR: CORE1 bss/data overlaps core1 stack reserve (top 8.25KB)")

    .bss  : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
        __HeapLimit = .;
    } > RAM

    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    .flash_end : {
        PROVIDE(__flash_binary_end = .);
    } > FLASH

    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 256, "Binary info must be in first 256 bytes of the binary")
}
