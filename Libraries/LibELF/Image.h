/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibELF/ELF.h>

namespace ELF {

struct Symbol {
    StringView name;
    u64 address { 0 };
    u64 size { 0 };
};

// A validated view of an ELF64 little-endian file held in memory. Every
// header, segment and table it hands out has been bounds-checked against
// the file. The file must stay alive and 8-byte aligned.
class Image {
public:
    [[nodiscard]] static ErrorOr<Image> parse(ReadonlyBytes file);

    [[nodiscard]] Elf64_Ehdr const& header() const { return *m_header; }
    [[nodiscard]] Span<Elf64_Phdr const> program_headers() const { return m_program_headers; }

    // The bytes a segment takes from the file (p_filesz of them).
    [[nodiscard]] ReadonlyBytes segment_data(Elf64_Phdr const&) const;

    [[nodiscard]] bool has_symbols() const { return !m_symbols.is_empty(); }

    // The function (or, failing that, the nearest preceding symbol) that
    // contains `address`.
    [[nodiscard]] Optional<Symbol> find_symbol(u64 address) const;

    // The address of a symbol by exact name.
    [[nodiscard]] Optional<u64> find_symbol_address(StringView name) const;

private:
    Image() = default;

    [[nodiscard]] StringView symbol_name(Elf64_Sym const&) const;

    ReadonlyBytes m_file;
    Elf64_Ehdr const* m_header { nullptr };
    Span<Elf64_Phdr const> m_program_headers;
    Span<Elf64_Sym const> m_symbols;
    ReadonlyBytes m_string_table;
};

}
