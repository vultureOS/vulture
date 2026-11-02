/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The stub: it runs at its physical address. All of it is in the file, which ends at __image_end, on a page boundary,
 * and the payload follows it there. It fits below where the firmware may have loaded it.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x80000;
    __image_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    }

    /* Zeroed memory is in the file too: what follows the file in memory is the payload. */
    .data : ALIGN(16)
    {
        *(.data .data.*)
        *(.got .got.*)
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
        stub_translation_table = .;
        . += 4K;
        . += 64K;
        __stack_top = .;
        __bss_end = .;
        QUAD(0)
    }

    . = ALIGN(4K);
    __image_end = .;

    ASSERT(__image_end <= 0x200000, "the stub must end below where the firmware may load it")

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.rela*)
    }
}
