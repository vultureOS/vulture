/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Shaper.h>

// The scripts of India, which are written by the syllable: a consonant (the base) with the ones before it reduced to half-forms, those after it to forms below or beside it, and vowels
// that go wherever they go, some of them before the whole lot though they come after in the text. Ra at the start usually turns into a mark on top ("reph") and moves to the end.
// So the glyphs of each syllable are put in the order they'll be drawn, then features make the forms, and then a second pass moves what can only be placed once the forms are known.
//
// This follows Microsoft's "Developing OpenType Fonts for Indic Scripts".

namespace Text::Shaping {

using Unicode::CodePoint;
using Unicode::IndicPositionalCategory;
using Unicode::IndicSyllabicCategory;
using Unicode::Script;

namespace {

enum Category : u8 {
    X,
    C,
    V,
    N,
    H,
    ZWNJ,
    ZWJ,
    M,
    SM,
    A,
    VD,
    PLACEHOLDER,
    DOTTED_CIRCLE,
    RS,
    REPHA,
    RA,
    CM,
    SYMBOL,
    CS,
};

enum Position : u8 {
    START,
    RA_TO_BECOME_REPH,
    PRE_M,
    PRE_C,
    BASE_C,
    AFTER_MAIN,
    ABOVE_C,
    BEFORE_SUB,
    BELOW_C,
    AFTER_SUB,
    BEFORE_POST,
    POST_C,
    AFTER_POST,
    SMVD,
    END,
};

enum class RephMode : u8 {
    Implicit, // Ra + halant, unless ZWJ's in the way.
    Explicit, // Ra + halant + ZWJ.
    LogRepha, // Encoded as a letter of its own.
};

enum class BelowFormMode : u8 {
    PreAndPost,
    PostOnly,
};

struct Config {
    Script script;
    CodePoint virama;
    Position reph_position;
    RephMode reph_mode;
    BelowFormMode below_form_mode;
};

Config const s_configs[] = {
    { Script::Devanagari, 0x094d, BEFORE_POST, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Bengali, 0x09cd, AFTER_SUB, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Gurmukhi, 0x0a4d, BEFORE_SUB, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Gujarati, 0x0acd, BEFORE_POST, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Oriya, 0x0b4d, AFTER_MAIN, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Tamil, 0x0bcd, AFTER_POST, RephMode::Implicit, BelowFormMode::PreAndPost },
    { Script::Telugu, 0x0c4d, AFTER_POST, RephMode::Explicit, BelowFormMode::PostOnly },
    { Script::Kannada, 0x0ccd, AFTER_POST, RephMode::Implicit, BelowFormMode::PostOnly },
    { Script::Malayalam, 0x0d4d, AFTER_MAIN, RephMode::LogRepha, BelowFormMode::PreAndPost },
};

bool is_ra(CodePoint code)
{
    switch (code) {
    case 0x0930: // Devanagari
    case 0x09b0: // Bengali
    case 0x09f0: // Assamese
    case 0x0a30: // Gurmukhi: which doesn't have a reph, but has a below form
    case 0x0ab0: // Gujarati
    case 0x0b30: // Oriya
    case 0x0bb0: // Tamil
    case 0x0c30: // Telugu
    case 0x0cb0: // Kannada
    case 0x0d30: // Malayalam
        return true;
    default:
        return false;
    }
}

Category category_of(CodePoint code)
{
    switch (code) {
    case 0x200c:
        return ZWNJ;
    case 0x200d:
        return ZWJ;
    case 0x25cc:
        return DOTTED_CIRCLE;
    default:
        break;
    }
    switch (Unicode::indic_syllabic_category_of(code)) {
    case IndicSyllabicCategory::Consonant:
    case IndicSyllabicCategory::Consonant_Dead:
    case IndicSyllabicCategory::Consonant_Head_Letter:
    case IndicSyllabicCategory::Consonant_With_Stacker:
        return is_ra(code) ? RA : C;
    case IndicSyllabicCategory::Consonant_Placeholder:
        return PLACEHOLDER;
    case IndicSyllabicCategory::Consonant_Preceding_Repha:
    case IndicSyllabicCategory::Consonant_Succeeding_Repha:
        return REPHA;
    case IndicSyllabicCategory::Consonant_Medial:
        return CM;
    case IndicSyllabicCategory::Vowel_Independent:
        return V;
    case IndicSyllabicCategory::Vowel_Dependent:
    case IndicSyllabicCategory::Pure_Killer:
        return M;
    case IndicSyllabicCategory::Nukta:
        return N;
    case IndicSyllabicCategory::Virama:
    case IndicSyllabicCategory::Invisible_Stacker:
        return H;
    case IndicSyllabicCategory::Bindu:
    case IndicSyllabicCategory::Visarga:
    case IndicSyllabicCategory::Syllable_Modifier:
    case IndicSyllabicCategory::Gemination_Mark:
        return SM;
    case IndicSyllabicCategory::Cantillation_Mark:
        return A;
    case IndicSyllabicCategory::Tone_Mark:
        return VD;
    case IndicSyllabicCategory::Register_Shifter:
        return RS;
    case IndicSyllabicCategory::Avagraha:
    case IndicSyllabicCategory::Number:
        return SYMBOL;
    default:
        return X;
    }
}

// Where a vowel sign goes, once it's been read: this differs by script, and is how each script's fonts expect it.
Position matra_position(CodePoint code, Script script)
{
    switch (Unicode::indic_positional_category_of(code)) {
    case IndicPositionalCategory::Left:
    case IndicPositionalCategory::Visual_Order_Left:
    case IndicPositionalCategory::Top_And_Left:
    case IndicPositionalCategory::Left_And_Right:
    case IndicPositionalCategory::Top_And_Left_And_Right:
    case IndicPositionalCategory::Top_And_Bottom_And_Left:
    case IndicPositionalCategory::Bottom_And_Left:
        return PRE_M;
    case IndicPositionalCategory::Right:
    case IndicPositionalCategory::Top_And_Right:
    case IndicPositionalCategory::Bottom_And_Right:
    case IndicPositionalCategory::Top_And_Bottom_And_Right:
        switch (script) {
        case Script::Devanagari:
            return AFTER_SUB;
        case Script::Telugu:
            return code <= 0x0c42 ? BEFORE_SUB : AFTER_SUB;
        case Script::Kannada:
            return code < 0x0cc3 || code > 0x0cd6 ? BEFORE_SUB : AFTER_SUB;
        default:
            return AFTER_POST;
        }
    case IndicPositionalCategory::Top:
    case IndicPositionalCategory::Top_And_Bottom:
        switch (script) {
        case Script::Devanagari:
        case Script::Bengali:
        case Script::Malayalam:
            return AFTER_SUB;
        case Script::Telugu:
        case Script::Kannada:
            return BEFORE_SUB;
        default:
            return AFTER_POST;
        }
    case IndicPositionalCategory::Bottom:
        switch (script) {
        case Script::Devanagari:
        case Script::Bengali:
        case Script::Oriya:
            return AFTER_SUB;
        case Script::Telugu:
        case Script::Kannada:
            return BEFORE_SUB;
        default:
            return AFTER_POST;
        }
    default:
        return AFTER_POST;
    }
}

bool is_joiner(u8 category)
{
    return category == ZWJ || category == ZWNJ;
}

bool is_consonant(u8 category)
{
    return category == C || category == RA || category == CM || category == V || category == PLACEHOLDER || category == DOTTED_CIRCLE || category == REPHA || category == CS;
}

// ---- Which is which syllable. As a parser over the categories, by the grammar in the document this follows.

class SyllableParser {
public:
    enum class Kind : u8 {
        Consonant,
        Vowel,
        Standalone,
        Symbol,
        Broken,
        Other,
    };

    explicit SyllableParser(ShapingBuffer const& buffer)
        : m_buffer(buffer)
    {
    }

    // How long the syllable starting here is, and what it is.
    size_t parse(size_t start, Kind& kind)
    {
        m_at = start;
        auto const attempt = [&](auto rule, Kind what) -> bool {
            m_at = start;
            if (!(this->*rule)())
                return false;
            kind = what;
            return true;
        };
        if (attempt(&SyllableParser::consonant_syllable, Kind::Consonant))
            return m_at - start;
        if (attempt(&SyllableParser::vowel_syllable, Kind::Vowel))
            return m_at - start;
        if (attempt(&SyllableParser::standalone_syllable, Kind::Standalone))
            return m_at - start;
        if (attempt(&SyllableParser::symbol_syllable, Kind::Symbol))
            return m_at - start;
        if (attempt(&SyllableParser::broken_syllable, Kind::Broken) && m_at > start)
            return m_at - start;
        kind = Kind::Other;
        return 1;
    }

private:
    [[nodiscard]] u8 peek(size_t ahead = 0) const
    {
        return m_at + ahead < m_buffer.size() ? m_buffer[m_at + ahead].shaping_category : 255;
    }

    bool accept(u8 category)
    {
        if (peek() == category) {
            ++m_at;
            return true;
        }
        return false;
    }

    void optional_nuktas()
    {
        accept(RS);
        while (accept(N)) { }
    }

    bool consonant()
    {
        if (!accept(C) && !accept(RA))
            return false;
        // ZWJ? N*
        if (peek() == ZWJ && peek(1) == N)
            ++m_at;
        optional_nuktas();
        return true;
    }

    bool halant_group()
    {
        size_t const saved = m_at;
        if (is_joiner(peek()) && peek(1) == H)
            ++m_at;
        if (!accept(H)) {
            m_at = saved;
            return false;
        }
        if (peek() == ZWJ && peek(1) == N) {
            m_at += 2;
        } else if (peek() == ZWJ) {
            ++m_at;
        }
        return true;
    }

    void matra_group()
    {
        size_t const saved = m_at;
        while (is_joiner(peek()))
            ++m_at;
        if (!accept(M)) {
            m_at = saved;
            return;
        }
        optional_nuktas();
        if (peek() == H) {
            ++m_at;
        } else if (peek() == ZWJ && peek(1) == H && peek(2) == ZWJ && peek(3) == RA) {
            m_at += 4;
        }
    }

    void tail()
    {
        size_t saved = m_at;
        if (is_joiner(peek()) && peek(1) == SM)
            ++m_at;
        if (accept(SM)) {
            accept(SM);
            accept(ZWNJ);
        } else {
            m_at = saved;
        }
        while (accept(A) || accept(VD)) { }
        saved = m_at;
    }

    void after_consonants()
    {
        accept(CM);
        // A halant left at the end, or some vowel signs.
        size_t const saved = m_at;
        if (peek() == H && peek(1) == ZWNJ) {
            m_at += 2;
        } else if (!halant_group_then_nothing()) {
            m_at = saved;
            if (peek() == H && peek(1) == ZWJ)
                m_at += 2;
            for (int i = 0; i < 4; ++i) {
                size_t const before = m_at;
                matra_group();
                if (m_at == before)
                    break;
            }
        }
        tail();
    }

    bool halant_group_then_nothing()
    {
        // A halant that isn't before a consonant: that's what's left by now.
        return halant_group();
    }

    void reph()
    {
        if (peek() == RA && peek(1) == H) {
            m_at += 2;
        } else if (peek() == REPHA) {
            ++m_at;
        }
    }

    bool consonant_syllable()
    {
        if (peek() == REPHA || peek() == CS)
            ++m_at;
        if (!consonant())
            return false;
        // As many more as are joined by a halant.
        for (int i = 0; i < 8; ++i) {
            size_t const saved = m_at;
            if (!halant_group() || !consonant()) {
                m_at = saved;
                break;
            }
        }
        after_consonants();
        return true;
    }

    bool vowel_syllable()
    {
        size_t const saved = m_at;
        reph();
        if (!accept(V)) {
            m_at = saved;
            return false;
        }
        optional_nuktas();
        if (peek() == ZWJ && peek(1) != H) {
            ++m_at;
            return true;
        }
        for (int i = 0; i < 8; ++i) {
            size_t const held = m_at;
            if (!halant_group() || !consonant()) {
                m_at = held;
                break;
            }
        }
        after_consonants();
        return true;
    }

    bool standalone_syllable()
    {
        size_t const saved = m_at;
        if (peek() == REPHA || peek() == CS)
            ++m_at;
        else
            reph();
        if (!accept(PLACEHOLDER) && !accept(DOTTED_CIRCLE)) {
            m_at = saved;
            return false;
        }
        optional_nuktas();
        for (int i = 0; i < 8; ++i) {
            size_t const held = m_at;
            if (!halant_group() || !consonant()) {
                m_at = held;
                break;
            }
        }
        after_consonants();
        return true;
    }

    bool symbol_syllable()
    {
        if (!accept(SYMBOL))
            return false;
        optional_nuktas();
        tail();
        return true;
    }

    bool broken_syllable()
    {
        size_t const saved = m_at;
        reph();
        optional_nuktas();
        for (int i = 0; i < 8; ++i) {
            size_t const held = m_at;
            if (!halant_group() || !consonant()) {
                m_at = held;
                break;
            }
        }
        after_consonants();
        return m_at > saved;
    }

    ShapingBuffer const& m_buffer;
    size_t m_at { 0 };
};

// ---- Asking a font.

class FormFinder {
public:
    FormFinder(OpenTypeLayout const& gsub, GlyphIndex virama)
        : m_gsub(gsub)
        , m_virama(virama)
    {
    }

    // Whether this feature would make something of these two.
    [[nodiscard]] bool would_substitute(u32 feature, GlyphIndex first, GlyphIndex second) const
    {
        Vector<u16> lookups;
        m_gsub.collect_lookups(feature, lookups);
        if (lookups.is_empty())
            return false;
        ShapingBuffer probe;
        ShapingGlyph a;
        a.glyph = first;
        a.mask = 1;
        a.glyph_class = ShapingGlyph::Base;
        ShapingGlyph b = a;
        b.glyph = second;
        probe.append(a);
        probe.append(b);
        m_gsub.classify(probe);
        for (u16 lookup : lookups)
            m_gsub.apply_lookup(lookup, probe, 1, false);
        return probe.size() != 2 || probe[0].glyph != first || probe[1].glyph != second;
    }

    [[nodiscard]] Position position_of_consonant(GlyphIndex consonant) const
    {
        if (m_virama == 0)
            return BASE_C;
        // Both orders: the font may look for either.
        auto const has = [&](u32 feature) { return would_substitute(feature, m_virama, consonant) || would_substitute(feature, consonant, m_virama); };
        if (has(feature_tag("blwf")))
            return BELOW_C;
        if (has(feature_tag("pstf")) || has(feature_tag("pref")))
            return POST_C;
        return BASE_C;
    }

private:
    OpenTypeLayout const& m_gsub;
    GlyphIndex m_virama;
};

struct IndicMasks {
    u32 rphf, pref, blwf, abvf, half, pstf, vatu, cjct, cfar, init;
    u32 nukt, akhn, rkrf;
};

// What's in a syllable, with its base: put in order, and told which features it wants.
void reorder_syllable(ShapingBuffer& buffer, size_t start, size_t end, Config const& config, IndicMasks const& masks, FormFinder const& finder)
{
    // Consonants first: where they'd go if they weren't the base.
    for (size_t i = start; i < end; ++i) {
        auto& glyph = buffer[i];
        if (glyph.shaping_category == C || glyph.shaping_category == RA)
            glyph.shaping_position = finder.position_of_consonant(glyph.glyph);
        else if (glyph.shaping_category == M)
            glyph.shaping_position = matra_position(glyph.code_point, config.script);
        else if (glyph.shaping_category == SM || glyph.shaping_category == VD || glyph.shaping_category == A)
            glyph.shaping_position = SMVD;
        else if (glyph.shaping_category == CM)
            glyph.shaping_position = BELOW_C;
        else
            glyph.shaping_position = BASE_C;
    }

    size_t base = end;
    size_t limit = start;
    bool has_reph = false;

    // Ra and a halant at the start: a reph, if the font makes one.
    if (config.reph_mode != RephMode::LogRepha && start + 3 <= end && buffer[start].shaping_category == RA && buffer[start + 1].shaping_category == H) {
        bool const allowed = config.reph_mode == RephMode::Explicit ? buffer[start + 2].shaping_category == ZWJ : !is_joiner(buffer[start + 2].shaping_category);
        if (allowed && finder.would_substitute(feature_tag("rphf"), buffer[start].glyph, buffer[start + 1].glyph)) {
            limit = start + 2;
            while (limit < end && is_joiner(buffer[limit].shaping_category))
                ++limit;
            base = start;
            has_reph = true;
        }
    } else if (config.reph_mode == RephMode::LogRepha && buffer[start].shaping_category == REPHA) {
        limit = start + 1;
        while (limit < end && is_joiner(buffer[limit].shaping_category))
            ++limit;
        base = start;
        has_reph = true;
    }

    // The base: from the end, the last consonant that doesn't have a form to go below or after.
    {
        size_t i = end;
        bool seen_below = false;
        while (i > limit) {
            --i;
            auto const& glyph = buffer[i];
            if (is_consonant(glyph.shaping_category)) {
                if (glyph.shaping_position != BELOW_C && (glyph.shaping_position != POST_C || seen_below)) {
                    base = i;
                    break;
                }
                if (glyph.shaping_position == BELOW_C)
                    seen_below = true;
                base = i;
            } else if (start < i && glyph.shaping_category == ZWJ && buffer[i - 1].shaping_category == H) {
                // A joiner after a halant keeps what's before it from being the base's half-form.
                break;
            }
        }
    }
    if (base == end) {
        // No consonant at all: what's first is the base.
        base = start;
    }
    if (has_reph && base == start) {
        // With nothing else to be the base, there's no reph after all.
        if (limit - start >= end - start)
            has_reph = false;
    }

    // Before the base: those go where half forms go. The base's the base. A reph is first.
    for (size_t i = start; i < base; ++i)
        buffer[i].shaping_position = min(PRE_C, static_cast<Position>(buffer[i].shaping_position));
    if (base < end)
        buffer[base].shaping_position = BASE_C;
    if (has_reph)
        buffer[start].shaping_position = RA_TO_BECOME_REPH;

    // Marks and such keep what they're with: each goes where the one before it did.
    u8 last_position = START;
    for (size_t i = start; i < end; ++i) {
        auto& glyph = buffer[i];
        if (glyph.shaping_category == ZWJ || glyph.shaping_category == ZWNJ || glyph.shaping_category == N || glyph.shaping_category == RS || glyph.shaping_category == H) {
            glyph.shaping_position = last_position;
            // A halant after a matra stays with the matra's group, though not ahead of the base.
            if (glyph.shaping_category == H && glyph.shaping_position == PRE_M) {
                for (size_t j = i; j > start; --j) {
                    if (buffer[j - 1].shaping_position != PRE_M) {
                        glyph.shaping_position = buffer[j - 1].shaping_position;
                        break;
                    }
                }
            }
        } else if (glyph.shaping_position != SMVD) {
            last_position = glyph.shaping_position;
        }
    }

    // Things after the base with no place of their own go with whatever's last.
    {
        size_t last = base;
        for (size_t i = base + 1; i < end; ++i) {
            if (is_consonant(buffer[i].shaping_category)) {
                for (size_t j = last + 1; j < i; ++j) {
                    if (buffer[j].shaping_position < SMVD)
                        buffer[j].shaping_position = buffer[i].shaping_position;
                }
                last = i;
            } else if (buffer[i].shaping_category == M) {
                last = i;
            }
        }
    }

    // Into that order, and stably. So the base can be found again, remember which it was by what cluster it's in.
    auto const base_glyph_position = base < end ? base : start;
    (void)base_glyph_position;
    for (size_t i = start + 1; i < end; ++i) {
        auto const held = buffer[i];
        size_t j = i;
        while (j > start && buffer[j - 1].shaping_position > held.shaping_position) {
            buffer[j] = buffer[j - 1];
            --j;
        }
        buffer[j] = held;
    }
    base = end;
    for (size_t i = start; i < end; ++i) {
        if (buffer[i].shaping_position == BASE_C) {
            base = i;
            break;
        }
    }

    // Which feature's for what.
    for (size_t i = start; i < end; ++i) {
        auto& glyph = buffer[i];
        if (glyph.shaping_position == RA_TO_BECOME_REPH) {
            glyph.mask |= masks.rphf;
            continue;
        }
    }
    if (has_reph) {
        // The halant goes with it.
        for (size_t i = start + 1; i < end && buffer[i].shaping_position <= RA_TO_BECOME_REPH; ++i)
            buffer[i].mask |= masks.rphf;
    }
    if (base < end) {
        u32 pre_mask = masks.half;
        if (config.below_form_mode == BelowFormMode::PreAndPost)
            pre_mask |= masks.blwf;
        for (size_t i = start; i < base; ++i) {
            if (!(buffer[i].mask & masks.rphf))
                buffer[i].mask |= pre_mask;
        }
        for (size_t i = base + 1; i < end; ++i)
            buffer[i].mask |= masks.blwf | masks.abvf | masks.pstf;
    }

    // A non-joiner stops half forms before it; a joiner asks for them, and for conjuncts not to form.
    for (size_t i = start + 1; i < end; ++i) {
        if (buffer[i].shaping_category != ZWNJ && buffer[i].shaping_category != ZWJ)
            continue;
        bool const non_joiner = buffer[i].shaping_category == ZWNJ;
        size_t j = i;
        do {
            --j;
            if (non_joiner)
                buffer[j].mask &= ~masks.half;
            if (is_consonant(buffer[j].shaping_category))
                break;
        } while (j > start);
    }

    // A pre-base form: the consonant after the base that has one, with its halant.
    if (base < end && masks.pref != 0) {
        for (size_t i = base + 1; i + 1 < end; ++i) {
            if (is_consonant(buffer[i + 1].shaping_category) || is_consonant(buffer[i].shaping_category)) {
                if (finder.would_substitute(feature_tag("pref"), buffer[i].glyph, buffer[i + 1].glyph)) {
                    buffer[i].mask |= masks.pref;
                    buffer[i + 1].mask |= masks.pref;
                    break;
                }
            }
        }
    }
}

// Now that the forms are there: what's still to move.
void final_reordering(ShapingBuffer& buffer, size_t start, size_t end, Config const& config, IndicMasks const& masks, GlyphIndex virama)
{
    if (end <= start)
        return;

    // The base: the first that's still marked as being one, or anything that's taken over from it by being in a ligature.
    size_t base = start;
    {
        bool found = false;
        for (size_t i = start; i < end; ++i) {
            if (buffer[i].shaping_position >= BASE_C) {
                base = i;
                found = true;
                break;
            }
        }
        if (!found)
            base = end - 1;
        // A consonant that became part of the base by a ligature is further on: go by what's not a mark.
        for (size_t i = base + 1; i < end; ++i) {
            if (buffer[i].shaping_position == BASE_C)
                base = i;
        }
    }

    auto const is_halant_glyph = [&](ShapingGlyph const& glyph) {
        return glyph.shaping_category == H || (virama != 0 && glyph.glyph == virama);
    };

    // Vowel signs that go before: as far along as can be, to just after any halant that's before the base and hasn't turned into anything.
    if (start < base && buffer[start].shaping_position <= PRE_M) {
        size_t new_position = base;
        for (size_t i = base; i > start + 1; --i) {
            if (buffer[i - 1].shaping_position == PRE_M) {
                // Past any that are already matras.
                continue;
            }
            if (is_halant_glyph(buffer[i - 1]) && !(buffer[i - 1].is_ligated) && !(buffer[i - 1].is_multiplied)) {
                new_position = i;
                break;
            }
            new_position = start;
        }
        // Only if a halant was found does it go there; otherwise it's the very start of the syllable.
        bool halant_found = false;
        for (size_t i = base; i > start; --i) {
            if (is_halant_glyph(buffer[i - 1]) && buffer[i - 1].shaping_position != PRE_M) {
                new_position = i;
                halant_found = true;
                break;
            }
        }
        if (!halant_found)
            new_position = start;
        // From here on, the matras at the front, moved to there.
        size_t const matra_count = [&] {
            size_t count = 0;
            for (size_t i = start; i < end && buffer[i].shaping_position == PRE_M; ++i)
                ++count;
            return count;
        }();
        if (new_position > start && matra_count > 0 && new_position > start + matra_count - 1) {
            for (size_t n = 0; n < matra_count; ++n) {
                auto const held = buffer[start];
                for (size_t i = start; i + 1 < new_position; ++i)
                    buffer[i] = buffer[i + 1];
                buffer[new_position - 1] = held;
            }
            if (base >= start + matra_count && base < new_position)
                base -= matra_count;
        }
    } else if (start < base) {
        // A matra that's past something else, as far as positions go.
        for (size_t i = start + 1; i < base; ++i) {
            if (buffer[i].shaping_position != PRE_M)
                continue;
            size_t new_position = i;
            for (size_t k = base; k > i; --k) {
                if (is_halant_glyph(buffer[k - 1])) {
                    new_position = k;
                    break;
                }
            }
            if (new_position > i) {
                auto const held = buffer[i];
                for (size_t k = i; k + 1 < new_position; ++k)
                    buffer[k] = buffer[k + 1];
                buffer[new_position - 1] = held;
            }
            break;
        }
    }

    // The reph: it's still first, as a mark, and goes where this script has it.
    if (start + 1 < end && buffer[start].shaping_position == RA_TO_BECOME_REPH && (buffer[start].is_ligated || buffer[start].was_substituted)) {
        auto const reph_position = config.reph_position;
        size_t new_position = end;
        bool placed = false;

        if (reph_position != AFTER_POST) {
            // After the first halant past the base that isn't followed by anything joined to it.
            if (start + 1 < base || (start + 1 <= base)) {
                size_t i = start + 1;
                while (i <= base && i < end && !is_halant_glyph(buffer[i]))
                    ++i;
                if (i <= base && i < end && is_halant_glyph(buffer[i])) {
                    if (i + 1 < end && is_joiner(buffer[i + 1].shaping_category))
                        ++i;
                    new_position = i;
                    placed = true;
                }
            }
            // Otherwise, before the first thing that goes after where a reph goes.
            if (!placed) {
                size_t i = base;
                while (i + 1 < end && buffer[i + 1].shaping_position <= reph_position)
                    ++i;
                if (reph_position != BEFORE_POST || true) {
                    new_position = i;
                    placed = true;
                }
            }
        }
        if (!placed) {
            // After everything but what's at the very end.
            size_t i = end - 1;
            while (i > start + 1 && buffer[i].shaping_position == SMVD)
                --i;
            new_position = i;
            if (new_position > start + 1 && is_halant_glyph(buffer[new_position])) {
                // Ahead of a halant left at the end: it's not going to look right after it.
            }
        }
        if (new_position > start && new_position < end) {
            auto const held = buffer[start];
            for (size_t i = start; i < new_position; ++i)
                buffer[i] = buffer[i + 1];
            buffer[new_position] = held;
            if (start < base && base <= new_position)
                --base;
        }
    }

    // A pre-base form, made by now: it goes before the base, and after any halant there is.
    if (masks.pref != 0 && base + 1 < end) {
        for (size_t i = base + 1; i < end; ++i) {
            if (!(buffer[i].mask & masks.pref))
                continue;
            if (!buffer[i].is_ligated && !buffer[i].was_substituted)
                break;
            size_t new_position = base;
            while (new_position > start && !(buffer[new_position - 1].shaping_category == M || is_halant_glyph(buffer[new_position - 1])))
                --new_position;
            if (new_position > start && buffer[new_position - 1].shaping_category == M) {
                size_t const held_position = new_position;
                for (size_t k = base; k > held_position; --k) {
                    if (buffer[k - 1].shaping_category == M) {
                        new_position = k - 1;
                        break;
                    }
                }
            }
            auto const held = buffer[i];
            for (size_t k = i; k > new_position; --k)
                buffer[k] = buffer[k - 1];
            buffer[new_position] = held;
            if (new_position <= base && base < i)
                ++base;
            break;
        }
    }

    // First in a word, for those that have a form for that.
    if (config.script != Script::Malayalam && config.script != Script::Tamil && buffer[start].shaping_position == PRE_M) {
        bool const at_word_start = start == 0 || !is_consonant(buffer[start - 1].shaping_category);
        if (at_word_start)
            buffer[start].mask |= masks.init;
    }

    // All in one cluster: nothing in a syllable can be selected apart, now that they've moved around.
    u32 cluster = buffer[start].cluster;
    for (size_t i = start; i < end; ++i)
        cluster = min(cluster, buffer[i].cluster);
    for (size_t i = start; i < end; ++i)
        buffer[i].cluster = cluster;
}

}

void prepare_indic(Typeface const& typeface, OpenTypeLayout const& gsub, ShapingBuffer& buffer, Plan& plan, Script script)
{
    Config config = s_configs[0];
    for (auto const& candidate : s_configs) {
        if (candidate.script == script)
            config = candidate;
    }
    GlyphIndex const virama = typeface.glyph_for_code_point(config.virama);
    FormFinder const finder(gsub, virama);

    IndicMasks masks {};
    masks.nukt = plan.mask_for(feature_tag("nukt"));
    masks.akhn = plan.mask_for(feature_tag("akhn"));
    masks.rphf = plan.mask_for(feature_tag("rphf"));
    masks.rkrf = plan.mask_for(feature_tag("rkrf"));
    masks.pref = plan.mask_for(feature_tag("pref"));
    masks.blwf = plan.mask_for(feature_tag("blwf"));
    masks.abvf = plan.mask_for(feature_tag("abvf"));
    masks.half = plan.mask_for(feature_tag("half"));
    masks.pstf = plan.mask_for(feature_tag("pstf"));
    masks.vatu = plan.mask_for(feature_tag("vatu"));
    masks.cjct = plan.mask_for(feature_tag("cjct"));
    masks.cfar = plan.mask_for(feature_tag("cfar"));
    masks.init = plan.mask_for(feature_tag("init"));

    for (auto& glyph : buffer) {
        glyph.shaping_category = category_of(glyph.code_point);
        glyph.shaping_position = BASE_C;
    }

    // Syllable by syllable, with a dotted circle where one's needed for a mark with nothing to go on.
    u8 serial = 1;
    GlyphIndex const dotted_circle = typeface.glyph_for_code_point(0x25cc);
    SyllableParser parser(buffer);
    for (size_t start = 0; start < buffer.size();) {
        SyllableParser::Kind kind = SyllableParser::Kind::Other;
        size_t length = parser.parse(start, kind);
        if (kind == SyllableParser::Kind::Broken && dotted_circle != 0) {
            ShapingGlyph circle = buffer[start];
            circle.glyph = dotted_circle;
            circle.code_point = 0x25cc;
            circle.shaping_category = DOTTED_CIRCLE;
            circle.glyph_class = ShapingGlyph::Base;
            circle.is_hidden = false;
            circle.is_zwj = circle.is_zwnj = false;
            size_t place = start;
            // After a reph, if there is one: it wants something to sit on.
            if (buffer[start].shaping_category == RA && start + 1 < buffer.size() && buffer[start + 1].shaping_category == H)
                place = start + 2;
            buffer.insert(place, circle);
            ++length;
            kind = SyllableParser::Kind::Standalone;
        }
        for (size_t i = start; i < start + length; ++i)
            buffer[i].syllable = serial;
        // (The serial tells the next pass what belongs together: when it comes round, neighbours just have to differ.)
        serial = static_cast<u8>(serial == 255 ? 1 : serial + 1);
        if (kind == SyllableParser::Kind::Consonant || kind == SyllableParser::Kind::Vowel || kind == SyllableParser::Kind::Standalone)
            reorder_syllable(buffer, start, start + length, config, masks, finder);
        start += length;
    }

    // In the order the document gives: forms first, and each with what it needs to have been done already.
    plan.add(feature_tag("nukt"), Plan::global_mask, true);
    plan.pause();
    plan.add(feature_tag("akhn"), Plan::global_mask, true);
    plan.pause();
    plan.add(feature_tag("rphf"), masks.rphf, true);
    plan.pause();
    plan.add(feature_tag("rkrf"), Plan::global_mask, true);
    plan.pause();
    plan.add(feature_tag("pref"), masks.pref, true);
    plan.pause();
    plan.add(feature_tag("blwf"), masks.blwf, true);
    plan.pause();
    plan.add(feature_tag("abvf"), masks.abvf, true);
    plan.pause();
    plan.add(feature_tag("half"), masks.half, true);
    plan.pause();
    plan.add(feature_tag("pstf"), masks.pstf, true);
    plan.pause();
    plan.add(feature_tag("vatu"), Plan::global_mask, true);
    plan.pause();
    plan.add(feature_tag("cjct"), Plan::global_mask, true);
    plan.pause([config, masks, virama](ShapingBuffer& glyphs) {
        for (size_t start = 0; start < glyphs.size();) {
            size_t end = start + 1;
            while (end < glyphs.size() && glyphs[end].syllable == glyphs[start].syllable)
                ++end;
            final_reordering(glyphs, start, end, config, masks, virama);
            start = end;
        }
    });
    plan.add(feature_tag("init"), masks.init, true);
    plan.pause();
    plan.add(feature_tag("pres"), Plan::global_mask, true);
    plan.add(feature_tag("abvs"), Plan::global_mask, true);
    plan.add(feature_tag("blws"), Plan::global_mask, true);
    plan.add(feature_tag("psts"), Plan::global_mask, true);
    plan.add(feature_tag("haln"), Plan::global_mask, true);
    plan.pause();
}

}
