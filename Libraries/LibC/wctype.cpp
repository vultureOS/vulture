/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Generated/Enums.h>
#include <LibUnicode/Tables.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

// What kind of character it is, by Unicode's tables and the rules a Mac's C library goes by (found by asking it about every code point: see Tests/Parity/wctype-oracle.c). These
// aren't quite the same as what Unicode would call a letter: marks aren't letters here, and neither are other scripts' digits digits.

using Unicode::GeneralCategory;

namespace {

GeneralCategory category_of(wint_t code)
{
    using namespace Unicode::Tables;
    if (code > 0x10ffff)
        return GeneralCategory::Cn;
    return static_cast<GeneralCategory>(run_value(general_category, general_category_count, code));
}

bool never_a_character(wint_t code)
{
    return (code >= 0xfdd0 && code <= 0xfdef) || (code & 0xfffe) == 0xfffe;
}

}

extern "C" {

int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswxdigit(wint_t c) { return iswdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iswupper(wint_t c) { return category_of(c) == GeneralCategory::Lu; }
int iswlower(wint_t c) { return category_of(c) == GeneralCategory::Ll; }

int iswalpha(wint_t c)
{
    auto const category = category_of(c);
    return category == GeneralCategory::Lu || category == GeneralCategory::Ll || category == GeneralCategory::Lt || category == GeneralCategory::Lm || category == GeneralCategory::Lo;
}

int iswalnum(wint_t c) { return iswalpha(c) || iswdigit(c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }

int iswcntrl(wint_t c)
{
    auto const category = category_of(c);
    return category == GeneralCategory::Cc || category == GeneralCategory::Cf;
}

int iswspace(wint_t c)
{
    if (c == ' ' || (c >= '\t' && c <= '\r'))
        return 1;
    auto const category = category_of(c);
    return category == GeneralCategory::Zs || category == GeneralCategory::Zl || category == GeneralCategory::Zp;
}

int iswprint(wint_t c)
{
    auto const category = category_of(c);
    if (category == GeneralCategory::Cn || category == GeneralCategory::Cc || category == GeneralCategory::Cf || category == GeneralCategory::Cs)
        return 0;
    return !never_a_character(c);
}

int iswgraph(wint_t c)
{
    auto const category = category_of(c);
    return iswprint(c) && category != GeneralCategory::Zs && category != GeneralCategory::Zl && category != GeneralCategory::Zp;
}

int iswpunct(wint_t c)
{
    switch (category_of(c)) {
    case GeneralCategory::Pc:
    case GeneralCategory::Pd:
    case GeneralCategory::Ps:
    case GeneralCategory::Pe:
    case GeneralCategory::Pi:
    case GeneralCategory::Pf:
    case GeneralCategory::Po:
        return 1;
    case GeneralCategory::Sm:
    case GeneralCategory::Sc:
    case GeneralCategory::Sk:
    case GeneralCategory::So:
        // The symbols only count in ASCII, where C has always said so.
        return c < 0x80;
    default:
        return 0;
    }
}

wint_t towlower(wint_t c)
{
    using namespace Unicode::Tables;
    if (auto const* entry = find_entry(simple_lower, simple_lower_count, 2, c))
        return entry[1];
    return c;
}

wint_t towupper(wint_t c)
{
    using namespace Unicode::Tables;
    if (auto const* entry = find_entry(simple_upper, simple_upper_count, 2, c))
        return entry[1];
    return c;
}

wctype_t wctype(char const* name)
{
    char const* const names[] = { "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0)
            return i + 1;
    }
    return 0;
}

int iswctype(wint_t c, wctype_t type)
{
    switch (type) {
    case 1:
        return iswalnum(c);
    case 2:
        return iswalpha(c);
    case 3:
        return iswblank(c);
    case 4:
        return iswcntrl(c);
    case 5:
        return iswdigit(c);
    case 6:
        return iswgraph(c);
    case 7:
        return iswlower(c);
    case 8:
        return iswprint(c);
    case 9:
        return iswpunct(c);
    case 10:
        return iswspace(c);
    case 11:
        return iswupper(c);
    case 12:
        return iswxdigit(c);
    default:
        return 0;
    }
}

// How many columns of a terminal it takes.
int wcwidth(wchar_t character)
{
    using namespace Unicode::Tables;
    auto const code = static_cast<wint_t>(character);
    if (code == 0)
        return 0;
    // (Soft hyphen: a format character that's still a control here, as it's only shown at the end of a line.)
    if (code == 0xad || never_a_character(code))
        return -1;
    auto const category = category_of(code);
    if (category == GeneralCategory::Cc || category == GeneralCategory::Cs || category == GeneralCategory::Cn)
        return -1;
    if (category == GeneralCategory::Mn || category == GeneralCategory::Me || category == GeneralCategory::Cf || category == GeneralCategory::Zl || category == GeneralCategory::Zp)
        return 0;
    // Conjoining vowels and final consonants go with what came first.
    if (code >= 0x1160 && code <= 0x11ff)
        return 0;
    auto const width = static_cast<Unicode::EastAsianWidth>(run_value(east_asian_width, east_asian_width_count, code));
    if (width == Unicode::EastAsianWidth::W || width == Unicode::EastAsianWidth::F)
        return 2;
    // A mark that takes up room of its own still shares the cell of what it's on.
    return category == GeneralCategory::Mc ? 0 : 1;
}

}
