/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The kernel runs in the top 2 GiB of the address space. Each permission
 * class gets its own page-aligned segment, so VultureBoot can map code
 * read-execute, constants read-only and data read-write (never executable).
 *
 * It is position independent. VultureBoot picks where it really goes, somewhere
 * above this address, and applies the relative relocations listed in .rela.dyn.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* read, execute */
    rodata PT_LOAD FLAGS(4); /* read */
    data PT_LOAD FLAGS(6);   /* read, write */
    dynamic PT_DYNAMIC FLAGS(4);
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
        __rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata

    /* Recovery addresses for faults in user memory access (UserAccess.S). */
    __ex_table : ALIGN(8)
    {
        __ex_table_start = .;
        KEEP(*(__ex_table))
        __ex_table_end = .;
    } :rodata

    /* What the loader reads to relocate the kernel. */
    .dynsym : { *(.dynsym) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .hash : { *(.hash) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .rela.dyn : { *(.rela.dyn) *(.rela.*) } :rodata

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata

    .dynamic : ALIGN(4K) { __data_start = .; *(.dynamic) } :data :dynamic
    .got : { *(.got .got.*) } :data
    .got.plt : { *(.got.plt) } :data

    .data : ALIGN(8)
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
