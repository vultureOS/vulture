/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/UEFI/UEFI.h>
#include <LibBase/Format.h>

namespace Boot {

// The firmware text console as a FormatSink. Converts UTF-8 to the UCS-2 the
// firmware expects and turns "\n" into "\r\n".
class Console final : public FormatSink {
public:
    explicit Console(UEFI::SimpleTextOutputProtocol* output)
        : m_output(output)
    {
    }

    void write(StringView text) override;
    void write_ucs2(char16_t const* text);

    void set_color(UEFI::TextColor foreground, UEFI::TextColor background = UEFI::TextColor::Black);
    void clear();

    template<typename... Args>
    void print(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
    {
        format_to(*this, format, args...);
    }

    template<typename... Args>
    void println(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
    {
        format_to(*this, format, args...);
        write("\n"_sv);
    }

private:
    void append(char16_t);
    void flush();

    UEFI::SimpleTextOutputProtocol* m_output { nullptr };
    char16_t m_buffer[128];
    size_t m_used { 0 };
};

// Set once in efi_main; used to report failures from anywhere in the loader.
extern Console* g_console;

}
