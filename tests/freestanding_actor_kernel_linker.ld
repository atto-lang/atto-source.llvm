ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
  RAM (rw) : ORIGIN = 0x20000000, LENGTH = 64K
}
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .data : ALIGN(4) {
    __data_start = .;
    *(.data*)
    . = ALIGN(4);
    __data_end = .;
  } > RAM AT > FLASH
  __data_load_start = LOADADDR(.data);
  .bss (NOLOAD) : ALIGN(4) {
    __bss_start = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM
  ASSERT(__bss_end < __stack_top - 4096, "Cortex-M reserved stack would overlap BSS")
}
