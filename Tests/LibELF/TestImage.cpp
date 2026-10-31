/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibELF/Image.h>
#include <LibTest/TestCase.h>

namespace {

constexpr u64 text_base = 0xffffffff80001000;

// A small but complete executable: one PT_LOAD segment plus a symbol table.
struct TestFile {
    alignas(8) u8 bytes[1024] {};

    TestFile()
    {
        using namespace ELF;

        Elf64_Ehdr header {};
        header.e_ident[0] = 0x7f;
        header.e_ident[1] = 'E';
        header.e_ident[2] = 'L';
        header.e_ident[3] = 'F';
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_type = ET_EXEC;
        header.e_machine = EM_AARCH64;
        header.e_version = EV_CURRENT;
        header.e_entry = text_base;
        header.e_phoff = 64;
        header.e_shoff = 0x200;
        header.e_ehsize = sizeof(Elf64_Ehdr);
        header.e_phentsize = sizeof(Elf64_Phdr);
        header.e_phnum = 1;
        header.e_shentsize = sizeof(Elf64_Shdr);
        header.e_shnum = 3;
        put(0, header);

        Elf64_Phdr segment {};
        segment.p_type = PT_LOAD;
        segment.p_flags = PF_R | PF_X;
        segment.p_offset = 0x100;
        segment.p_vaddr = text_base;
        segment.p_paddr = 0;
        segment.p_filesz = 16;
        segment.p_memsz = 32;
        segment.p_align = 4096;
        put(64, segment);
        for (u8 i = 0; i < 16; ++i)
            bytes[0x100 + i] = i;

        char const strings[] = "\0alpha\0beta\0_start";
        __builtin_memcpy(bytes + 0x180, strings, sizeof(strings));

        put(0x1a0 + 1 * sizeof(Elf64_Sym), symbol(1, STT_FUNC, text_base, 0x20));
        put(0x1a0 + 2 * sizeof(Elf64_Sym), symbol(7, STT_FUNC, text_base + 0x20, 0x10));
        put(0x1a0 + 3 * sizeof(Elf64_Sym), symbol(12, STT_NOTYPE, text_base + 0x1000, 0));

        Elf64_Shdr symbol_table {};
        symbol_table.sh_type = SHT_SYMTAB;
        symbol_table.sh_offset = 0x1a0;
        symbol_table.sh_size = 4 * sizeof(Elf64_Sym);
        symbol_table.sh_link = 2;
        symbol_table.sh_entsize = sizeof(Elf64_Sym);
        put(0x200 + 1 * sizeof(Elf64_Shdr), symbol_table);

        Elf64_Shdr string_table {};
        string_table.sh_type = SHT_STRTAB;
        string_table.sh_offset = 0x180;
        string_table.sh_size = sizeof(strings);
        put(0x200 + 2 * sizeof(Elf64_Shdr), string_table);
    }

    static ELF::Elf64_Sym symbol(u32 name, u8 type, u64 value, u64 size)
    {
        ELF::Elf64_Sym result {};
        result.st_name = name;
        result.st_info = type;
        result.st_value = value;
        result.st_size = size;
        return result;
    }

    template<typename T>
    void put(size_t offset, T const& value)
    {
        __builtin_memcpy(bytes + offset, &value, sizeof(T));
    }

    ReadonlyBytes span() const { return { bytes, sizeof(bytes) }; }
};

}

TEST_CASE(parses_headers_and_segments)
{
    TestFile file;
    auto image = ELF::Image::parse(file.span());
    EXPECT(!image.is_error());
    EXPECT_EQ(image.value().header().e_machine, ELF::EM_AARCH64);
    EXPECT_EQ(image.value().header().e_entry, text_base);
    EXPECT_EQ(image.value().program_headers().size(), 1u);

    auto const& segment = image.value().program_headers()[0];
    auto data = image.value().segment_data(segment);
    EXPECT_EQ(data.size(), 16u);
    EXPECT_EQ(data[15], 15);
}

TEST_CASE(finds_symbols)
{
    TestFile file;
    auto image = MUST(ELF::Image::parse(file.span()));
    EXPECT(image.has_symbols());

    auto alpha = image.find_symbol(text_base + 0x10);
    EXPECT(alpha.has_value());
    EXPECT_EQ(alpha->name, "alpha"_sv);
    EXPECT_EQ(alpha->address, text_base);

    auto beta = image.find_symbol(text_base + 0x25);
    EXPECT(beta.has_value());
    EXPECT_EQ(beta->name, "beta"_sv);

    // Beyond every sized function: the nearest preceding symbol wins.
    auto start = image.find_symbol(text_base + 0x1004);
    EXPECT(start.has_value());
    EXPECT_EQ(start->name, "_start"_sv);

    EXPECT(!image.find_symbol(0x1000).has_value());
    EXPECT_EQ(image.find_symbol_address("beta"_sv), text_base + 0x20);
    EXPECT(!image.find_symbol_address("gamma"_sv).has_value());
}

TEST_CASE(rejects_malformed_files)
{
    TestFile truncated;
    EXPECT(ELF::Image::parse(truncated.span().trim(32)).is_error());

    TestFile bad_magic;
    bad_magic.bytes[1] = 'X';
    EXPECT(ELF::Image::parse(bad_magic.span()).is_error());

    TestFile segment_outside;
    ELF::Elf64_Phdr segment {};
    __builtin_memcpy(&segment, segment_outside.bytes + 64, sizeof(segment));
    segment.p_filesz = 4096;
    segment_outside.put(64, segment);
    EXPECT(ELF::Image::parse(segment_outside.span()).is_error());

    TestFile bad_symbol_link;
    ELF::Elf64_Shdr symbol_table {};
    __builtin_memcpy(&symbol_table, bad_symbol_link.bytes + 0x200 + sizeof(ELF::Elf64_Shdr), sizeof(symbol_table));
    symbol_table.sh_link = 9;
    bad_symbol_link.put(0x200 + sizeof(ELF::Elf64_Shdr), symbol_table);
    EXPECT(ELF::Image::parse(bad_symbol_link.span()).is_error());
}
