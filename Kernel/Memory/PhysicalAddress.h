/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>

namespace Kernel {

constexpr size_t page_size = 4 * KiB;
constexpr unsigned page_shift = 12;

// A physical memory address. A distinct type, so physical and virtual
// addresses cannot be mixed up silently.
class PhysicalAddress {
public:
    constexpr PhysicalAddress() = default;
    constexpr explicit PhysicalAddress(u64 address)
        : m_address(address)
    {
    }

    [[nodiscard]] constexpr u64 get() const { return m_address; }
    [[nodiscard]] constexpr PhysicalAddress offset(u64 bytes) const { return PhysicalAddress(m_address + bytes); }
    [[nodiscard]] constexpr u64 page_frame() const { return m_address >> page_shift; }
    [[nodiscard]] constexpr bool is_page_aligned() const { return m_address % page_size == 0; }

    constexpr bool operator==(PhysicalAddress const&) const = default;
    constexpr bool operator<(PhysicalAddress other) const { return m_address < other.m_address; }
    constexpr bool operator<=(PhysicalAddress other) const { return m_address <= other.m_address; }

private:
    u64 m_address { 0 };
};

}

template<>
struct Base::Formatter<Kernel::PhysicalAddress> {
    // Hexadecimal unless the format says otherwise.
    static void format(FormatSink& sink, FormatSpec const& spec, Kernel::PhysicalAddress address)
    {
        FormatSpec hex = spec;
        if (hex.type == 0) {
            hex.type = 'x';
            hex.alternate_form = true;
        }
        format_integer(sink, hex, address.get(), false);
    }
};
