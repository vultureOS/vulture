/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibELF/Image.h>

namespace ELF {

// True if [offset, offset + count * element_size) lies within the file.
static bool range_fits(ReadonlyBytes file, u64 offset, u64 count, u64 element_size)
{
    u64 length = 0;
    u64 end = 0;
    if (__builtin_mul_overflow(count, element_size, &length) || __builtin_add_overflow(offset, length, &end))
        return false;
    return end <= file.size();
}

template<typename T>
static T const* at_offset(ReadonlyBytes file, u64 offset)
{
    return reinterpret_cast<T const*>(file.data() + offset);
}

ErrorOr<Image> Image::parse(ReadonlyBytes file)
{
    if (file.size() < sizeof(Elf64_Ehdr))
        return Error::from_string_literal("ELF: file too small");
    if (reinterpret_cast<FlatPtr>(file.data()) % alignof(Elf64_Ehdr) != 0)
        return Error::from_string_literal("ELF: file is not 8-byte aligned");

    Image image;
    image.m_file = file;
    image.m_header = at_offset<Elf64_Ehdr>(file, 0);
    auto const& header = *image.m_header;

    auto const& ident = header.e_ident;
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F')
        return Error::from_string_literal("ELF: bad magic");
    if (ident[EI_CLASS] != ELFCLASS64 || ident[EI_DATA] != ELFDATA2LSB || ident[EI_VERSION] != EV_CURRENT)
        return Error::from_string_literal("ELF: not a little-endian ELF64 file");

    if (header.e_phnum > 0) {
        if (header.e_phentsize != sizeof(Elf64_Phdr) || !range_fits(file, header.e_phoff, header.e_phnum, sizeof(Elf64_Phdr)) || header.e_phoff % 8 != 0)
            return Error::from_string_literal("ELF: bad program header table");
        image.m_program_headers = { at_offset<Elf64_Phdr>(file, header.e_phoff), header.e_phnum };
        for (auto const& segment : image.m_program_headers) {
            if (segment.p_type != PT_LOAD)
                continue;
            if (segment.p_filesz > segment.p_memsz || !range_fits(file, segment.p_offset, segment.p_filesz, 1))
                return Error::from_string_literal("ELF: segment outside the file");
        }
    }

    // Symbols are optional: a stripped file simply has none.
    if (header.e_shoff != 0 && header.e_shnum > 0) {
        if (header.e_shentsize != sizeof(Elf64_Shdr) || !range_fits(file, header.e_shoff, header.e_shnum, sizeof(Elf64_Shdr)) || header.e_shoff % 8 != 0)
            return Error::from_string_literal("ELF: bad section header table");
        Span<Elf64_Shdr const> sections { at_offset<Elf64_Shdr>(file, header.e_shoff), header.e_shnum };
        for (auto const& section : sections) {
            if (section.sh_type != SHT_SYMTAB)
                continue;
            if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_offset % 8 != 0 || !range_fits(file, section.sh_offset, section.sh_size, 1) || section.sh_link >= sections.size())
                return Error::from_string_literal("ELF: bad symbol table");
            auto const& strings = sections[section.sh_link];
            if (strings.sh_type != SHT_STRTAB || !range_fits(file, strings.sh_offset, strings.sh_size, 1))
                return Error::from_string_literal("ELF: bad symbol string table");
            image.m_symbols = { at_offset<Elf64_Sym>(file, section.sh_offset), section.sh_size / sizeof(Elf64_Sym) };
            image.m_string_table = file.slice(strings.sh_offset, strings.sh_size);
            break;
        }
    }

    return image;
}

ReadonlyBytes Image::segment_data(Elf64_Phdr const& segment) const
{
    return m_file.slice(segment.p_offset, segment.p_filesz);
}

StringView Image::symbol_name(Elf64_Sym const& symbol) const
{
    if (symbol.st_name >= m_string_table.size())
        return {};
    auto const* start = reinterpret_cast<char const*>(m_string_table.data() + symbol.st_name);
    size_t limit = m_string_table.size() - symbol.st_name;
    size_t length = 0;
    while (length < limit && start[length] != '\0')
        ++length;
    return { start, length };
}

Optional<Symbol> Image::find_symbol(u64 address) const
{
    Elf64_Sym const* containing = nullptr;
    Elf64_Sym const* preceding = nullptr;
    for (auto const& symbol : m_symbols) {
        u8 type = elf64_st_type(symbol.st_info);
        if ((type != STT_FUNC && type != STT_NOTYPE) || symbol.st_name == 0 || symbol.st_value > address)
            continue;
        if (type == STT_FUNC && address - symbol.st_value < symbol.st_size) {
            containing = &symbol;
            break;
        }
        if (!preceding || symbol.st_value > preceding->st_value)
            preceding = &symbol;
    }

    auto const* found = containing ? containing : preceding;
    if (!found)
        return {};
    return Symbol { symbol_name(*found), found->st_value, found->st_size };
}

Optional<u64> Image::find_symbol_address(StringView name) const
{
    for (auto const& symbol : m_symbols) {
        if (symbol.st_name != 0 && symbol_name(symbol) == name)
            return symbol.st_value;
    }
    return {};
}

}
