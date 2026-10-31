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

constexpr Elf64_Word PT_LOAD = 1;

constexpr Elf64_Word PF_X = 1;
constexpr Elf64_Word PF_W = 2;
constexpr Elf64_Word PF_R = 4;

constexpr Elf64_Word SHT_SYMTAB = 2;
constexpr Elf64_Word SHT_STRTAB = 3;

constexpr u8 STT_NOTYPE = 0;
constexpr u8 STT_OBJECT = 1;
constexpr u8 STT_FUNC = 2;

constexpr u8 elf64_st_type(u8 info) { return info & 0xf; }

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

static_assert(sizeof(Elf64_Ehdr) == 64);
static_assert(sizeof(Elf64_Phdr) == 56);
static_assert(sizeof(Elf64_Shdr) == 64);
static_assert(sizeof(Elf64_Sym) == 24);

}
