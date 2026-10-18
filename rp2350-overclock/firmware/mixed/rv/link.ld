OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    PAYLOAD (rwx) : ORIGIN = 0x20060000, LENGTH = 16K
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
    } > PAYLOAD

    .data : { *(.data*) *(.sdata*) } > PAYLOAD
    .bss  : { *(.bss*) *(.sbss*) *(COMMON) } > PAYLOAD
    _end = .;
}
