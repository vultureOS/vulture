/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCompress/Deflate.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/Unicode.h>

// The files Unicode publishes to test an implementation of its algorithms with. Every line of them has to pass: that's what conformance is.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const*, ...);
}

using namespace Unicode;

namespace {

Vector<u8> read_gzipped(StringView name)
{
    StringBuilder path;
    path.append(StringView(VULTURE_UNICODE_TEST_DATA));
    path.append(name);
    path.append(".gz"_sv);
    auto const full_path = MUST(path.to_string());
    FILE* file = fopen(full_path.characters(), "rb");
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
    return MUST(Compress::gzip_decompress(bytes.span()));
}

template<typename Each>
void for_each_line(Vector<u8> const& data, Each each)
{
    StringView const all { reinterpret_cast<char const*>(data.data()), data.size() };
    size_t start = 0;
    size_t number = 0;
    while (start < all.length()) {
        size_t end = start;
        while (end < all.length() && all[end] != '\n')
            ++end;
        ++number;
        auto line = all.substring_view(start, end - start);
        each(line, number);
        start = end + 1;
    }
}

u32 hex_value(StringView text)
{
    u32 value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        char const c = text[i];
        value = value * 16 + static_cast<u32>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return value;
}

// "0061 0301": code points.
Vector<CodePoint> parse_code_points(StringView text)
{
    Vector<CodePoint> result;
    size_t i = 0;
    while (i < text.length()) {
        while (i < text.length() && text[i] == ' ')
            ++i;
        size_t const start = i;
        while (i < text.length() && text[i] != ' ')
            ++i;
        if (i > start)
            MUST(result.try_append(hex_value(text.substring_view(start, i - start))));
    }
    return result;
}

StringView before_comment(StringView line)
{
    auto const at = line.find('#');
    auto trimmed = at.has_value() ? line.substring_view(0, at.value()) : line;
    while (!trimmed.is_empty() && (trimmed[trimmed.length() - 1] == ' ' || trimmed[trimmed.length() - 1] == '\t'))
        trimmed = trimmed.substring_view(0, trimmed.length() - 1);
    return trimmed;
}

Vector<StringView> split_on(StringView text, char separator)
{
    Vector<StringView> fields;
    size_t start = 0;
    for (size_t i = 0; i <= text.length(); ++i) {
        if (i == text.length() || text[i] == separator) {
            MUST(fields.try_append(text.substring_view(start, i - start)));
            start = i + 1;
        }
    }
    return fields;
}

bool same(Vector<CodePoint> const& a, Vector<CodePoint> const& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

constexpr size_t most_shown = 8;

struct Tally {
    char const* name;
    size_t passed { 0 };
    size_t failed { 0 };
    void note(bool ok, size_t line)
    {
        if (ok) {
            ++passed;
            return;
        }
        ++failed;
        if (failed <= most_shown)
            printf("%s: line %zu is wrong\n", name, line);
    }
    void report() const
    {
        printf("%s: %zu of %zu\n", name, passed, passed + failed);
    }
};

}

TEST_CASE(normalization)
{
    Tally tally { "NormalizationTest" };
    auto const data = read_gzipped("NormalizationTest.txt"_sv);
    // Everything that's in part 1, which is every character with a decomposition. The rest should come through it all as they are.
    Vector<u8> in_part_1;
    MUST(in_part_1.try_resize(last_code_point + 1));
    __builtin_memset(in_part_1.data(), 0, in_part_1.size());
    bool part_1 = false;

    for_each_line(data, [&](StringView line, size_t number) {
        auto const text = before_comment(line);
        if (text.is_empty())
            return;
        if (text[0] == '@') {
            part_1 = text == "@Part1"_sv;
            return;
        }
        auto const fields = split_on(text, ';');
        auto const c1 = parse_code_points(fields[0]);
        auto const c2 = parse_code_points(fields[1]);
        auto const c3 = parse_code_points(fields[2]);
        auto const c4 = parse_code_points(fields[3]);
        auto const c5 = parse_code_points(fields[4]);
        if (part_1 && c1.size() == 1)
            in_part_1[c1[0]] = 1;
        auto const of = [](Vector<CodePoint> const& text_in, NormalizationForm form) { return MUST(normalize(text_in.span(), form)); };
        using F = NormalizationForm;
        bool ok = same(c2, of(c1, F::NFC)) && same(c2, of(c2, F::NFC)) && same(c2, of(c3, F::NFC)) && same(c4, of(c4, F::NFC)) && same(c4, of(c5, F::NFC));
        ok = ok && same(c3, of(c1, F::NFD)) && same(c3, of(c2, F::NFD)) && same(c3, of(c3, F::NFD)) && same(c5, of(c4, F::NFD)) && same(c5, of(c5, F::NFD));
        ok = ok && same(c4, of(c1, F::NFKC)) && same(c4, of(c2, F::NFKC)) && same(c4, of(c3, F::NFKC)) && same(c4, of(c4, F::NFKC)) && same(c4, of(c5, F::NFKC));
        ok = ok && same(c5, of(c1, F::NFKD)) && same(c5, of(c2, F::NFKD)) && same(c5, of(c3, F::NFKD)) && same(c5, of(c4, F::NFKD)) && same(c5, of(c5, F::NFKD));
        tally.note(ok, number);
    });
    tally.report();
    EXPECT_EQ(tally.failed, 0u);
    EXPECT(tally.passed > 19000);

    size_t wrong = 0;
    for (CodePoint code = 0; code <= last_code_point; ++code) {
        if (in_part_1[code] || (code >= 0xd800 && code <= 0xdfff))
            continue;
        Vector<CodePoint> one;
        MUST(one.try_append(code));
        bool const ok = same(one, MUST(normalize(one.span(), NormalizationForm::NFC))) && same(one, MUST(normalize(one.span(), NormalizationForm::NFD)))
            && same(one, MUST(normalize(one.span(), NormalizationForm::NFKC))) && same(one, MUST(normalize(one.span(), NormalizationForm::NFKD)));
        if (!ok && ++wrong <= most_shown)
            printf("normalization: U+%04X should come through as it is\n", code);
    }
    EXPECT_EQ(wrong, 0u);
}

namespace {

// "÷ 0020 × 0308 ÷": the text, and where each ÷ is.
void parse_breaks(StringView text, Vector<CodePoint>& code_points, Vector<size_t>& boundaries)
{
    size_t i = 0;
    while (i < text.length()) {
        while (i < text.length() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        if (i >= text.length())
            break;
        if (static_cast<u8>(text[i]) == 0xc3 && static_cast<u8>(text[i + 1]) == 0xb7) { // ÷
            MUST(boundaries.try_append(code_points.size()));
            i += 2;
        } else if (static_cast<u8>(text[i]) == 0xc3 && static_cast<u8>(text[i + 1]) == 0x97) { // ×
            i += 2;
        } else {
            size_t const start = i;
            while (i < text.length() && text[i] != ' ' && text[i] != '\t')
                ++i;
            MUST(code_points.try_append(hex_value(text.substring_view(start, i - start))));
        }
    }
}

template<typename Find>
void check_break_file(StringView file, char const* name, Find find)
{
    Tally tally { name };
    auto const data = read_gzipped(file);
    for_each_line(data, [&](StringView line, size_t number) {
        auto const text = before_comment(line);
        if (text.is_empty())
            return;
        Vector<CodePoint> code_points;
        Vector<size_t> expected;
        parse_breaks(text, code_points, expected);
        auto const found = find(code_points);
        bool ok = found.size() == expected.size();
        for (size_t i = 0; ok && i < found.size(); ++i)
            ok = found[i] == expected[i];
        tally.note(ok, number);
    });
    tally.report();
    EXPECT_EQ(tally.failed, 0u);
    EXPECT(tally.passed > 100);
}

}

TEST_CASE(grapheme_clusters)
{
    check_break_file("GraphemeBreakTest.txt"_sv, "GraphemeBreakTest", [](Vector<CodePoint> const& text) { return MUST(grapheme_boundaries(text.span())); });
}

TEST_CASE(words)
{
    check_break_file("WordBreakTest.txt"_sv, "WordBreakTest", [](Vector<CodePoint> const& text) { return MUST(word_boundaries(text.span())); });
}

TEST_CASE(sentences)
{
    check_break_file("SentenceBreakTest.txt"_sv, "SentenceBreakTest", [](Vector<CodePoint> const& text) { return MUST(sentence_boundaries(text.span())); });
}

TEST_CASE(lines)
{
    check_break_file("LineBreakTest.txt"_sv, "LineBreakTest", [](Vector<CodePoint> const& text) {
        Vector<size_t> positions;
        for (auto const& opportunity : MUST(line_break_opportunities(text.span())))
            MUST(positions.try_append(opportunity.position));
        return positions;
    });
}

namespace {

Optional<BidiClass> bidi_class_named(StringView name)
{
    struct Named {
        StringView name;
        BidiClass value;
    };
    Named const all[] = {
        { "L"_sv, BidiClass::L }, { "R"_sv, BidiClass::R }, { "AL"_sv, BidiClass::AL }, { "EN"_sv, BidiClass::EN }, { "ES"_sv, BidiClass::ES }, { "ET"_sv, BidiClass::ET },
        { "AN"_sv, BidiClass::AN }, { "CS"_sv, BidiClass::CS }, { "NSM"_sv, BidiClass::NSM }, { "BN"_sv, BidiClass::BN }, { "B"_sv, BidiClass::B }, { "S"_sv, BidiClass::S },
        { "WS"_sv, BidiClass::WS }, { "ON"_sv, BidiClass::ON }, { "LRE"_sv, BidiClass::LRE }, { "LRO"_sv, BidiClass::LRO }, { "RLE"_sv, BidiClass::RLE }, { "RLO"_sv, BidiClass::RLO },
        { "PDF"_sv, BidiClass::PDF }, { "LRI"_sv, BidiClass::LRI }, { "RLI"_sv, BidiClass::RLI }, { "FSI"_sv, BidiClass::FSI }, { "PDI"_sv, BidiClass::PDI },
    };
    for (auto const& entry : all) {
        if (entry.name == name)
            return entry.value;
    }
    return {};
}

// BidiTest's lists start after a tab.
StringView without_tabs(StringView field)
{
    while (!field.is_empty() && field[0] == '\t')
        field = field.substring_view(1);
    return field;
}

// Levels, as "x 1 0": x is 0xff, and what's expected.
Vector<u8> parse_levels(StringView text)
{
    Vector<u8> levels;
    for (auto field : split_on(text, ' ')) {
        field = without_tabs(field);
        if (field.is_empty())
            continue;
        if (field == "x"_sv) {
            MUST(levels.try_append(bidi_level_removed));
            continue;
        }
        u8 value = 0;
        for (size_t i = 0; i < field.length(); ++i)
            value = static_cast<u8>(value * 10 + (field[i] - '0'));
        MUST(levels.try_append(value));
    }
    return levels;
}

Vector<size_t> parse_order(StringView text)
{
    Vector<size_t> order;
    for (auto field : split_on(text, ' ')) {
        field = without_tabs(field);
        if (field.is_empty())
            continue;
        size_t value = 0;
        for (size_t i = 0; i < field.length(); ++i)
            value = value * 10 + static_cast<size_t>(field[i] - '0');
        MUST(order.try_append(value));
    }
    return order;
}

template<typename A, typename B>
bool same_lists(A const& a, B const& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

}

TEST_CASE(bidi_by_classes)
{
    Tally tally { "BidiTest" };
    auto const data = read_gzipped("BidiTest.txt"_sv);
    Vector<u8> expected_levels;
    Vector<size_t> expected_order;
    for_each_line(data, [&](StringView line, size_t number) {
        auto const text = before_comment(line);
        if (text.is_empty())
            return;
        if (text.starts_with("@Levels:"_sv)) {
            expected_levels = parse_levels(text.substring_view(8));
            return;
        }
        if (text.starts_with("@Reorder:"_sv)) {
            expected_order = parse_order(text.substring_view(9));
            return;
        }
        if (text[0] == '@')
            return;
        auto const fields = split_on(text, ';');
        Vector<BidiClass> classes;
        for (auto const name : split_on(fields[0], ' ')) {
            if (!name.is_empty())
                MUST(classes.try_append(bidi_class_named(name).value()));
        }
        unsigned bits = 0;
        for (size_t i = 0; i < fields[1].length(); ++i) {
            if (fields[1][i] != ' ')
                bits = bits * 10 + static_cast<unsigned>(fields[1][i] - '0');
        }
        ParagraphDirection const directions[] = { ParagraphDirection::Auto, ParagraphDirection::LeftToRight, ParagraphDirection::RightToLeft };
        for (unsigned which = 0; which < 3; ++which) {
            if (!(bits & (1u << which)))
                continue;
            auto const paragraph = MUST(resolve_bidi(classes.span(), directions[which]));
            auto const levels = MUST(reordering_levels(paragraph, 0, classes.size()));
            auto const order = MUST(visual_order(levels.span()));
            tally.note(same_lists(levels, expected_levels) && same_lists(order, expected_order), number);
        }
    });
    tally.report();
    EXPECT_EQ(tally.failed, 0u);
    EXPECT(tally.passed > 500000);
}

TEST_CASE(bidi_by_characters)
{
    Tally tally { "BidiCharacterTest" };
    auto const data = read_gzipped("BidiCharacterTest.txt"_sv);
    for_each_line(data, [&](StringView line, size_t number) {
        auto const text = before_comment(line);
        if (text.is_empty())
            return;
        auto const fields = split_on(text, ';');
        auto const code_points = parse_code_points(fields[0]);
        auto const direction = fields[1] == "0"_sv ? ParagraphDirection::LeftToRight : (fields[1] == "1"_sv ? ParagraphDirection::RightToLeft : ParagraphDirection::Auto);
        auto const paragraph = MUST(resolve_bidi(code_points.span(), direction));
        auto const levels = MUST(reordering_levels(paragraph, 0, code_points.size()));
        auto const order = MUST(visual_order(levels.span()));
        bool const ok = paragraph.paragraph_level == static_cast<u8>(fields[2][0] - '0') && same_lists(levels, parse_levels(fields[3])) && same_lists(order, parse_order(fields[4]));
        tally.note(ok, number);
    });
    tally.report();
    EXPECT_EQ(tally.failed, 0u);
    EXPECT(tally.passed > 90000);
}
