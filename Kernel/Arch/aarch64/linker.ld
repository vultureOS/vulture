/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The kernel runs in the top 2 GiB of the address space. Each permission
 * class gets its own page-aligned segment, so VultureBoot can map code
 * read-execute, constants read-only and data read-write (never executable).
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* read, execute */
    rodata PT_LOAD FLAGS(4); /* read */
    data PT_LOAD FLAGS(6);   /* read, write */
}

KERNEL_VIRTUAL_BASE = 0xffffffff80000000;

SECTIONS
{
    . = KERNEL_VIRTUAL_BASE;
    __kernel_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.text.boot))
        *(.text.vectors)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    } :rodata

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    } :data

    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
