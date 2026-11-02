/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibTLS/ASN1.h>

namespace TLS::ASN1 {

ErrorOr<Element> Reader::read()
{
    if (m_at + 2 > m_bytes.size())
        return Error::from_string_literal("DER: truncated");
    size_t const start = m_at;
    u8 const tag = m_bytes[m_at++];
    // High-tag-number form isn't used in anything here.
    if ((tag & 0x1f) == 0x1f)
        return Error::from_string_literal("DER: long tag");
    u8 const first = m_bytes[m_at++];
    size_t length = first;
    if (first & 0x80) {
        size_t const count = first & 0x7f;
        if (count == 0 || count > 4 || m_at + count > m_bytes.size())
            return Error::from_string_literal("DER: bad length");
        length = 0;
        for (size_t i = 0; i < count; ++i)
            length = length << 8 | m_bytes[m_at++];
        // The shortest form, as DER requires.
        if (length < 0x80 || (count > 1 && (length >> ((count - 1) * 8)) == 0))
            return Error::from_string_literal("DER: length not minimal");
    }
    if (length > m_bytes.size() - m_at)
        return Error::from_string_literal("DER: longer than what is there");
    Element element;
    element.tag = tag;
    element.contents = { m_bytes.data() + m_at, length };
    element.whole = { m_bytes.data() + start, m_at + length - start };
    m_at += length;
    return element;
}

ErrorOr<Element> Reader::read_expecting(u8 tag)
{
    auto element = TRY(read());
    if (element.tag != tag)
        return Error::from_string_literal("DER: unexpected tag");
    return element;
}

bool object_is(ReadonlyBytes encoded, StringView dotted)
{
    // Decode it and compare in text, which keeps all the table of names readable.
    char decoded[96];
    size_t written = 0;
    u64 value = 0;
    bool first = true;
    auto put_number = [&](u64 number) {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + number % 10);
            number /= 10;
        } while (number > 0);
        while (count > 0 && written < sizeof(decoded) - 1)
            decoded[written++] = digits[--count];
    };
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (value >> 57)
            return false;
        value = value << 7 | (encoded[i] & 0x7f);
        if (encoded[i] & 0x80)
            continue;
        if (first) {
            u64 const top = value < 40 ? 0 : (value < 80 ? 1 : 2);
            put_number(top);
            if (written < sizeof(decoded) - 1)
                decoded[written++] = '.';
            put_number(value - top * 40);
            first = false;
        } else {
            if (written < sizeof(decoded) - 1)
                decoded[written++] = '.';
            put_number(value);
        }
        value = 0;
    }
    return StringView { decoded, written } == dotted;
}

namespace {

// Days from 1970-01-01 to a date, by Howard Hinnant's method.
i64 days_from_civil(i64 year, u32 month, u32 day)
{
    year -= month <= 2 ? 1 : 0;
    i64 const era = (year >= 0 ? year : year - 399) / 400;
    u32 const year_of_era = static_cast<u32>(year - era * 400);
    u32 const day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    u32 const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<i64>(day_of_era) - 719468;
}

}

ErrorOr<i64> parse_time(Element const& element)
{
    auto const& text = element.contents;
    size_t digits;
    if (element.tag == tag_utc_time)
        digits = 12;
    else if (element.tag == tag_generalized_time)
        digits = 14;
    else
        return Error::from_string_literal("certificate: not a time");
    if (text.size() != digits + 1 || text[digits] != 'Z')
        return Error::from_string_literal("certificate: a time not in UTC");
    int values[7];
    for (size_t i = 0; i < digits; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return Error::from_string_literal("certificate: not a time");
    }
    auto two = [&](size_t at) { return (text[at] - '0') * 10 + (text[at + 1] - '0'); };
    size_t at = 0;
    i64 year;
    if (digits == 12) {
        year = two(0);
        // RFC 5280: 00-49 are 20xx, and 50-99 are 19xx.
        year += year < 50 ? 2000 : 1900;
        at = 2;
    } else {
        year = two(0) * 100 + two(2);
        at = 4;
    }
    for (int i = 0; i < 5; ++i, at += 2)
        values[i] = two(at);
    int const month = values[0], day = values[1], hour = values[2], minute = values[3], second = values[4];
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
        return Error::from_string_literal("certificate: not a time");
    return days_from_civil(year, static_cast<u32>(month), static_cast<u32>(day)) * 86400 + hour * 3600 + minute * 60 + second;
}

}
