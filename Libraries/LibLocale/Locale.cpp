/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibLocale/Locale.h>

namespace Locale {

LocaleData const* find_locale(StringView identifier)
{
    // en-US as well as en_US.
    char normalized[32];
    if (identifier.is_empty() || identifier.length() >= sizeof(normalized))
        return nullptr;
    for (size_t i = 0; i < identifier.length(); ++i)
        normalized[i] = identifier[i] == '-' ? '_' : identifier[i];
    StringView const wanted { normalized, identifier.length() };
    for (size_t i = 0; i < locale_count; ++i) {
        if (StringView(locales[i].identifier) == wanted)
            return &locales[i];
    }
    // The language, at least: its own country, as far as what's here goes.
    auto const language_length = wanted.find('_').value_or(wanted.length());
    auto const language = wanted.substring_view(0, language_length);
    for (size_t i = 0; i < locale_count; ++i) {
        if (StringView(locales[i].language) == language)
            return &locales[i];
    }
    return nullptr;
}

LocaleData const& default_locale()
{
    return *find_locale("en_US"_sv);
}

}
