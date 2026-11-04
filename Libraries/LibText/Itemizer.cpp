/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Itemizer.h>

extern "C" {
int open(char const* path, int flags, ...);
int close(int fd);
long lseek(int fd, long offset, int whence);
void* mmap(void* address, unsigned long length, int protection, int flags, int fd, long offset);
}

namespace Text {

using Unicode::CodePoint;
using Unicode::Script;

namespace {

// For good: a font in the cascade's there until the program ends. And the same pages for every program, however many are using it, as some of these are large.
ReadonlyBytes map_whole_file(char const* path)
{
    int const fd = open(path, 0);
    if (fd < 0)
        return {};
    long const size = lseek(fd, 0, 2);
    constexpr int protection_read = 1;
    constexpr int map_private = 2;
    void* mapped = size > 0 ? mmap(nullptr, static_cast<unsigned long>(size), protection_read, map_private, fd, 0) : reinterpret_cast<void*>(-1);
    close(fd);
    if (mapped == reinterpret_cast<void*>(-1))
        return {};
    return { static_cast<u8 const*>(mapped), static_cast<size_t>(size) };
}

bool is_emoji_like(CodePoint code)
{
    return Unicode::is_emoji_presentation(code) || (code >= 0x1f000 && code <= 0x1faff) || code == 0x200d || code == 0xfe0f || code == 0x20e3;
}

// Which fonts have which script. In the order to try them.
struct ScriptFonts {
    Script script;
    StringView regular;
    StringView bold;
};

ScriptFonts const s_script_fonts[] = {
    { Script::Arabic, "NotoSansArabic-Regular.ttf"_sv, "NotoSansArabic-Bold.ttf"_sv },
    { Script::Hebrew, "NotoSansHebrew-Regular.ttf"_sv, "NotoSansHebrew-Bold.ttf"_sv },
    { Script::Devanagari, "NotoSansDevanagari-Regular.ttf"_sv, "NotoSansDevanagari-Bold.ttf"_sv },
    { Script::Bengali, "NotoSansBengali-Regular.ttf"_sv, "NotoSansBengali-Bold.ttf"_sv },
    { Script::Gurmukhi, "NotoSansGurmukhi-Regular.ttf"_sv, "NotoSansGurmukhi-Bold.ttf"_sv },
    { Script::Gujarati, "NotoSansGujarati-Regular.ttf"_sv, "NotoSansGujarati-Bold.ttf"_sv },
    { Script::Oriya, "NotoSansOriya-Regular.ttf"_sv, "NotoSansOriya-Bold.ttf"_sv },
    { Script::Tamil, "NotoSansTamil-Regular.ttf"_sv, "NotoSansTamil-Bold.ttf"_sv },
    { Script::Telugu, "NotoSansTelugu-Regular.ttf"_sv, "NotoSansTelugu-Bold.ttf"_sv },
    { Script::Kannada, "NotoSansKannada-Regular.ttf"_sv, "NotoSansKannada-Bold.ttf"_sv },
    { Script::Malayalam, "NotoSansMalayalam-Regular.ttf"_sv, "NotoSansMalayalam-Bold.ttf"_sv },
    { Script::Thai, "NotoSansThai-Regular.ttf"_sv, "NotoSansThai-Bold.ttf"_sv },
    { Script::Lao, "NotoSansLao-Regular.ttf"_sv, "NotoSansLao-Bold.ttf"_sv },
    { Script::Han, "NotoSansCJK-Regular.ttc"_sv, "NotoSansCJK-Bold.ttc"_sv },
    { Script::Hiragana, "NotoSansCJK-Regular.ttc"_sv, "NotoSansCJK-Bold.ttc"_sv },
    { Script::Katakana, "NotoSansCJK-Regular.ttc"_sv, "NotoSansCJK-Bold.ttc"_sv },
    { Script::Hangul, "NotoSansCJK-Regular.ttc"_sv, "NotoSansCJK-Bold.ttc"_sv },
};

constexpr StringView emoji_font = "NotoColorEmoji.ttf"_sv;
constexpr StringView symbols_font = "NotoSansSymbols2-Regular.ttf"_sv;
constexpr StringView general_font = "NotoSans-Regular.ttf"_sv;

}

FontCascade& FontCascade::the()
{
    static FontCascade* s_the = new FontCascade;
    return *s_the;
}

void FontCascade::set_directory(StringView directory)
{
    m_directory = MUST(String::from_view(directory));
    m_loaded.clear();
}

Typeface const* FontCascade::load(StringView file)
{
    for (auto& loaded : m_loaded) {
        if (loaded.file.view() == file)
            return loaded.typeface.ptr();
    }
    StringBuilder path;
    path.append(m_directory.is_empty() ? "/System/Library/Fonts"_sv : m_directory.view());
    path.append('/');
    path.append(file);
    auto const full_path = MUST(path.to_string());
    Loaded loaded;
    loaded.file = MUST(String::from_view(file));
    loaded.tried = true;
    auto const bytes = map_whole_file(full_path.characters());
    if (!bytes.is_empty()) {
        if (auto typeface = Typeface::parse_mapped(bytes); !typeface.is_error())
            loaded.typeface = typeface.release_value();
    }
    MUST(m_loaded.try_append(move(loaded)));
    return m_loaded.last().typeface.ptr();
}

Typeface const* FontCascade::emoji_typeface_for(CodePoint code)
{
    auto const* emoji = load(emoji_font);
    return emoji && emoji->glyph_for_code_point(code) != 0 ? emoji : nullptr;
}

Typeface const* FontCascade::typeface_for(Typeface const& primary, CodePoint code, Script script)
{
    if (primary.glyph_for_code_point(code) != 0 && !is_emoji_like(code))
        return &primary;
    if (is_emoji_like(code)) {
        if (auto const* emoji = load(emoji_font); emoji && emoji->glyph_for_code_point(code) != 0)
            return emoji;
    }
    bool const is_bold = primary.weight() >= 600;
    for (auto const& fonts : s_script_fonts) {
        if (fonts.script != script)
            continue;
        auto const* found = load(is_bold ? fonts.bold : fonts.regular);
        if (!found && is_bold)
            found = load(fonts.regular);
        if (found && found->glyph_for_code_point(code) != 0)
            return found;
    }
    if (primary.glyph_for_code_point(code) != 0)
        return &primary;
    auto const* general = load(general_font);
    if (general && general->glyph_for_code_point(code) != 0)
        return general;
    auto const* symbols = load(symbols_font);
    if (symbols && symbols->glyph_for_code_point(code) != 0)
        return symbols;
    // Anything else there is, by trying each. (An unusual character, but there might be something for it.)
    for (auto const& fonts : s_script_fonts) {
        auto const* found = load(fonts.regular);
        if (found && found->glyph_for_code_point(code) != 0)
            return found;
    }
    return nullptr;
}

ErrorOr<Vector<TextItem>> itemize(Span<CodePoint const> text, Typeface const& primary, Unicode::ParagraphDirection direction)
{
    Vector<TextItem> items;
    if (text.is_empty())
        return items;

    // Which way: levels from the bidi algorithm.
    auto const paragraph = TRY(Unicode::resolve_bidi(text, direction));
    auto const levels = TRY(Unicode::reordering_levels(paragraph, 0, text.size()));

    // Which script: what's common takes after what's before it, or else after what's next.
    Vector<Script> scripts;
    TRY(scripts.try_resize(text.size()));
    Script current = Script::Common;
    for (size_t i = 0; i < text.size(); ++i) {
        Script const script = Unicode::script_of(text[i]);
        if (script != Script::Common && script != Script::Inherited && script != Script::Unknown)
            current = script;
        scripts[i] = current;
    }
    Script after = Script::Latin;
    for (size_t i = text.size(); i > 0; --i) {
        if (scripts[i - 1] == Script::Common)
            scripts[i - 1] = after;
        else
            after = scripts[i - 1];
    }

    // Which font: a grapheme cluster at a time, so that nothing gets separated from what it's on.
    Vector<Typeface const*> faces;
    TRY(faces.try_resize(text.size()));
    auto const boundaries = TRY(Unicode::grapheme_boundaries(text));
    for (size_t b = 0; b + 1 < boundaries.size(); ++b) {
        size_t const start = boundaries[b];
        size_t const end = boundaries[b + 1];
        // The first character that isn't invisible says, and the rest have to go along.
        Typeface const* chosen = nullptr;
        // Something that's text by default, with the selector that says it's to be a picture.
        if (end - start >= 2 && text[start + 1] == 0xfe0f)
            chosen = FontCascade::the().emoji_typeface_for(text[start]);
        for (size_t i = start; i < end && !chosen; ++i) {
            if (!Unicode::is_default_ignorable(text[i]))
                chosen = FontCascade::the().typeface_for(primary, text[i], scripts[i]);
        }
        if (!chosen)
            chosen = &primary;
        for (size_t i = start; i < end; ++i)
            faces[i] = chosen;
    }

    // Runs of all three alike, in the order of the text.
    Vector<TextItem> logical;
    for (size_t i = 0; i < text.size();) {
        size_t j = i + 1;
        u8 const level = levels[i] == Unicode::bidi_level_removed ? 0 : levels[i];
        while (j < text.size()) {
            u8 const next_level = levels[j] == Unicode::bidi_level_removed ? level : levels[j];
            if (next_level != level || scripts[j] != scripts[i] || faces[j] != faces[i])
                break;
            ++j;
        }
        TextItem item;
        item.start = i;
        item.end = j;
        item.script = scripts[i];
        item.is_right_to_left = level & 1;
        item.typeface = faces[i];
        TRY(logical.try_append(item));
        i = j;
    }

    // In the order they're drawn: by the levels of the runs, rule L2, as if each run were one character.
    Vector<u8> run_levels;
    for (auto const& item : logical) {
        u8 const level = levels[item.start] == Unicode::bidi_level_removed ? 0 : levels[item.start];
        TRY(run_levels.try_append(level));
    }
    auto const order = TRY(Unicode::visual_order(run_levels.span()));
    for (size_t place : order)
        TRY(items.try_append(logical[place]));
    return items;
}

}
