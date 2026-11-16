/* QEMU Stellaris LM3S6965EVB: flash at 0, SRAM at 0x20000000.
   Deliberately no CRT, OS, libc, malloc, or general runtime library. */
ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text .text.*) *(.rodata .rodata.*) } > FLASH
  .data : { *(.data .data.*) } > RAM AT > FLASH
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > RAM
  /* No supported foreign unwinder: target trap above is fail-stop only. */
  /DISCARD/ : { *(.eh_frame*) *(.ARM.extab*) *(.ARM.exidx*) }
}
