/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

// Where a line can end: UAX #14's rules, in its order and with its numbers. This is the rules as they're written, a question at each place in the text, rather than the
// table they're usually turned into: it's plenty fast enough for a page of text, and can be read against the standard.

namespace Unicode {

using namespace Tables;

LineBreakClass line_break_class_of(CodePoint code)
{
    return static_cast<LineBreakClass>(run_value(Tables::line_break, line_break_count, code));
}

namespace {

using LB = LineBreakClass;

// What's left of the text once LB9 has had combining marks join whatever they're on: each is as if it's its base, wherever that is, and has no break before it.
struct Unit {
    LB type { LB::AL };
    bool is_east_asian { false };
    bool is_initial_quote { false }; // A QU that's Pi.
    bool is_final_quote { false };   // A QU that's Pf.
    bool is_dotted_circle { false };
    bool is_hyphen_u2010 { false };
    bool is_unassigned_pictographic { false };
    // The last of what it's made of is a ZWJ: LB8a.
    bool ends_with_zwj { false };
    size_t start { 0 };
};

LB resolved_class(CodePoint code)
{
    // LB1
    auto const type = line_break_class_of(code);
    switch (type) {
    case LB::AI:
    case LB::SG:
    case LB::XX:
        return LB::AL;
    case LB::SA: {
        auto const category = Unicode::general_category(code);
        return category == GeneralCategory::Mn || category == GeneralCategory::Mc ? LB::CM : LB::AL;
    }
    case LB::CJ:
        return LB::NS;
    default:
        return type;
    }
}

bool absorbs_marks(LB type)
{
    return type != LB::BK && type != LB::CR && type != LB::LF && type != LB::NL && type != LB::SP && type != LB::ZW;
}

Unit unit_of(CodePoint code, LB type, size_t start)
{
    Unit unit;
    unit.type = type;
    unit.start = start;
    auto const width = Unicode::east_asian_width(code);
    unit.is_east_asian = width == EastAsianWidth::F || width == EastAsianWidth::W || width == EastAsianWidth::H;
    auto const category = Unicode::general_category(code);
    unit.is_initial_quote = type == LB::QU && category == GeneralCategory::Pi;
    unit.is_final_quote = type == LB::QU && category == GeneralCategory::Pf;
    unit.is_dotted_circle = code == 0x25cc;
    unit.is_hyphen_u2010 = code == 0x2010;
    unit.is_unassigned_pictographic = category == GeneralCategory::Cn && Unicode::is_extended_pictographic(code);
    return unit;
}

class Rules {
public:
    explicit Rules(Vector<Unit> const& units)
        : m_units(units)
    {
    }

    enum class Decision : u8 {
        NoBreak,
        Break,
        Mandatory,
    };

    // Between unit k - 1 and unit k.
    [[nodiscard]] Decision at(size_t k) const
    {
        auto const& a_unit = m_units[k - 1];
        auto const& b_unit = m_units[k];
        LB const a = a_unit.type;
        LB const b = b_unit.type;
        bool const has_next = k + 1 < m_units.size();
        LB const next = has_next ? m_units[k + 1].type : LB::XX;
        bool const has_before_a = k >= 2;
        LB const before_a = has_before_a ? m_units[k - 2].type : LB::XX;

        // LB4
        if (a == LB::BK)
            return Decision::Mandatory;
        // LB5
        if (a == LB::CR && b == LB::LF)
            return Decision::NoBreak;
        if (a == LB::CR || a == LB::LF || a == LB::NL)
            return Decision::Mandatory;
        // LB6
        if (b == LB::BK || b == LB::CR || b == LB::LF || b == LB::NL)
            return Decision::NoBreak;
        // LB7
        if (b == LB::SP || b == LB::ZW)
            return Decision::NoBreak;
        // LB8
        size_t const before_spaces = index_before_spaces(k);
        bool const any_before_spaces = before_spaces != no_index;
        LB const last_before_spaces = any_before_spaces ? m_units[before_spaces].type : LB::XX;
        if (last_before_spaces == LB::ZW)
            return Decision::Break;
        // LB8a
        if (a_unit.ends_with_zwj)
            return Decision::NoBreak;
        // LB11
        if (a == LB::WJ || b == LB::WJ)
            return Decision::NoBreak;
        // LB12
        if (a == LB::GL)
            return Decision::NoBreak;
        // LB12a
        if (b == LB::GL && a != LB::SP && a != LB::BA && a != LB::HY)
            return Decision::NoBreak;
        // LB13
        if (b == LB::CL || b == LB::CP || b == LB::EX || b == LB::SY)
            return Decision::NoBreak;
        // LB14
        if (last_before_spaces == LB::OP)
            return Decision::NoBreak;
        // LB15a
        if (any_before_spaces && m_units[before_spaces].is_initial_quote) {
            if (before_spaces == 0)
                return Decision::NoBreak;
            LB const preceding = m_units[before_spaces - 1].type;
            if (preceding == LB::BK || preceding == LB::CR || preceding == LB::LF || preceding == LB::NL || preceding == LB::OP || preceding == LB::QU || preceding == LB::GL || preceding == LB::SP || preceding == LB::ZW)
                return Decision::NoBreak;
        }
        // LB15b
        if (b_unit.is_final_quote) {
            if (!has_next || next == LB::SP || next == LB::GL || next == LB::WJ || next == LB::CL || next == LB::QU || next == LB::CP || next == LB::EX || next == LB::IS || next == LB::SY || next == LB::BK || next == LB::CR || next == LB::LF || next == LB::NL || next == LB::ZW)
                return Decision::NoBreak;
        }
        // LB15c
        if (a == LB::SP && b == LB::IS && next == LB::NU && has_next)
            return Decision::Break;
        // LB15d
        if (b == LB::IS)
            return Decision::NoBreak;
        // LB16
        if ((last_before_spaces == LB::CL || last_before_spaces == LB::CP) && b == LB::NS)
            return Decision::NoBreak;
        // LB17
        if (last_before_spaces == LB::B2 && b == LB::B2)
            return Decision::NoBreak;
        // LB18
        if (a == LB::SP)
            return Decision::Break;
        // LB19
        if (b == LB::QU && !b_unit.is_initial_quote)
            return Decision::NoBreak;
        if (a == LB::QU && !a_unit.is_final_quote)
            return Decision::NoBreak;
        // LB19a
        if (b == LB::QU && !a_unit.is_east_asian)
            return Decision::NoBreak;
        if (b == LB::QU && (!has_next || !m_units[k + 1].is_east_asian))
            return Decision::NoBreak;
        if (a == LB::QU && !b_unit.is_east_asian)
            return Decision::NoBreak;
        if (a == LB::QU && (!has_before_a || !m_units[k - 2].is_east_asian))
            return Decision::NoBreak;
        // LB20
        if (a == LB::CB || b == LB::CB)
            return Decision::Break;
        // LB20a
        if ((a == LB::HY || a_unit.is_hyphen_u2010) && b == LB::AL) {
            if (!has_before_a || before_a == LB::BK || before_a == LB::CR || before_a == LB::LF || before_a == LB::NL || before_a == LB::SP || before_a == LB::ZW || before_a == LB::CB || before_a == LB::GL)
                return Decision::NoBreak;
        }
        // LB21
        if (b == LB::BA || b == LB::HY || b == LB::NS || a == LB::BB)
            return Decision::NoBreak;
        // LB21a
        if (has_before_a && before_a == LB::HL && (a == LB::HY || (a == LB::BA && !a_unit.is_east_asian)) && b != LB::HL)
            return Decision::NoBreak;
        // LB21b
        if (a == LB::SY && b == LB::HL)
            return Decision::NoBreak;
        // LB22
        if (b == LB::IN)
            return Decision::NoBreak;
        // LB23
        if (((a == LB::AL || a == LB::HL) && b == LB::NU) || (a == LB::NU && (b == LB::AL || b == LB::HL)))
            return Decision::NoBreak;
        // LB23a
        if (a == LB::PR && (b == LB::ID || b == LB::EB || b == LB::EM))
            return Decision::NoBreak;
        if ((a == LB::ID || a == LB::EB || a == LB::EM) && b == LB::PO)
            return Decision::NoBreak;
        // LB24
        if ((a == LB::PR || a == LB::PO) && (b == LB::AL || b == LB::HL))
            return Decision::NoBreak;
        if ((a == LB::AL || a == LB::HL) && (b == LB::PR || b == LB::PO))
            return Decision::NoBreak;
        // LB25
        if (in_number_before(k) && (b == LB::PO || b == LB::PR))
            return Decision::NoBreak;
        if ((a == LB::CL || a == LB::CP) && k >= 2 && in_number_before(k - 1) && (b == LB::PO || b == LB::PR))
            return Decision::NoBreak;
        if ((a == LB::PO || a == LB::PR) && b == LB::OP && next == LB::NU && has_next)
            return Decision::NoBreak;
        if ((a == LB::PO || a == LB::PR) && b == LB::OP && next == LB::IS && k + 2 < m_units.size() && m_units[k + 2].type == LB::NU)
            return Decision::NoBreak;
        if ((a == LB::PO || a == LB::PR) && b == LB::NU)
            return Decision::NoBreak;
        if ((a == LB::HY || a == LB::IS) && b == LB::NU)
            return Decision::NoBreak;
        if (in_number_before(k) && b == LB::NU)
            return Decision::NoBreak;
        // LB26
        if (a == LB::JL && (b == LB::JL || b == LB::JV || b == LB::H2 || b == LB::H3))
            return Decision::NoBreak;
        if ((a == LB::JV || a == LB::H2) && (b == LB::JV || b == LB::JT))
            return Decision::NoBreak;
        if ((a == LB::JT || a == LB::H3) && b == LB::JT)
            return Decision::NoBreak;
        // LB27
        if ((a == LB::JL || a == LB::JV || a == LB::JT || a == LB::H2 || a == LB::H3) && b == LB::PO)
            return Decision::NoBreak;
        if (a == LB::PR && (b == LB::JL || b == LB::JV || b == LB::JT || b == LB::H2 || b == LB::H3))
            return Decision::NoBreak;
        // LB28
        if ((a == LB::AL || a == LB::HL) && (b == LB::AL || b == LB::HL))
            return Decision::NoBreak;
        // LB28a
        auto const is_aksara = [](Unit const& unit) { return unit.type == LB::AK || unit.is_dotted_circle || unit.type == LB::AS; };
        auto const is_aksara_start = [](Unit const& unit) { return unit.type == LB::AK || unit.is_dotted_circle; };
        if (a == LB::AP && is_aksara(b_unit))
            return Decision::NoBreak;
        if (is_aksara(a_unit) && (b == LB::VF || b == LB::VI))
            return Decision::NoBreak;
        if (a == LB::VI && has_before_a && is_aksara(m_units[k - 2]) && is_aksara_start(b_unit))
            return Decision::NoBreak;
        if (is_aksara(a_unit) && is_aksara(b_unit) && has_next && next == LB::VF)
            return Decision::NoBreak;
        // LB29
        if (a == LB::IS && (b == LB::AL || b == LB::HL))
            return Decision::NoBreak;
        // LB30
        if ((a == LB::AL || a == LB::HL || a == LB::NU) && b == LB::OP && !b_unit.is_east_asian)
            return Decision::NoBreak;
        if (a == LB::CP && !a_unit.is_east_asian && (b == LB::AL || b == LB::HL || b == LB::NU))
            return Decision::NoBreak;
        // LB30a
        if (a == LB::RI && b == LB::RI) {
            size_t count = 0;
            for (size_t i = k; i > 0 && m_units[i - 1].type == LB::RI; --i)
                ++count;
            if (count % 2 == 1)
                return Decision::NoBreak;
        }
        // LB30b
        if (a == LB::EB && b == LB::EM)
            return Decision::NoBreak;
        if (a_unit.is_unassigned_pictographic && b == LB::EM)
            return Decision::NoBreak;
        // LB31
        return Decision::Break;
    }

private:
    static constexpr size_t no_index = ~size_t(0);

    // The unit before `k`, looking through any spaces. None, if it's only spaces back to the start.
    [[nodiscard]] size_t index_before_spaces(size_t k) const
    {
        size_t i = k;
        while (i > 0) {
            --i;
            if (m_units[i].type != LB::SP)
                return i;
        }
        return no_index;
    }

    // Do the units just before `k` make NU (SY | IS)*?
    [[nodiscard]] bool in_number_before(size_t k) const
    {
        for (size_t i = k; i > 0; --i) {
            auto const type = m_units[i - 1].type;
            if (type == LB::NU)
                return true;
            if (type != LB::SY && type != LB::IS)
                return false;
        }
        return false;
    }

    Vector<Unit> const& m_units;
};

}

ErrorOr<Vector<LineBreakOpportunity>> line_break_opportunities(Span<CodePoint const> text)
{
    Vector<LineBreakOpportunity> result;
    if (text.is_empty())
        return result;

    Vector<Unit> units;
    TRY(units.try_ensure_capacity(text.size()));
    for (size_t i = 0; i < text.size(); ++i) {
        CodePoint const code = text[i];
        LB type = resolved_class(code);
        bool const is_mark_like = type == LB::CM || type == LB::ZWJ;
        // LB9
        if (is_mark_like && !units.is_empty() && absorbs_marks(units.last().type)) {
            units.last().ends_with_zwj = type == LB::ZWJ;
            continue;
        }
        // LB10: nothing to go on, so it's as if it were a letter.
        bool const was_zwj = type == LB::ZWJ;
        if (is_mark_like) {
            auto unit = unit_of('A', LB::AL, i);
            unit.ends_with_zwj = was_zwj;
            TRY(units.try_append(unit));
            continue;
        }
        TRY(units.try_append(unit_of(code, type, i)));
    }

    Rules rules(units);
    for (size_t k = 1; k < units.size(); ++k) {
        auto const decision = rules.at(k);
        if (decision != Rules::Decision::NoBreak)
            TRY(result.try_append({ units[k].start, decision == Rules::Decision::Mandatory }));
    }
    // LB3
    TRY(result.try_append({ text.size(), true }));
    return result;
}

}
