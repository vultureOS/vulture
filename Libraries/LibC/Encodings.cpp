/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Encodings.h"
#include "Lock.h"
#include <stdlib.h>

namespace LibC::Encodings {

namespace {

// From a character back to where it is in a table: worked out the first time something's written in that encoding, as most programs never do.
struct Reverse {
    struct Entry {
        u32 code;
        u32 pointer;
    };
    Entry* entries { nullptr };
    size_t count { 0 };
};

enum class TableKind : u8 {
    JIS0208,
    JIS0212,
    EUCKR,
    GB18030,
    Big5,
    Count,
};

Reverse s_reverse[static_cast<size_t>(TableKind::Count)];
LibC::Lock s_reverse_lock;

u32 value_at(TableKind kind, size_t pointer)
{
    switch (kind) {
    case TableKind::JIS0208:
        return pointer < jis0208_count ? jis0208[pointer] : 0;
    case TableKind::JIS0212:
        return pointer < jis0212_count ? jis0212[pointer] : 0;
    case TableKind::EUCKR:
        return pointer < euc_kr_count ? euc_kr[pointer] : 0;
    case TableKind::GB18030:
        return pointer < gb18030_count ? gb18030[pointer] : 0;
    case TableKind::Big5:
        return pointer < big5_count ? big5[pointer] : 0;
    default:
        return 0;
    }
}

size_t size_of(TableKind kind)
{
    switch (kind) {
    case TableKind::JIS0208:
        return jis0208_count;
    case TableKind::JIS0212:
        return jis0212_count;
    case TableKind::EUCKR:
        return euc_kr_count;
    case TableKind::GB18030:
        return gb18030_count;
    case TableKind::Big5:
        return big5_count;
    default:
        return 0;
    }
}

int compare_entries(void const* a, void const* b)
{
    auto const& first = *static_cast<Reverse::Entry const*>(a);
    auto const& second = *static_cast<Reverse::Entry const*>(b);
    if (first.code != second.code)
        return first.code < second.code ? -1 : 1;
    if (first.pointer != second.pointer)
        return first.pointer < second.pointer ? -1 : 1;
    return 0;
}

Reverse const& reverse_of(TableKind kind)
{
    LibC::LockGuard guard(s_reverse_lock);
    auto& reverse = s_reverse[static_cast<size_t>(kind)];
    if (reverse.entries)
        return reverse;
    size_t const total = size_of(kind);
    auto* entries = static_cast<Reverse::Entry*>(malloc(total * sizeof(Reverse::Entry)));
    if (!entries)
        return reverse;
    size_t count = 0;
    for (size_t pointer = 0; pointer < total; ++pointer) {
        u32 const code = value_at(kind, pointer);
        if (code)
            entries[count++] = { code, static_cast<u32>(pointer) };
    }
    qsort(entries, count, sizeof(Reverse::Entry), compare_entries);
    reverse.entries = entries;
    reverse.count = count;
    return reverse;
}

// The first place it is, in the table's order. The second number's what's wanted in a few of them, for Big5: see below.
bool find_pointer(TableKind kind, u32 code, u32& pointer, u32& last_pointer)
{
    auto const& reverse = reverse_of(kind);
    size_t low = 0;
    size_t high = reverse.count;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        if (reverse.entries[middle].code < code)
            low = middle + 1;
        else
            high = middle;
    }
    if (low >= reverse.count || reverse.entries[low].code != code)
        return false;
    pointer = reverse.entries[low].pointer;
    last_pointer = pointer;
    for (size_t i = low + 1; i < reverse.count && reverse.entries[i].code == code; ++i)
        last_pointer = reverse.entries[i].pointer;
    return true;
}

int decode_shift_jis(unsigned char const* in, size_t left, unsigned& code)
{
    if (left < 1)
        return 0;
    unsigned char const lead = in[0];
    if (lead < 0x80) {
        code = lead;
        return 1;
    }
    if (lead >= 0xa1 && lead <= 0xdf) {
        code = 0xff61 + (lead - 0xa1u);
        return 1;
    }
    if (!((lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xfc)))
        return -1;
    if (left < 2)
        return 0;
    unsigned char const trail = in[1];
    if (!((trail >= 0x40 && trail <= 0x7e) || (trail >= 0x80 && trail <= 0xfc)))
        return -1;
    unsigned const lead_offset = lead < 0xa0 ? 0x81 : 0xc1;
    unsigned const trail_offset = trail < 0x7f ? 0x40 : 0x41;
    size_t const pointer = (lead - lead_offset) * 188u + trail - trail_offset;
    // The private use area, at the end.
    if (pointer >= 8836 && pointer <= 10715) {
        code = 0xe000 + static_cast<unsigned>(pointer - 8836);
        return 2;
    }
    code = value_at(TableKind::JIS0208, pointer);
    return code ? 2 : -1;
}

int encode_shift_jis(unsigned code, unsigned char* out, size_t left)
{
    if (code < 0x80) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    }
    if (code >= 0xff61 && code <= 0xff9f) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code - 0xff61 + 0xa1);
        return 1;
    }
    u32 pointer, last;
    if (!find_pointer(TableKind::JIS0208, code, pointer, last))
        return -1;
    // Some are there twice, and the ones past here are extensions that Shift_JIS has a better place for.
    if (pointer >= 8272 && pointer <= 8835 && last > 8835)
        pointer = last;
    if (left < 2)
        return 0;
    u32 const lead = pointer / 188;
    u32 const trail = pointer % 188;
    out[0] = static_cast<unsigned char>(lead + (lead < 0x1f ? 0x81 : 0xc1));
    out[1] = static_cast<unsigned char>(trail + (trail < 0x3f ? 0x40 : 0x41));
    return 2;
}

int decode_euc_jp(unsigned char const* in, size_t left, unsigned& code)
{
    if (left < 1)
        return 0;
    unsigned char const lead = in[0];
    if (lead < 0x80) {
        code = lead;
        return 1;
    }
    if (left < 2)
        return 0;
    if (lead == 0x8e) {
        if (in[1] < 0xa1 || in[1] > 0xdf)
            return -1;
        code = 0xff61 + (in[1] - 0xa1u);
        return 2;
    }
    if (lead == 0x8f) {
        if (left < 3)
            return 0;
        if (in[1] < 0xa1 || in[1] > 0xfe || in[2] < 0xa1 || in[2] > 0xfe)
            return -1;
        code = value_at(TableKind::JIS0212, (in[1] - 0xa1u) * 94 + (in[2] - 0xa1u));
        return code ? 3 : -1;
    }
    if (lead < 0xa1 || lead > 0xfe || in[1] < 0xa1 || in[1] > 0xfe)
        return -1;
    code = value_at(TableKind::JIS0208, (lead - 0xa1u) * 94 + (in[1] - 0xa1u));
    return code ? 2 : -1;
}

int encode_euc_jp(unsigned code, unsigned char* out, size_t left)
{
    if (code < 0x80) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    }
    if (code >= 0xff61 && code <= 0xff9f) {
        if (left < 2)
            return 0;
        out[0] = 0x8e;
        out[1] = static_cast<unsigned char>(code - 0xff61 + 0xa1);
        return 2;
    }
    u32 pointer, last;
    if (!find_pointer(TableKind::JIS0208, code, pointer, last))
        return -1;
    if (left < 2)
        return 0;
    out[0] = static_cast<unsigned char>(pointer / 94 + 0xa1);
    out[1] = static_cast<unsigned char>(pointer % 94 + 0xa1);
    return 2;
}

int decode_euc_kr(unsigned char const* in, size_t left, unsigned& code)
{
    if (left < 1)
        return 0;
    if (in[0] < 0x80) {
        code = in[0];
        return 1;
    }
    if (in[0] < 0x81 || in[0] > 0xfe)
        return -1;
    if (left < 2)
        return 0;
    if (in[1] < 0x41 || in[1] > 0xfe)
        return -1;
    code = value_at(TableKind::EUCKR, (in[0] - 0x81u) * 190 + (in[1] - 0x41u));
    return code ? 2 : -1;
}

int encode_euc_kr(unsigned code, unsigned char* out, size_t left)
{
    if (code < 0x80) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    }
    u32 pointer, last;
    if (!find_pointer(TableKind::EUCKR, code, pointer, last))
        return -1;
    if (left < 2)
        return 0;
    out[0] = static_cast<unsigned char>(pointer / 190 + 0x81);
    out[1] = static_cast<unsigned char>(pointer % 190 + 0x41);
    return 2;
}

// What the ranges say a four-byte pointer stands for, which is everything the two-byte table doesn't have, in order.
u32 gb18030_range_code(u32 pointer)
{
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575)
        return 0;
    if (pointer == 7457)
        return 0xe7c7;
    u32 offset = 0;
    u32 first = 0;
    for (size_t i = 0; i + 1 < gb18030_ranges_count; i += 2) {
        if (gb18030_ranges[i] > pointer)
            break;
        offset = gb18030_ranges[i];
        first = gb18030_ranges[i + 1];
    }
    return first + pointer - offset;
}

u32 gb18030_range_pointer(u32 code)
{
    if (code == 0xe7c7)
        return 7457;
    u32 offset = 0;
    u32 first = 0;
    for (size_t i = 0; i + 1 < gb18030_ranges_count; i += 2) {
        if (gb18030_ranges[i + 1] > code)
            break;
        offset = gb18030_ranges[i];
        first = gb18030_ranges[i + 1];
    }
    return offset + code - first;
}

int decode_gb(unsigned char const* in, size_t left, unsigned& code, bool four_bytes)
{
    if (left < 1)
        return 0;
    if (in[0] < 0x80) {
        code = in[0];
        return 1;
    }
    if (in[0] == 0x80 || in[0] == 0xff)
        return -1;
    if (left < 2)
        return 0;
    if (in[1] >= 0x30 && in[1] <= 0x39) {
        if (!four_bytes)
            return -1;
        if (left < 4)
            return 0;
        if (in[2] < 0x81 || in[2] > 0xfe || in[3] < 0x30 || in[3] > 0x39)
            return -1;
        u32 const pointer = (in[0] - 0x81u) * 12600 + (in[1] - 0x30u) * 1260 + (in[2] - 0x81u) * 10 + (in[3] - 0x30u);
        code = gb18030_range_code(pointer);
        return code ? 4 : -1;
    }
    if (in[1] < 0x40 || in[1] == 0x7f || in[1] == 0xff)
        return -1;
    unsigned const trail_offset = in[1] < 0x7f ? 0x40 : 0x41;
    code = value_at(TableKind::GB18030, (in[0] - 0x81u) * 190 + (in[1] - trail_offset));
    return code ? 2 : -1;
}

int encode_gb(unsigned code, unsigned char* out, size_t left, bool four_bytes)
{
    if (code < 0x80) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    }
    u32 pointer, last;
    if (find_pointer(TableKind::GB18030, code, pointer, last)) {
        if (left < 2)
            return 0;
        u32 const trail = pointer % 190;
        out[0] = static_cast<unsigned char>(pointer / 190 + 0x81);
        out[1] = static_cast<unsigned char>(trail + (trail < 0x3f ? 0x40 : 0x41));
        return 2;
    }
    if (!four_bytes || (code >= 0xd800 && code <= 0xdfff))
        return -1;
    if (left < 4)
        return 0;
    u32 range_pointer = gb18030_range_pointer(code);
    out[3] = static_cast<unsigned char>(range_pointer % 10 + 0x30);
    range_pointer /= 10;
    out[2] = static_cast<unsigned char>(range_pointer % 126 + 0x81);
    range_pointer /= 126;
    out[1] = static_cast<unsigned char>(range_pointer % 10 + 0x30);
    out[0] = static_cast<unsigned char>(range_pointer / 10 + 0x81);
    return 4;
}

int decode_big5(unsigned char const* in, size_t left, unsigned& code)
{
    if (left < 1)
        return 0;
    if (in[0] < 0x80) {
        code = in[0];
        return 1;
    }
    if (in[0] < 0x81 || in[0] > 0xfe)
        return -1;
    if (left < 2)
        return 0;
    unsigned char const trail = in[1];
    if (!((trail >= 0x40 && trail <= 0x7e) || (trail >= 0xa1 && trail <= 0xfe)))
        return -1;
    unsigned const trail_offset = trail < 0x7f ? 0x40 : 0x62;
    code = value_at(TableKind::Big5, (in[0] - 0x81u) * 157 + (trail - trail_offset));
    return code ? 2 : -1;
}

int encode_big5(unsigned code, unsigned char* out, size_t left)
{
    if (code < 0x80) {
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    }
    u32 pointer, last;
    if (!find_pointer(TableKind::Big5, code, pointer, last))
        return -1;
    // A few are in there twice, and the later is where Big5 itself has them.
    if (code == 0x2550 || code == 0x255e || code == 0x2561 || code == 0x256a || code == 0x5341 || code == 0x5345)
        pointer = last;
    if (left < 2)
        return 0;
    u32 const trail = pointer % 157;
    out[0] = static_cast<unsigned char>(pointer / 157 + 0x81);
    out[1] = static_cast<unsigned char>(trail + (trail < 0x3f ? 0x40 : 0x62));
    return 2;
}

}

int decode(MultiByte encoding, unsigned char const* in, size_t left, unsigned& code)
{
    switch (encoding) {
    case MultiByte::ShiftJIS:
        return decode_shift_jis(in, left, code);
    case MultiByte::EUCJP:
        return decode_euc_jp(in, left, code);
    case MultiByte::EUCKR:
        return decode_euc_kr(in, left, code);
    case MultiByte::GBK:
        return decode_gb(in, left, code, false);
    case MultiByte::GB18030:
        return decode_gb(in, left, code, true);
    case MultiByte::Big5:
        return decode_big5(in, left, code);
    }
    return -1;
}

int encode(MultiByte encoding, unsigned code, unsigned char* out, size_t left)
{
    switch (encoding) {
    case MultiByte::ShiftJIS:
        return encode_shift_jis(code, out, left);
    case MultiByte::EUCJP:
        return encode_euc_jp(code, out, left);
    case MultiByte::EUCKR:
        return encode_euc_kr(code, out, left);
    case MultiByte::GBK:
        return encode_gb(code, out, left, false);
    case MultiByte::GB18030:
        return encode_gb(code, out, left, true);
    case MultiByte::Big5:
        return encode_big5(code, out, left);
    }
    return -1;
}

}
