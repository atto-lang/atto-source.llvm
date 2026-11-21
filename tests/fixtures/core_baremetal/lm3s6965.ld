/* QEMU lm3s6965evb (Cortex-M3): 256 KiB flash at 0x0, 64 KiB SRAM at 0x20000000.
 * No libc, no heap region, no TLS, no constructors: the image has a fixed
 * stack and nothing else. */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

__stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : { *(.text .text.*) *(.rodata .rodata.*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx .ARM.exidx.*) } > FLASH

    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > SRAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SRAM

    /* Constructors and TLS are deliberately not placed or discarded here:
     * if an input ever carries them they land as orphan sections and the
     * gate rejects the image instead of silently hiding them. */
}
