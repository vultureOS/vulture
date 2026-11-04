/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Shaper.h>

// What this system's shaper does with a corpus of text, in a font: for comparing with what Core Text does with the same text in the same font (Tests/Parity/shaping-oracle.mm).
//
//     shape-print FONT.ttf SCRIPT RTL TEXT...      (each TEXT in UTF-8)

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const*, ...);
int strcmp(char const*, char const*);
int atoi(char const*);
size_t strlen(char const*);
}

using namespace Text;

namespace {

Vector<u8> read_file(char const* path)
{
    FILE* file = fopen(path, "rb");
    VERIFY(file);
    Vector<u8> bytes;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t const at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return bytes;
}

Unicode::Script script_named(char const* name)
{
    using Unicode::Script;
    struct Named {
        char const* name;
        Script script;
    };
    Named const all[] = {
        { "Arabic", Script::Arabic }, { "Hebrew", Script::Hebrew }, { "Devanagari", Script::Devanagari }, { "Bengali", Script::Bengali }, { "Tamil", Script::Tamil },
        { "Thai", Script::Thai }, { "Hangul", Script::Hangul }, { "Han", Script::Han }, { "Latin", Script::Latin }, { "Greek", Script::Greek }, { "Cyrillic", Script::Cyrillic },
        { "Kannada", Script::Kannada }, { "Telugu", Script::Telugu }, { "Malayalam", Script::Malayalam }, { "Gujarati", Script::Gujarati }, { "Gurmukhi", Script::Gurmukhi }, { "Oriya", Script::Oriya },
        { "Lao", Script::Lao }, { "Hiragana", Script::Hiragana }, { "Katakana", Script::Katakana },
    };
    for (auto const& entry : all) {
        if (strcmp(entry.name, name) == 0)
            return entry.script;
    }
    return Script::Latin;
}

}

int main(int argc, char** argv)
{
    if (argc < 5)
        return 2;
    auto typeface = MUST(Typeface::parse(read_file(argv[1])));
    // shape-print FONT --variations WGHT WDTH TEXT: each character's advance and outline, at those settings.
    if (strcmp(argv[2], "--variations") == 0) {
        Typeface::VariationSetting const settings[] = { { feature_tag("wght"), static_cast<float>(atoi(argv[3])) }, { feature_tag("wdth"), static_cast<float>(atoi(argv[4])) } };
        auto varied = MUST(typeface->with_variations({ settings, 2 }));
        auto const points = MUST(Unicode::code_points_of(StringView(argv[5], strlen(argv[5]))));
        printf("%s %s:", argv[3], argv[4]);
        for (auto const point : points) {
            auto const glyph = varied->glyph_for_code_point(point);
            auto const bounds = varied->outline_of(glyph).bounds();
            printf(" %u:%u:%d,%d,%d,%d", glyph, varied->advance_of(glyph), static_cast<int>(bounds.x + (bounds.x < 0 ? -0.5f : 0.5f)), static_cast<int>(bounds.y + (bounds.y < 0 ? -0.5f : 0.5f)), static_cast<int>(bounds.x + bounds.width + 0.5f), static_cast<int>(bounds.y + bounds.height + 0.5f));
        }
        printf("\n");
        return 0;
    }
    auto const script = script_named(argv[2]);
    bool const is_right_to_left = atoi(argv[3]) != 0;
    for (int i = 4; i < argc; ++i) {
        auto const points = MUST(Unicode::code_points_of(StringView(argv[i], strlen(argv[i]))));
        ShapeRequest request;
        request.text = points.span();
        request.script = script;
        request.is_right_to_left = is_right_to_left;
        auto const glyphs = MUST(shape(*typeface, request));
        printf("%s:", argv[i]);
        i32 pen = 0;
        for (auto const& glyph : glyphs) {
            printf(" %u@%d,%d", glyph.glyph, pen + glyph.x_offset, glyph.y_offset);
            pen += glyph.x_advance;
        }
        printf(" |%d\n", pen);
    }
    return 0;
}
