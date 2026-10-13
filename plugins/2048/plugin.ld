MEMORY
{
    APP_RAM (rwx) : ORIGIN = 0x20038000, LENGTH = 32K
}

SECTIONS
{
    .entry : ALIGN(4) { KEEP(*(.entry)) } > APP_RAM
    .text : ALIGN(4) { *(.text*) *(.rodata*) } > APP_RAM
    .data : ALIGN(4) { *(.data*) } > APP_RAM
    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > APP_RAM
    /DISCARD/ : { *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
}
