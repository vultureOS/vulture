/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include "Encodings.h"
#include <iconv.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>

// Wide characters are code points, and multibyte strings are UTF-8: the same as a Mac in a UTF-8 locale, and all there is here.
// Telling one character from another (which are letters, what's the upper case of it) is for ASCII and Latin-1 only, until M17
// brings Unicode's tables.

extern "C" {

size_t wcslen(wchar_t const* text)
{
    size_t length = 0;
    while (text[length])
        ++length;
    return length;
}

wchar_t* wcscpy(wchar_t* to, wchar_t const* from)
{
    size_t i = 0;
    for (; from[i]; ++i)
        to[i] = from[i];
    to[i] = 0;
    return to;
}

wchar_t* wcsncpy(wchar_t* to, wchar_t const* from, size_t count)
{
    size_t i = 0;
    for (; i < count && from[i]; ++i)
        to[i] = from[i];
    for (; i < count; ++i)
        to[i] = 0;
    return to;
}

wchar_t* wcscat(wchar_t* to, wchar_t const* from)
{
    wcscpy(to + wcslen(to), from);
    return to;
}

wchar_t* wcsncat(wchar_t* to, wchar_t const* from, size_t count)
{
    wchar_t* at = to + wcslen(to);
    size_t i = 0;
    for (; i < count && from[i]; ++i)
        at[i] = from[i];
    at[i] = 0;
    return to;
}

int wcscmp(wchar_t const* a, wchar_t const* b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a < *b ? -1 : *a > *b ? 1 : 0;
}

int wcsncmp(wchar_t const* a, wchar_t const* b, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

wchar_t* wcschr(wchar_t const* text, wchar_t wanted)
{
    for (;; ++text) {
        if (*text == wanted)
            return const_cast<wchar_t*>(text);
        if (!*text)
            return nullptr;
    }
}

wchar_t* wcsrchr(wchar_t const* text, wchar_t wanted)
{
    wchar_t const* found = nullptr;
    for (;; ++text) {
        if (*text == wanted)
            found = text;
        if (!*text)
            return const_cast<wchar_t*>(found);
    }
}

wchar_t* wcsstr(wchar_t const* text, wchar_t const* wanted)
{
    size_t const length = wcslen(wanted);
    if (!length)
        return const_cast<wchar_t*>(text);
    for (; *text; ++text) {
        if (wcsncmp(text, wanted, length) == 0)
            return const_cast<wchar_t*>(text);
    }
    return nullptr;
}

size_t wcsspn(wchar_t const* text, wchar_t const* accepted)
{
    size_t i = 0;
    while (text[i] && wcschr(accepted, text[i]))
        ++i;
    return i;
}

size_t wcscspn(wchar_t const* text, wchar_t const* rejected)
{
    size_t i = 0;
    while (text[i] && !wcschr(rejected, text[i]))
        ++i;
    return i;
}

wchar_t* wmemchr(wchar_t const* text, wchar_t wanted, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (text[i] == wanted)
            return const_cast<wchar_t*>(text + i);
    }
    return nullptr;
}

int wmemcmp(wchar_t const* a, wchar_t const* b, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

wchar_t* wmemcpy(wchar_t* to, wchar_t const* from, size_t count)
{
    return static_cast<wchar_t*>(memcpy(to, from, count * sizeof(wchar_t)));
}

wchar_t* wmemmove(wchar_t* to, wchar_t const* from, size_t count)
{
    return static_cast<wchar_t*>(memmove(to, from, count * sizeof(wchar_t)));
}

wchar_t* wmemset(wchar_t* to, wchar_t value, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        to[i] = value;
    return to;
}

// What can be a number comes across as ASCII, then the narrow functions take it from there.
namespace {

constexpr size_t number_room = 128;

size_t narrow_number(wchar_t const* text, char* out)
{
    size_t i = 0;
    while (i + 1 < number_room && text[i] > 0 && text[i] < 128) {
        out[i] = static_cast<char>(text[i]);
        ++i;
    }
    out[i] = 0;
    return i;
}

}

long wcstol(wchar_t const* text, wchar_t** end, int base)
{
    char narrow[number_room];
    narrow_number(text, narrow);
    char* stop = nullptr;
    long const value = strtol(narrow, &stop, base);
    if (end)
        *end = const_cast<wchar_t*>(text) + (stop - narrow);
    return value;
}

unsigned long wcstoul(wchar_t const* text, wchar_t** end, int base)
{
    char narrow[number_room];
    narrow_number(text, narrow);
    char* stop = nullptr;
    unsigned long const value = strtoul(narrow, &stop, base);
    if (end)
        *end = const_cast<wchar_t*>(text) + (stop - narrow);
    return value;
}

double wcstod(wchar_t const* text, wchar_t** end)
{
    char narrow[number_room];
    narrow_number(text, narrow);
    char* stop = nullptr;
    double const value = strtod(narrow, &stop);
    if (end)
        *end = const_cast<wchar_t*>(text) + (stop - narrow);
    return value;
}

int wcswidth(wchar_t const* text, size_t count)
{
    int width = 0;
    for (size_t i = 0; i < count && text[i]; ++i) {
        int const one = wcwidth(text[i]);
        if (one < 0)
            return -1;
        width += one;
    }
    return width;
}

// ---- UTF-8.

size_t mbrtowc(wchar_t* out, char const* text, size_t length, mbstate_t* state)
{
    static mbstate_t internal;
    if (!state)
        state = &internal;
    if (!text) {
        // "Start again": whatever was half read has to have been finished.
        bool const was_clean = state->remaining == 0;
        state->remaining = 0;
        state->value = 0;
        if (!was_clean) {
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        return 0;
    }
    if (length == 0)
        return static_cast<size_t>(-2);
    size_t used = 0;
    unsigned value = state->value;
    int remaining = state->remaining;
    if (remaining == 0) {
        auto const first = static_cast<unsigned char>(text[used++]);
        if (first < 0x80) {
            value = first;
        } else if (first >= 0xc2 && first < 0xe0) {
            value = first & 0x1f;
            remaining = 1;
        } else if (first >= 0xe0 && first < 0xf0) {
            value = first & 0x0f;
            remaining = 2;
        } else if (first >= 0xf0 && first < 0xf5) {
            value = first & 0x07;
            remaining = 3;
        } else {
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
    }
    while (remaining > 0) {
        if (used >= length) {
            state->value = value;
            state->remaining = remaining;
            return static_cast<size_t>(-2);
        }
        auto const next = static_cast<unsigned char>(text[used++]);
        if ((next & 0xc0) != 0x80) {
            state->remaining = 0;
            state->value = 0;
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        value = (value << 6) | (next & 0x3f);
        --remaining;
    }
    state->value = 0;
    state->remaining = 0;
    if (value > 0x10ffff || (value >= 0xd800 && value < 0xe000)) {
        errno = EILSEQ;
        return static_cast<size_t>(-1);
    }
    if (out)
        *out = static_cast<wchar_t>(value);
    return value == 0 ? 0 : used;
}

size_t wcrtomb(char* out, wchar_t character, mbstate_t*)
{
    char scratch[4];
    if (!out) {
        out = scratch;
        character = 0;
    }
    auto const code = static_cast<unsigned>(character);
    if (code < 0x80) {
        out[0] = static_cast<char>(code);
        return 1;
    }
    if (code < 0x800) {
        out[0] = static_cast<char>(0xc0 | (code >> 6));
        out[1] = static_cast<char>(0x80 | (code & 0x3f));
        return 2;
    }
    if (code >= 0xd800 && code < 0xe000) {
        errno = EILSEQ;
        return static_cast<size_t>(-1);
    }
    if (code < 0x10000) {
        out[0] = static_cast<char>(0xe0 | (code >> 12));
        out[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out[2] = static_cast<char>(0x80 | (code & 0x3f));
        return 3;
    }
    if (code <= 0x10ffff) {
        out[0] = static_cast<char>(0xf0 | (code >> 18));
        out[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out[3] = static_cast<char>(0x80 | (code & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return static_cast<size_t>(-1);
}

size_t mbrlen(char const* text, size_t length, mbstate_t* state)
{
    static mbstate_t internal;
    return mbrtowc(nullptr, text, length, state ? state : &internal);
}

size_t mbsrtowcs(wchar_t* out, char const** source, size_t count, mbstate_t* state)
{
    static mbstate_t internal;
    if (!state)
        state = &internal;
    char const* text = *source;
    size_t written = 0;
    while (!out || written < count) {
        wchar_t character = 0;
        size_t const used = mbrtowc(&character, text, 4, state);
        if (used == static_cast<size_t>(-1) || used == static_cast<size_t>(-2)) {
            if (out)
                *source = text;
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        if (used == 0) {
            if (out) {
                out[written] = 0;
                *source = nullptr;
            }
            return written;
        }
        if (out)
            out[written] = character;
        ++written;
        text += used;
    }
    *source = text;
    return written;
}

size_t wcsrtombs(char* out, wchar_t const** source, size_t count, mbstate_t* state)
{
    wchar_t const* text = *source;
    size_t written = 0;
    for (;; ++text) {
        char encoded[4];
        size_t const used = wcrtomb(encoded, *text, state);
        if (used == static_cast<size_t>(-1)) {
            if (out)
                *source = text;
            return static_cast<size_t>(-1);
        }
        if (*text == 0) {
            if (out && written < count) {
                out[written] = 0;
                *source = nullptr;
            }
            return written;
        }
        if (out) {
            if (written + used > count) {
                *source = text;
                return written;
            }
            memcpy(out + written, encoded, used);
        }
        written += used;
    }
}

int mbsinit(mbstate_t const* state)
{
    return !state || state->remaining == 0;
}

wint_t btowc(int byte)
{
    return byte >= 0 && byte < 0x80 ? static_cast<wint_t>(byte) : WEOF;
}

int wctob(wint_t character)
{
    return character < 0x80 ? static_cast<int>(character) : EOF;
}

// ---- stdio's side.

wint_t fgetwc(FILE* file)
{
    mbstate_t state {};
    for (;;) {
        int const c = fgetc(file);
        if (c == EOF)
            return WEOF;
        char const byte = static_cast<char>(c);
        wchar_t character = 0;
        size_t const used = mbrtowc(&character, &byte, 1, &state);
        if (used == static_cast<size_t>(-2))
            continue;
        if (used == static_cast<size_t>(-1))
            return WEOF;
        return static_cast<wint_t>(character);
    }
}

wint_t fputwc(wchar_t character, FILE* file)
{
    char encoded[4];
    size_t const used = wcrtomb(encoded, character, nullptr);
    if (used == static_cast<size_t>(-1) || fwrite(encoded, 1, used, file) != used)
        return WEOF;
    return static_cast<wint_t>(character);
}

wint_t getwc(FILE* file) { return fgetwc(file); }
wint_t putwc(wchar_t character, FILE* file) { return fputwc(character, file); }
wint_t getwchar() { return fgetwc(stdin); }
wint_t putwchar(wchar_t character) { return fputwc(character, stdout); }

int fputws(wchar_t const* text, FILE* file)
{
    for (; *text; ++text) {
        if (fputwc(*text, file) == WEOF)
            return -1;
    }
    return 0;
}

// Through the narrow one: a format and what it formats get turned to UTF-8 first.
int swprintf(wchar_t* out, size_t count, wchar_t const* format, ...)
{
    char narrow_format[1024];
    wchar_t const* from = format;
    mbstate_t state {};
    if (wcsrtombs(narrow_format, &from, sizeof(narrow_format), &state) == static_cast<size_t>(-1))
        return -1;
    char narrow[4096];
    va_list arguments;
    va_start(arguments, format);
    int const length = vsnprintf(narrow, sizeof(narrow), narrow_format, arguments);
    va_end(arguments);
    if (length < 0)
        return -1;
    char const* source = narrow;
    size_t const written = mbsrtowcs(out, &source, count, &state);
    if (written == static_cast<size_t>(-1) || written >= count)
        return -1;
    return static_cast<int>(written);
}

// ---- iconv.

namespace {

enum class Encoding : int {
    Invalid,
    ASCII,
    Latin1,
    UTF8,
    UTF16BE,
    UTF16LE,
    UTF16,
    UTF32BE,
    UTF32LE,
    UTF32,
    SingleByte, // Which is in Conversion's table.
    MultiByte,  // And which of those.
};

struct EncodingName {
    Encoding encoding;
    int which;
};

EncodingName encoding_named(char const* name)
{
    char lower[32];
    size_t i = 0;
    for (; name[i] && name[i] != '/' && i + 1 < sizeof(lower); ++i)
        lower[i] = static_cast<char>(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    lower[i] = 0;
    struct Name {
        char const* name;
        Encoding encoding;
    };
    Name const names[] = {
        { "ascii", Encoding::ASCII },
        { "us-ascii", Encoding::ASCII },
        { "ansi_x3.4-1968", Encoding::ASCII },
        { "646", Encoding::ASCII },
        { "iso-8859-1", Encoding::Latin1 },
        { "iso8859-1", Encoding::Latin1 },
        { "latin1", Encoding::Latin1 },
        { "utf-8", Encoding::UTF8 },
        { "utf8", Encoding::UTF8 },
        { "utf-16", Encoding::UTF16 },
        { "utf-16be", Encoding::UTF16BE },
        { "utf-16le", Encoding::UTF16LE },
        { "ucs-2be", Encoding::UTF16BE },
        { "ucs-2le", Encoding::UTF16LE },
        { "utf-32", Encoding::UTF32 },
        { "utf-32be", Encoding::UTF32BE },
        { "utf-32le", Encoding::UTF32LE },
        { "ucs-4be", Encoding::UTF32BE },
        { "ucs-4le", Encoding::UTF32LE },
        { "wchar_t", Encoding::UTF32LE },
    };
    for (auto const& entry : names) {
        if (strcmp(lower, entry.name) == 0)
            return { entry.encoding, 0 };
    }

    using LibC::Encodings::MultiByte;
    struct MultiByteName {
        char const* name;
        MultiByte encoding;
    };
    MultiByteName const multi_byte_names[] = {
        { "shift_jis", MultiByte::ShiftJIS },
        { "shift-jis", MultiByte::ShiftJIS },
        { "sjis", MultiByte::ShiftJIS },
        { "cp932", MultiByte::ShiftJIS },
        { "euc-jp", MultiByte::EUCJP },
        { "eucjp", MultiByte::EUCJP },
        { "euc-kr", MultiByte::EUCKR },
        { "euckr", MultiByte::EUCKR },
        { "cp949", MultiByte::EUCKR },
        { "gb2312", MultiByte::GBK },
        { "gbk", MultiByte::GBK },
        { "cp936", MultiByte::GBK },
        { "euc-cn", MultiByte::GBK },
        { "gb18030", MultiByte::GB18030 },
        { "big5", MultiByte::Big5 },
        { "big-5", MultiByte::Big5 },
        { "cp950", MultiByte::Big5 },
    };
    for (auto const& entry : multi_byte_names) {
        if (strcmp(lower, entry.name) == 0)
            return { Encoding::MultiByte, static_cast<int>(entry.encoding) };
    }

    // The others, by their names and what they're often called.
    struct Alias {
        char const* alias;
        char const* name;
    };
    Alias const aliases[] = {
        { "latin2", "iso-8859-2" }, { "latin3", "iso-8859-3" }, { "latin4", "iso-8859-4" }, { "latin-9", "iso-8859-15" }, { "latin9", "iso-8859-15" },
        { "cyrillic", "iso-8859-5" }, { "arabic", "iso-8859-6" }, { "greek", "iso-8859-7" }, { "hebrew", "iso-8859-8" },
        { "mac", "macintosh" }, { "macroman", "macintosh" }, { "maccyrillic", "x-mac-cyrillic" }, { "cp866", "ibm866" }, { "cp874", "windows-874" },
        { "cp1250", "windows-1250" }, { "cp1251", "windows-1251" }, { "cp1252", "windows-1252" }, { "cp1253", "windows-1253" }, { "cp1254", "windows-1254" },
        { "cp1255", "windows-1255" }, { "cp1256", "windows-1256" }, { "cp1257", "windows-1257" }, { "cp1258", "windows-1258" },
    };
    char const* wanted = lower;
    for (auto const& alias : aliases) {
        if (strcmp(lower, alias.alias) == 0)
            wanted = alias.name;
    }
    // iso8859-2 is iso-8859-2.
    char with_hyphen[40];
    if (strncmp(wanted, "iso8859-", 8) == 0) {
        strcpy(with_hyphen, "iso-");
        strcat(with_hyphen, wanted + 3);
        wanted = with_hyphen;
    }
    for (size_t table = 0; table < LibC::Encodings::single_byte_count; ++table) {
        if (strcmp(wanted, LibC::Encodings::single_byte_names[table]) == 0)
            return { Encoding::SingleByte, static_cast<int>(table) };
    }
    return { Encoding::Invalid, 0 };
}

struct Conversion {
    Encoding from;
    int from_which;
    Encoding to;
    int to_which;
    bool wrote_mark { false };
    bool from_is_little { false };
    bool read_mark { false };
};

// How many bytes it took, 0 for "there's not enough to tell", -1 for what's wrong.
int decode(Conversion& conversion, unsigned char const* in, size_t left, unsigned& code)
{
    switch (conversion.from) {
    case Encoding::SingleByte: {
        if (left < 1)
            return 0;
        if (in[0] < 0x80) {
            code = in[0];
            return 1;
        }
        code = LibC::Encodings::single_byte_tables[conversion.from_which][in[0] - 0x80];
        return code ? 1 : -1;
    }
    case Encoding::MultiByte:
        return LibC::Encodings::decode(static_cast<LibC::Encodings::MultiByte>(conversion.from_which), in, left, code);
    case Encoding::ASCII:
        if (left < 1)
            return 0;
        if (in[0] >= 0x80)
            return -1;
        code = in[0];
        return 1;
    case Encoding::Latin1:
        if (left < 1)
            return 0;
        code = in[0];
        return 1;
    case Encoding::UTF8: {
        if (left < 1)
            return 0;
        mbstate_t state {};
        wchar_t character = 0;
        size_t const used = mbrtowc(&character, reinterpret_cast<char const*>(in), left, &state);
        if (used == static_cast<size_t>(-2))
            return 0;
        if (used == static_cast<size_t>(-1))
            return -1;
        code = static_cast<unsigned>(character);
        return used == 0 ? 1 : static_cast<int>(used);
    }
    case Encoding::UTF16:
    case Encoding::UTF16BE:
    case Encoding::UTF16LE: {
        if (left < 2)
            return 0;
        int consumed = 0;
        bool little = conversion.from == Encoding::UTF16LE;
        if (conversion.from == Encoding::UTF16) {
            if (!conversion.read_mark) {
                conversion.read_mark = true;
                if ((in[0] == 0xfe && in[1] == 0xff) || (in[0] == 0xff && in[1] == 0xfe)) {
                    conversion.from_is_little = in[0] == 0xff;
                    consumed = 2;
                    in += 2;
                    left -= 2;
                    if (left < 2)
                        return consumed ? -2 : 0;
                }
            }
            little = conversion.from_is_little;
        }
        auto unit = [&](size_t at) { return little ? static_cast<unsigned>(in[at]) | (static_cast<unsigned>(in[at + 1]) << 8) : (static_cast<unsigned>(in[at]) << 8) | in[at + 1]; };
        unsigned const first = unit(0);
        if (first >= 0xd800 && first < 0xdc00) {
            if (left < 4)
                return 0;
            unsigned const second = unit(2);
            if (second < 0xdc00 || second >= 0xe000)
                return -1;
            code = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
            return consumed + 4;
        }
        if (first >= 0xdc00 && first < 0xe000)
            return -1;
        code = first;
        return consumed + 2;
    }
    case Encoding::UTF32:
    case Encoding::UTF32BE:
    case Encoding::UTF32LE: {
        if (left < 4)
            return 0;
        bool const little = conversion.from == Encoding::UTF32LE;
        code = little ? static_cast<unsigned>(in[0]) | static_cast<unsigned>(in[1]) << 8 | static_cast<unsigned>(in[2]) << 16 | static_cast<unsigned>(in[3]) << 24
                      : static_cast<unsigned>(in[0]) << 24 | static_cast<unsigned>(in[1]) << 16 | static_cast<unsigned>(in[2]) << 8 | in[3];
        if (code > 0x10ffff || (code >= 0xd800 && code < 0xe000))
            return -1;
        return 4;
    }
    default:
        return -1;
    }
}

// How many bytes it took, 0 if it won't fit, -1 if the encoding can't say it.
int encode(Conversion& conversion, unsigned code, unsigned char* out, size_t left)
{
    auto put_16 = [&](unsigned value, unsigned char* at, bool little) {
        at[little ? 0 : 1] = static_cast<unsigned char>(value);
        at[little ? 1 : 0] = static_cast<unsigned char>(value >> 8);
    };
    switch (conversion.to) {
    case Encoding::SingleByte: {
        if (code < 0x80) {
            if (left < 1)
                return 0;
            out[0] = static_cast<unsigned char>(code);
            return 1;
        }
        auto const* table = LibC::Encodings::single_byte_tables[conversion.to_which];
        for (int i = 0; i < 128; ++i) {
            if (table[i] == code) {
                if (left < 1)
                    return 0;
                out[0] = static_cast<unsigned char>(0x80 + i);
                return 1;
            }
        }
        return -1;
    }
    case Encoding::MultiByte:
        return LibC::Encodings::encode(static_cast<LibC::Encodings::MultiByte>(conversion.to_which), code, out, left);
    case Encoding::ASCII:
        if (code >= 0x80)
            return -1;
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    case Encoding::Latin1:
        if (code >= 0x100)
            return -1;
        if (left < 1)
            return 0;
        out[0] = static_cast<unsigned char>(code);
        return 1;
    case Encoding::UTF8: {
        char encoded[4];
        size_t const used = wcrtomb(encoded, static_cast<wchar_t>(code), nullptr);
        if (used == static_cast<size_t>(-1))
            return -1;
        if (left < used)
            return 0;
        memcpy(out, encoded, used);
        return static_cast<int>(used);
    }
    case Encoding::UTF16:
    case Encoding::UTF16BE:
    case Encoding::UTF16LE: {
        // As big-endian with a mark, for plain UTF-16.
        bool const little = conversion.to == Encoding::UTF16LE;
        size_t const mark = conversion.to == Encoding::UTF16 && !conversion.wrote_mark ? 2 : 0;
        size_t const units = code >= 0x10000 ? 2 : 1;
        if (left < mark + units * 2)
            return 0;
        if (mark) {
            put_16(0xfeff, out, false);
            conversion.wrote_mark = true;
        }
        if (units == 2) {
            unsigned const reduced = code - 0x10000;
            put_16(0xd800 + (reduced >> 10), out + mark, little);
            put_16(0xdc00 + (reduced & 0x3ff), out + mark + 2, little);
        } else {
            put_16(code, out + mark, little);
        }
        return static_cast<int>(mark + units * 2);
    }
    case Encoding::UTF32:
    case Encoding::UTF32BE:
    case Encoding::UTF32LE: {
        bool const little = conversion.to == Encoding::UTF32LE;
        size_t const mark = conversion.to == Encoding::UTF32 && !conversion.wrote_mark ? 4 : 0;
        if (left < mark + 4)
            return 0;
        auto put_32 = [&](unsigned value, unsigned char* at) {
            for (int i = 0; i < 4; ++i)
                at[little ? i : 3 - i] = static_cast<unsigned char>(value >> (8 * i));
        };
        if (mark) {
            put_32(0xfeff, out);
            conversion.wrote_mark = true;
        }
        put_32(code, out + mark);
        return static_cast<int>(mark + 4);
    }
    default:
        return -1;
    }
}

}

iconv_t iconv_open(char const* to, char const* from)
{
    auto const to_encoding = encoding_named(to);
    auto const from_encoding = encoding_named(from);
    if (to_encoding.encoding == Encoding::Invalid || from_encoding.encoding == Encoding::Invalid) {
        errno = EINVAL;
        return reinterpret_cast<iconv_t>(-1);
    }
    auto* conversion = static_cast<Conversion*>(malloc(sizeof(Conversion)));
    if (!conversion) {
        errno = ENOMEM;
        return reinterpret_cast<iconv_t>(-1);
    }
    *conversion = { from_encoding.encoding, from_encoding.which, to_encoding.encoding, to_encoding.which };
    return conversion;
}

size_t iconv(iconv_t descriptor, char** in, size_t* in_left, char** out, size_t* out_left)
{
    auto& conversion = *static_cast<Conversion*>(descriptor);
    // "Back to the start."
    if (!in || !*in) {
        conversion.wrote_mark = false;
        conversion.read_mark = false;
        return 0;
    }
    size_t irreversible = 0;
    while (*in_left > 0) {
        unsigned code = 0;
        auto const* from = reinterpret_cast<unsigned char const*>(*in);
        int const read = decode(conversion, from, *in_left, code);
        if (read == 0) {
            errno = EINVAL;
            return static_cast<size_t>(-1);
        }
        if (read == -2) {
            // Only a byte order mark: that's used up, and there's nothing more for now.
            *in += 2;
            *in_left -= 2;
            continue;
        }
        if (read < 0) {
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        int const wrote = encode(conversion, code, reinterpret_cast<unsigned char*>(*out), *out_left);
        if (wrote == 0) {
            errno = E2BIG;
            return static_cast<size_t>(-1);
        }
        if (wrote < 0) {
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        *in += read;
        *in_left -= static_cast<size_t>(read);
        *out += wrote;
        *out_left -= static_cast<size_t>(wrote);
    }
    return irreversible;
}

int iconv_close(iconv_t descriptor)
{
    free(descriptor);
    return 0;
}

}
