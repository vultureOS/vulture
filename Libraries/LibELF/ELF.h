/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// ELF64 definitions from the System V ABI. Names mirror the specification.

namespace ELF {

using Elf64_Addr = u64;
using Elf64_Off = u64;
using Elf64_Half = u16;
using Elf64_Word = u32;
using Elf64_Xword = u64;

constexpr size_t EI_NIDENT = 16;
constexpr size_t EI_CLASS = 4;
constexpr size_t EI_DATA = 5;
constexpr size_t EI_VERSION = 6;

constexpr u8 ELFCLASS64 = 2;
constexpr u8 ELFDATA2LSB = 1;
constexpr u8 EV_CURRENT = 1;

constexpr Elf64_Half ET_EXEC = 2;
constexpr Elf64_Half ET_DYN = 3;

constexpr Elf64_Half EM_X86_64 = 62;
constexpr Elf64_Half EM_AARCH64 = 183;
// What programs here are built for.
#if defined(__x86_64__)
constexpr Elf64_Half EM_NATIVE = EM_X86_64;
#else
constexpr Elf64_Half EM_NATIVE = EM_AARCH64;
#endif

constexpr Elf64_Word PT_LOAD = 1;
constexpr Elf64_Word PT_DYNAMIC = 2;
constexpr Elf64_Word PT_INTERP = 3;
constexpr Elf64_Word PT_PHDR = 6;
constexpr Elf64_Word PT_TLS = 7;
constexpr Elf64_Word PT_GNU_RELRO = 0x6474e552;

constexpr Elf64_Word PF_X = 1;
constexpr Elf64_Word PF_W = 2;
constexpr Elf64_Word PF_R = 4;

constexpr Elf64_Word SHT_SYMTAB = 2;
constexpr Elf64_Word SHT_STRTAB = 3;

constexpr u8 STT_NOTYPE = 0;
constexpr u8 STT_OBJECT = 1;
constexpr u8 STT_FUNC = 2;

constexpr u8 STT_TLS = 6;
constexpr u8 STT_GNU_IFUNC = 10;

constexpr u8 STB_LOCAL = 0;
constexpr u8 STB_GLOBAL = 1;
constexpr u8 STB_WEAK = 2;

constexpr Elf64_Half SHN_UNDEF = 0;

constexpr u8 elf64_st_type(u8 info) { return info & 0xf; }
constexpr u8 elf64_st_bind(u8 info) { return info >> 4; }

// The dynamic section's tags (those the dynamic linker reads).
constexpr i64 DT_NULL = 0;
constexpr i64 DT_NEEDED = 1;
constexpr i64 DT_PLTRELSZ = 2;
constexpr i64 DT_HASH = 4;
constexpr i64 DT_STRTAB = 5;
constexpr i64 DT_SYMTAB = 6;
constexpr i64 DT_RELA = 7;
constexpr i64 DT_RELASZ = 8;
constexpr i64 DT_RELAENT = 9;
constexpr i64 DT_STRSZ = 10;
constexpr i64 DT_SYMENT = 11;
constexpr i64 DT_INIT = 12;
constexpr i64 DT_FINI = 13;
constexpr i64 DT_SONAME = 14;
constexpr i64 DT_RUNPATH = 29;
constexpr i64 DT_JMPREL = 23;
constexpr i64 DT_INIT_ARRAY = 25;
constexpr i64 DT_FINI_ARRAY = 26;
constexpr i64 DT_INIT_ARRAYSZ = 27;
constexpr i64 DT_FINI_ARRAYSZ = 28;
constexpr i64 DT_FLAGS = 30;
constexpr i64 DT_TEXTREL = 22;

// AArch64 relocations.
constexpr u32 R_X86_64_NONE = 0;
constexpr u32 R_X86_64_64 = 1;
constexpr u32 R_X86_64_COPY = 5;
constexpr u32 R_X86_64_GLOB_DAT = 6;
constexpr u32 R_X86_64_JUMP_SLOT = 7;
constexpr u32 R_X86_64_RELATIVE = 8;
constexpr u32 R_X86_64_TPOFF64 = 18;
constexpr u32 R_AARCH64_NONE = 0;
constexpr u32 R_AARCH64_ABS64 = 257;
constexpr u32 R_AARCH64_COPY = 1024;
constexpr u32 R_AARCH64_GLOB_DAT = 1025;
constexpr u32 R_AARCH64_JUMP_SLOT = 1026;
constexpr u32 R_AARCH64_RELATIVE = 1027;
constexpr u32 R_AARCH64_TLS_TPREL64 = 1030;
constexpr u32 R_AARCH64_TLSDESC = 1031;
// The ones for what programs here are built for.
#if defined(__x86_64__)
constexpr u32 R_NATIVE_NONE = R_X86_64_NONE;
constexpr u32 R_NATIVE_ABS64 = R_X86_64_64;
constexpr u32 R_NATIVE_COPY = R_X86_64_COPY;
constexpr u32 R_NATIVE_GLOB_DAT = R_X86_64_GLOB_DAT;
constexpr u32 R_NATIVE_JUMP_SLOT = R_X86_64_JUMP_SLOT;
constexpr u32 R_NATIVE_RELATIVE = R_X86_64_RELATIVE;
constexpr u32 R_NATIVE_TPOFF = R_X86_64_TPOFF64;
#else
constexpr u32 R_NATIVE_NONE = R_AARCH64_NONE;
constexpr u32 R_NATIVE_ABS64 = R_AARCH64_ABS64;
constexpr u32 R_NATIVE_COPY = R_AARCH64_COPY;
constexpr u32 R_NATIVE_GLOB_DAT = R_AARCH64_GLOB_DAT;
constexpr u32 R_NATIVE_JUMP_SLOT = R_AARCH64_JUMP_SLOT;
constexpr u32 R_NATIVE_RELATIVE = R_AARCH64_RELATIVE;
constexpr u32 R_NATIVE_TPOFF = R_AARCH64_TLS_TPREL64;
#endif

constexpr u32 elf64_r_sym(u64 info) { return static_cast<u32>(info >> 32); }
constexpr u32 elf64_r_type(u64 info) { return static_cast<u32>(info & 0xffffffff); }

struct Elf64_Ehdr {
    u8 e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

struct Elf64_Shdr {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

struct Elf64_Sym {
    Elf64_Word st_name;
    u8 st_info;
    u8 st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

struct Elf64_Dyn {
    i64 d_tag;
    u64 d_val; // Or an address (d_ptr): the same bits.
};

struct Elf64_Rela {
    Elf64_Addr r_offset;
    u64 r_info;
    i64 r_addend;
};

static_assert(sizeof(Elf64_Ehdr) == 64);
static_assert(sizeof(Elf64_Phdr) == 56);
static_assert(sizeof(Elf64_Shdr) == 64);
static_assert(sizeof(Elf64_Sym) == 24);
static_assert(sizeof(Elf64_Dyn) == 16);
static_assert(sizeof(Elf64_Rela) == 24);

}
