/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/StringView.h>

// Reading DER (X.690): what X.509 is written in. Everything is checked against the bytes there
// really are, as all of this comes from strangers.

namespace TLS::ASN1 {

constexpr u8 tag_boolean = 0x01;
constexpr u8 tag_integer = 0x02;
constexpr u8 tag_bit_string = 0x03;
constexpr u8 tag_octet_string = 0x04;
constexpr u8 tag_null = 0x05;
constexpr u8 tag_object = 0x06;
constexpr u8 tag_utf8_string = 0x0c;
constexpr u8 tag_printable_string = 0x13;
constexpr u8 tag_ia5_string = 0x16;
constexpr u8 tag_utc_time = 0x17;
constexpr u8 tag_generalized_time = 0x18;
constexpr u8 tag_sequence = 0x30;
constexpr u8 tag_set = 0x31;

constexpr u8 context_tag(u8 number, bool constructed)
{
    return static_cast<u8>(0x80 | (constructed ? 0x20 : 0) | number);
}

struct Element {
    u8 tag { 0 };
    ReadonlyBytes contents;
    // With its tag and length.
    ReadonlyBytes whole;
};

class Reader {
public:
    Reader() = default;
    explicit Reader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    [[nodiscard]] bool at_end() const { return m_at >= m_bytes.size(); }
    [[nodiscard]] ErrorOr<Element> read();
    // Fails on any other tag.
    [[nodiscard]] ErrorOr<Element> read_expecting(u8 tag);
    // What the next element's tag is, or 0 at the end.
    [[nodiscard]] u8 next_tag() const { return at_end() ? 0 : m_bytes[m_at]; }

private:
    ReadonlyBytes m_bytes;
    size_t m_at { 0 };
};

// Is this object identifier the one written like "1.2.840.113549.1.1.11"?
[[nodiscard]] bool object_is(ReadonlyBytes encoded, StringView dotted);
// UTCTime or GeneralizedTime, as seconds since 1970.
[[nodiscard]] ErrorOr<i64> parse_time(Element const&);

}
