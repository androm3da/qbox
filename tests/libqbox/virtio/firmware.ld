/*
 * Linker script for VirtIO guest firmware
 */

ENTRY(_start)

SECTIONS
{
    . = 0x0;

    .text : {
        *(.text)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
    }
}
