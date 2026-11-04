/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Shaper.h>

namespace Text {

using Unicode::CodePoint;
using Unicode::GeneralCategory;
using Unicode::Script;

namespace Shaping {

Plan::Plan(OpenTypeLayout const& gsub, OpenTypeLayout const& gpos)
    : m_gsub(gsub)
    , m_gpos(gpos)
{
    m_stages.append(Stage {});
}

u32 Plan::mask_for(u32 feature)
{
    for (size_t i = 0; i < m_named_features.size(); ++i) {
        if (m_named_features[i] == feature)
            return m_named_masks[i];
    }
    u32 const mask = m_next_bit;
    // (There's a limit, and if it's reached the rest apply everywhere, which is better than nowhere.)
    m_next_bit = m_next_bit < 0x80000000 ? m_next_bit << 1 : m_next_bit;
    m_named_features.append(feature);
    m_named_masks.append(mask);
    return mask;
}

void Plan::add(u32 feature, u32 mask, bool joiners_count)
{
    auto& stage = m_stages.last();
    stage.features.append(feature);
    stage.masks.append(mask);
    stage.joiners_count.append(joiners_count);
}

void Plan::pause(Function<void(ShapingBuffer&)> after)
{
    m_stages.last().after = move(after);
    m_stages.append(Stage {});
}

void Plan::add_position(u32 feature)
{
    m_position_features.append(feature);
}

namespace {

// The lookups of these features, each with which glyphs it's for, in the order the font lists its lookups in: which is what decides what happens first, not the order of the features.
struct LookupToRun {
    u16 lookup;
    u32 mask;
    bool joiners_count;
};

void gather(OpenTypeLayout const& layout, Span<u32 const> features, Span<u32 const> masks, Span<bool const> joiners_count, Vector<LookupToRun>& out)
{
    for (size_t i = 0; i < features.size(); ++i) {
        Vector<u16> lookups;
        layout.collect_lookups(features[i], lookups);
        for (u16 lookup : lookups) {
            bool merged = false;
            for (auto& existing : out) {
                if (existing.lookup == lookup) {
                    existing.mask |= masks[i];
                    merged = true;
                    break;
                }
            }
            if (!merged)
                out.append({ lookup, masks[i], joiners_count[i] });
        }
    }
    for (size_t i = 1; i < out.size(); ++i) {
        auto const held = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].lookup > held.lookup) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = held;
    }
}

}

void Plan::substitute(ShapingBuffer& buffer, bool is_right_to_left) const
{
    for (auto const& stage : m_stages) {
        Vector<LookupToRun> lookups;
        gather(m_gsub, stage.features.span(), stage.masks.span(), stage.joiners_count.span(), lookups);
        for (auto const& entry : lookups)
            m_gsub.apply_lookup(entry.lookup, buffer, entry.mask, is_right_to_left, entry.joiners_count);
        if (stage.after)
            stage.after(buffer);
    }
}

void Plan::position(ShapingBuffer& buffer, bool is_right_to_left) const
{
    Vector<u32> masks;
    Vector<bool> joiners_count;
    for (size_t i = 0; i < m_position_features.size(); ++i) {
        masks.append(global_mask);
        joiners_count.append(false);
    }
    Vector<LookupToRun> lookups;
    gather(m_gpos, m_position_features.span(), masks.span(), joiners_count.span(), lookups);
    for (auto const& entry : lookups)
        m_gpos.apply_lookup(entry.lookup, buffer, entry.mask, is_right_to_left, false);
}

}

namespace {

using Shaping::Plan;

bool is_arabic_family(Script script)
{
    return script == Script::Arabic || script == Script::Syriac || script == Script::Mandaic || script == Script::Manichaean || script == Script::Mongolian || script == Script::Nko
        || script == Script::Phags_Pa || script == Script::Psalter_Pahlavi || script == Script::Adlam;
}

bool is_indic(Script script)
{
    switch (script) {
    case Script::Devanagari:
    case Script::Bengali:
    case Script::Gurmukhi:
    case Script::Gujarati:
    case Script::Oriya:
    case Script::Tamil:
    case Script::Telugu:
    case Script::Kannada:
    case Script::Malayalam:
        return true;
    default:
        return false;
    }
}

// OpenType's name for a script. Some have an old one and a new one, where the newer means the font's written for how shaping's done now.
LayoutScript layout_script_of(Script script, u32 language)
{
    LayoutScript result;
    result.language = language;
    auto const say = [&](char const (&first)[5], char const (&second)[5]) {
        result.tags[0] = feature_tag(first);
        result.tags[1] = feature_tag(second);
    };
    switch (script) {
    case Script::Arabic:
        say("arab", "arab");
        break;
    case Script::Syriac:
        say("syrc", "syrc");
        break;
    case Script::Hebrew:
        say("hebr", "hebr");
        break;
    case Script::Latin:
        say("latn", "latn");
        break;
    case Script::Greek:
        say("grek", "grek");
        break;
    case Script::Cyrillic:
        say("cyrl", "cyrl");
        break;
    case Script::Armenian:
        say("armn", "armn");
        break;
    case Script::Georgian:
        say("geor", "geor");
        break;
    case Script::Devanagari:
        say("dev2", "deva");
        break;
    case Script::Bengali:
        say("bng2", "beng");
        break;
    case Script::Gurmukhi:
        say("gur2", "guru");
        break;
    case Script::Gujarati:
        say("gjr2", "gujr");
        break;
    case Script::Oriya:
        say("ory2", "orya");
        break;
    case Script::Tamil:
        say("tml2", "taml");
        break;
    case Script::Telugu:
        say("tel2", "telu");
        break;
    case Script::Kannada:
        say("knd2", "knda");
        break;
    case Script::Malayalam:
        say("mlm2", "mlym");
        break;
    case Script::Thai:
        say("thai", "thai");
        break;
    case Script::Lao:
        say("lao ", "lao ");
        break;
    case Script::Hangul:
        say("hang", "hang");
        break;
    case Script::Hiragana:
    case Script::Katakana:
        say("kana", "kana");
        break;
    case Script::Han:
        say("hani", "hani");
        break;
    case Script::Khmer:
        say("khmr", "khmr");
        break;
    case Script::Myanmar:
        say("mym2", "mymr");
        break;
    case Script::Sinhala:
        say("sinh", "sinh");
        break;
    case Script::Tibetan:
        say("tibt", "tibt");
        break;
    default:
        break;
    }
    return result;
}

bool is_mark_category(GeneralCategory category)
{
    return category == GeneralCategory::Mn || category == GeneralCategory::Mc || category == GeneralCategory::Me;
}

// The order marks are put in for a font. Unicode's classes are only for telling whether two texts are the same, and aren't the order fonts expect: Hebrew's points go in the order that's
// been agreed for typesetting it, and Arabic's shadda goes first, as other vowel marks sit on it.
u8 mark_order(CodePoint code)
{
    u8 const value = Unicode::combining_class(code);
    if (value >= 10 && value <= 26) {
        constexpr u8 hebrew[] = { 22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25, 13, 10, 11, 26 };
        return hebrew[value - 10];
    }
    if (value >= 27 && value <= 33) {
        constexpr u8 arabic[] = { 28, 29, 30, 31, 32, 33, 27 };
        return arabic[value - 27];
    }
    switch (value) {
    case 103: // Thai sara u and uu come next to what they're on.
        return 3;
    case 130: // Tibetan.
        return 132;
    case 132:
        return 131;
    default:
        return value;
    }
}

// Arabic also has marks that have to be right against the letter, whatever their class says (UAX #53).
bool is_arabic_modifier_mark(CodePoint code)
{
    switch (code) {
    case 0x0654:
    case 0x0655:
    case 0x0658:
    case 0x06dc:
    case 0x06e3:
    case 0x06e7:
    case 0x06e8:
    case 0x08ca:
    case 0x08cb:
    case 0x08cd:
    case 0x08ce:
    case 0x08d3:
    case 0x08f3:
        return true;
    default:
        return false;
    }
}

void add_glyph(ShapingBuffer& buffer, CodePoint code, u32 cluster)
{
    ShapingGlyph glyph;
    glyph.code_point = code;
    glyph.cluster = cluster;
    glyph.mask = Plan::global_mask;
    glyph.is_zwj = code == 0x200d;
    glyph.is_zwnj = code == 0x200c;
    buffer.append(glyph);
}

// Text as the font has it: taken apart, marks put in order, and then put back together as far as the font has what that makes.
void normalize_for_font(Typeface const& typeface, Span<CodePoint const> text, Script script, ShapingBuffer& buffer)
{
    auto const has_glyph = [&](CodePoint code) { return typeface.glyph_for_code_point(code) != 0; };
    bool const is_indic_script = is_indic(script);

    for (size_t i = 0; i < text.size(); ++i) {
        CodePoint const code = text[i];
        bool const followed_by_mark = i + 1 < text.size() && is_mark_category(Unicode::general_category(text[i + 1]));
        // A vowel sign that goes on both sides of its consonant is two signs, and is treated that way, as it's only as two that one of them can move.
        bool const is_split_vowel = is_indic_script && Unicode::indic_syllabic_category_of(code) == Unicode::IndicSyllabicCategory::Vowel_Dependent;
        // Left alone if the font has it, and nothing's going to want to combine with it. (Some can't be got back by composing, and a font may have those as they are.)
        if (has_glyph(code) && !followed_by_mark && !is_split_vowel) {
            add_glyph(buffer, code, static_cast<u32>(i));
            continue;
        }
        Vector<CodePoint> parts;
        MUST(Unicode::append_canonical_decomposition(parts, code));
        bool all_there = true;
        for (auto part : parts)
            all_there = all_there && has_glyph(part);
        if (parts.size() > 1 && !all_there && has_glyph(code)) {
            add_glyph(buffer, code, static_cast<u32>(i));
            continue;
        }
        for (auto part : parts)
            add_glyph(buffer, part, static_cast<u32>(i));
    }

    // Marks in order.
    for (size_t i = 0; i < buffer.size();) {
        if (Unicode::combining_class(buffer[i].code_point) == 0) {
            ++i;
            continue;
        }
        size_t const start = i;
        while (i < buffer.size() && Unicode::combining_class(buffer[i].code_point) != 0)
            ++i;
        for (size_t a = start + 1; a < i; ++a) {
            auto const held = buffer[a];
            u8 const held_class = mark_order(held.code_point);
            size_t b = a;
            while (b > start && mark_order(buffer[b - 1].code_point) > held_class) {
                buffer[b] = buffer[b - 1];
                --b;
            }
            buffer[b] = held;
        }
        if (script == Script::Arabic) {
            // Modifier marks to the front of their own class, and those of class 230 ahead of what's in 220.
            size_t place = start;
            u8 const classes[] = { 220, 230 };
            for (u8 wanted : classes) {
                for (size_t a = start; a < i; ++a) {
                    if (Unicode::combining_class(buffer[a].code_point) == wanted && is_arabic_modifier_mark(buffer[a].code_point)) {
                        auto const held = buffer[a];
                        for (size_t b = a; b > place; --b)
                            buffer[b] = buffer[b - 1];
                        buffer[place++] = held;
                    }
                }
            }
        }
    }

    // Together again, where the font would rather.
    if (buffer.is_empty())
        return;
    size_t written = 0;
    size_t starter = buffer.size();
    i32 last_class = -1;
    for (size_t i = 0; i < buffer.size(); ++i) {
        auto const glyph = buffer[i];
        u8 const current_class = Unicode::combining_class(glyph.code_point);
        bool const blocked = last_class != -1 && last_class >= current_class;
        // (In Indic scripts, only nuktas. Vowel signs that have been split up are to stay that way.)
        bool const may_compose = !is_indic_script || current_class == 7;
        if (starter != buffer.size() && !blocked && may_compose) {
            CodePoint const composed = Unicode::compose_pair(buffer[starter].code_point, glyph.code_point);
            if (composed != 0 && has_glyph(composed)) {
                buffer[starter].code_point = composed;
                buffer[starter].cluster = min(buffer[starter].cluster, glyph.cluster);
                continue;
            }
        }
        if (current_class == 0) {
            starter = written;
            last_class = -1;
        } else {
            last_class = current_class;
        }
        buffer[written++] = glyph;
    }
    MUST(buffer.try_resize(written));
}

// What each character looks like, in this font.
void map_to_glyphs(Typeface const& typeface, ShapingBuffer& buffer, bool is_right_to_left)
{
    GlyphIndex const space = typeface.glyph_for_code_point(' ');
    size_t written = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        auto glyph = buffer[i];
        CodePoint code = glyph.code_point;
        // Selectors say which of a character's looks is wanted, and aren't there themselves.
        if (i + 1 < buffer.size()) {
            CodePoint const next = buffer[i + 1].code_point;
            bool const is_selector = (next >= 0xfe00 && next <= 0xfe0f) || (next >= 0xe0100 && next <= 0xe01ef);
            if (is_selector) {
                if (auto const varied = typeface.glyph_for_variation(code, next)) {
                    glyph.glyph = varied;
                    buffer[written++] = glyph;
                    ++i;
                    continue;
                }
            }
        }
        if (is_right_to_left) {
            CodePoint const mirrored = Unicode::bidi_mirrored(code);
            if (mirrored != code && typeface.glyph_for_code_point(mirrored) != 0)
                code = mirrored;
        }
        glyph.glyph = typeface.glyph_for_code_point(code);
        if (glyph.glyph == 0) {
            if (code == 0x2011)
                glyph.glyph = typeface.glyph_for_code_point(0x2010);
            else if (code == 0x00a0 || code == 0x202f || (code >= 0x2000 && code <= 0x200a))
                glyph.glyph = space;
        }
        glyph.is_hidden = Unicode::is_default_ignorable(glyph.code_point);
        buffer[written++] = glyph;
    }
    MUST(buffer.try_resize(written));
}

// Which are marks, if the font hasn't said.
void classify_by_unicode(ShapingBuffer& buffer)
{
    for (auto& glyph : buffer) {
        auto const category = Unicode::general_category(glyph.code_point);
        glyph.glyph_class = is_mark_category(category) ? ShapingGlyph::Mark : ShapingGlyph::Base;
    }
}

// Thai and Lao: sara am is two things, the part that goes above and the vowel after, and the first goes before any tone mark.
void prepare_thai_or_lao(ShapingBuffer& buffer, Script script)
{
    CodePoint const sara_am = script == Script::Thai ? 0x0e33 : 0x0eb3;
    CodePoint const nikhahit = script == Script::Thai ? 0x0e4d : 0x0ecd;
    CodePoint const aa = script == Script::Thai ? 0x0e32 : 0x0eb2;
    auto const is_above_mark = [&](CodePoint code) {
        if (script == Script::Thai)
            return (code >= 0x0e34 && code <= 0x0e37) || code == 0x0e31 || (code >= 0x0e47 && code <= 0x0e4e);
        return (code >= 0x0eb4 && code <= 0x0eb7) || code == 0x0eb1 || code == 0x0ebb || (code >= 0x0ec8 && code <= 0x0ecd);
    };
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i].code_point != sara_am)
            continue;
        auto first = buffer[i];
        first.code_point = nikhahit;
        buffer[i].code_point = aa;
        // Back past tone marks.
        size_t place = i;
        while (place > 0 && is_above_mark(buffer[place - 1].code_point))
            --place;
        buffer.insert(place, first);
        ++i;
    }
}

// Each ends up with its advance from the font, and a mark on something else has none.
void set_advances(Typeface const& typeface, ShapingBuffer& buffer)
{
    for (auto& glyph : buffer) {
        glyph.x_advance = typeface.advance_of(glyph.glyph);
        glyph.y_advance = 0;
    }
}

void zero_mark_advances(ShapingBuffer& buffer)
{
    for (auto& glyph : buffer) {
        if (glyph.glyph_class == ShapingGlyph::Mark) {
            glyph.x_advance = 0;
            glyph.y_advance = 0;
        }
    }
}

// Where what's attached to something ends up: its offset from there, wherever that's got to.
void resolve_attachments(ShapingBuffer& buffer, bool is_right_to_left)
{
    Vector<bool> done;
    MUST(done.try_resize(buffer.size()));
    for (size_t i = 0; i < buffer.size(); ++i)
        done[i] = false;

    auto const resolve = [&](auto const& self, size_t i, int depth) -> void {
        if (done[i] || depth > 64)
            return;
        done[i] = true;
        auto& glyph = buffer[i];
        if (glyph.attachment == ShapingGlyph::NotAttached || glyph.attached_to == 0)
            return;
        i64 const target_index = static_cast<i64>(i) + glyph.attached_to;
        if (target_index < 0 || target_index >= static_cast<i64>(buffer.size()))
            return;
        auto const target_at = static_cast<size_t>(target_index);
        self(self, target_at, depth + 1);
        auto const& target = buffer[target_at];
        if (glyph.attachment == ShapingGlyph::Cursive) {
            glyph.y_offset += target.y_offset;
            return;
        }
        // A mark: where its target is, and across whatever advances are between them. (In logical order; the other way, the advances are behind it.)
        glyph.x_offset += target.x_offset;
        glyph.y_offset += target.y_offset;
        if (is_right_to_left) {
            // In the order they're drawn, what's later in the text comes first: from a mark's pen to its base's is whatever's after the base, up to and including the mark.
            if (target_at < i) {
                for (size_t k = target_at + 1; k <= i; ++k)
                    glyph.x_offset += buffer[k].x_advance;
            } else {
                for (size_t k = i + 1; k <= target_at; ++k)
                    glyph.x_offset -= buffer[k].x_advance;
            }
        } else {
            if (target_at < i) {
                for (size_t k = target_at; k < i; ++k)
                    glyph.x_offset -= buffer[k].x_advance;
            } else {
                for (size_t k = i; k < target_at; ++k)
                    glyph.x_offset += buffer[k].x_advance;
            }
        }
    };
    for (size_t i = 0; i < buffer.size(); ++i)
        resolve(resolve, i, 0);
}

}

ErrorOr<Vector<ShapedGlyph>> shape(Typeface const& typeface, ShapeRequest const& request)
{
    Vector<ShapedGlyph> result;
    if (request.text.is_empty())
        return result;

    OpenTypeLayout gsub(typeface, OpenTypeLayout::Table::GSUB);
    OpenTypeLayout gpos(typeface, OpenTypeLayout::Table::GPOS);
    LayoutScript const wanted = layout_script_of(request.script, request.language);
    (void)gsub.select_script(wanted);
    (void)gpos.select_script(wanted);

    ShapingBuffer buffer;
    TRY(buffer.try_ensure_capacity(request.text.size() + 8));
    normalize_for_font(typeface, request.text, request.script, buffer);
    if (request.script == Script::Thai || request.script == Script::Lao)
        prepare_thai_or_lao(buffer, request.script);
    map_to_glyphs(typeface, buffer, request.is_right_to_left);

    if (gsub.has_glyph_classes() || gpos.has_glyph_classes())
        gsub.classify(buffer);
    else
        classify_by_unicode(buffer);

    // Everything's asked for by what it's called: a font only does what it has.
    Plan plan(gsub, gpos);
    if (request.is_right_to_left) {
        plan.add_global(feature_tag("rtla"));
        plan.add_global(feature_tag("rtlm"));
    } else {
        plan.add_global(feature_tag("ltra"));
        plan.add_global(feature_tag("ltrm"));
    }
    plan.pause();
    plan.add_global(feature_tag("ccmp"));
    plan.add_global(feature_tag("locl"));
    plan.pause();
    bool const is_arabic = is_arabic_family(request.script);
    if (is_arabic) {
        Shaping::prepare_arabic(buffer, plan, request.script == Script::Syriac);
    } else if (is_indic(request.script)) {
        Shaping::prepare_indic(typeface, gsub, buffer, plan, request.script);
    } else if (request.script == Script::Hangul) {
        Shaping::prepare_hangul(buffer, plan);
    }

    // What every script has, after its own.
    plan.add(feature_tag("rlig"), Plan::global_mask, is_arabic);
    if (is_arabic)
        plan.pause();
    plan.add(feature_tag("rclt"), Plan::global_mask, is_arabic);
    // (Contextual alternates are only for scripts that need them to be written at all. Elsewhere they're a choice for the reader, and aren't on by default: as on a Mac.)
    if (is_arabic || is_indic(request.script))
        plan.add(feature_tag("calt"), Plan::global_mask, is_arabic);
    if (is_arabic)
        plan.pause();
    plan.add_global(feature_tag("clig"));
    plan.add_global(feature_tag("liga"));
    plan.substitute(buffer, request.is_right_to_left);

    set_advances(typeface, buffer);
    bool const has_positions = gpos.exists();

    // Spaces and what isn't there take up nothing, once a font's said all it's going to about them.
    plan.add_position(feature_tag("abvm"));
    plan.add_position(feature_tag("blwm"));
    plan.add_position(feature_tag("curs"));
    plan.add_position(feature_tag("dist"));
    plan.add_position(feature_tag("kern"));
    plan.add_position(feature_tag("mark"));
    plan.add_position(feature_tag("mkmk"));
    plan.add_position(feature_tag("abvs"));

    // Marks take up no room, unless the font's said so itself by giving them some after this. In Indic scripts, what a font's given them is what it means: many vowel signs are marks and have width.
    if (!is_indic(request.script))
        zero_mark_advances(buffer);
    if (has_positions)
        plan.position(buffer, request.is_right_to_left);

    resolve_attachments(buffer, request.is_right_to_left);

    if (request.is_right_to_left) {
        for (size_t a = 0, b = buffer.size(); a + 1 < b; ++a, --b) {
            auto const held = buffer[a];
            buffer[a] = buffer[b - 1];
            buffer[b - 1] = held;
        }
    }

    TRY(result.try_ensure_capacity(buffer.size()));
    GlyphIndex const space = typeface.glyph_for_code_point(' ');
    for (auto const& glyph : buffer) {
        // What isn't to be seen is still there, for its place in the text: a space with no room.
        if (glyph.is_hidden) {
            TRY(result.try_append(ShapedGlyph { space, glyph.cluster, 0, 0, 0, 0 }));
            continue;
        }
        TRY(result.try_append(ShapedGlyph { glyph.glyph, glyph.cluster, glyph.x_advance, glyph.y_advance, glyph.x_offset, glyph.y_offset }));
    }
    return result;
}

}
