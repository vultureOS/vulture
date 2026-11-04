/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibForeignFS/Volume.h>
#include <LibUnicode/Unicode.h>

namespace ForeignFS {

static ErrorOr<void> append_utf16(Vector<u16>& output, Unicode::CodePoint code_point)
{
    if (code_point < 0x10000)
        return output.try_append(static_cast<u16>(code_point));
    u32 const rest = code_point - 0x10000;
    TRY(output.try_append(static_cast<u16>(0xd800 + (rest >> 10))));
    return output.try_append(static_cast<u16>(0xdc00 + (rest & 0x3ff)));
}

ErrorOr<Vector<u16>> utf16_of(StringView utf8)
{
    if (!Unicode::is_valid_utf8(utf8))
        return Error::from_errno(EILSEQ);
    auto const code_points = TRY(Unicode::code_points_of(utf8));
    Vector<u16> output;
    for (auto code_point : code_points)
        TRY(append_utf16(output, code_point));
    return output;
}

ErrorOr<String> utf8_of_utf16(Span<u16 const> utf16)
{
    Vector<Unicode::CodePoint> code_points;
    for (size_t i = 0; i < utf16.size(); ++i) {
        u32 unit = utf16[i];
        if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < utf16.size() && utf16[i + 1] >= 0xdc00 && utf16[i + 1] < 0xe000) {
            unit = 0x10000 + ((unit - 0xd800) << 10) + (utf16[i + 1] - 0xdc00);
            ++i;
        } else if (unit >= 0xd800 && unit < 0xe000) {
            // Half of a pair, from somewhere that didn't care: shown as what can't be shown.
            unit = 0xfffd;
        }
        TRY(code_points.try_append(unit));
    }
    return Unicode::utf8_of({ code_points.begin(), code_points.size() });
}

ErrorOr<Vector<u16>> comparable_name(StringView utf8)
{
    if (!Unicode::is_valid_utf8(utf8))
        return Error::from_errno(ENOENT);
    auto const normalized = TRY(Unicode::normalize(utf8, Unicode::NormalizationForm::NFC));
    auto const code_points = TRY(Unicode::code_points_of(normalized.view()));
    Vector<u16> output;
    for (auto code_point : code_points)
        TRY(append_utf16(output, Unicode::simple_to_upper(code_point)));
    return output;
}

}
