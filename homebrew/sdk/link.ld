/* Leapster homebrew: code in cartridge ROM, data/bss in main RAM. */
ENTRY(_start)
MEMORY {
  ROM (rx)  : ORIGIN = 0x80001000, LENGTH = 0x00EFF000
  RAM (rwx) : ORIGIN = 0x3C000200, LENGTH = 0x00100000
}
SECTIONS {
  .text : { KEEP(*(.text.start)) *(.text .text.*) } > ROM
  .rodata : { *(.rodata .rodata.*) } > ROM
  .data : { __data_start = .; *(.data .data.* .sdata .sdata.*) __data_end = .; } > RAM AT > ROM
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) __bss_end = .; } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.comment) *(.ARC.attributes) }
}
