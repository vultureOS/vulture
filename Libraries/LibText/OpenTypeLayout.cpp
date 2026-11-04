/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/OpenTypeLayout.h>

namespace Text {

namespace {

// How much a font can ask for. Fonts are data from anywhere, and these stop one that's made to take forever.
constexpr int largest_context_depth = 8;
constexpr size_t largest_buffer_growth = 4096;
constexpr size_t largest_match = 64;
constexpr u32 most_operations_per_glyph = 2048;

// What a lookup says to ignore.
constexpr u16 flag_right_to_left = 0x0001;
constexpr u16 flag_ignore_bases = 0x0002;
constexpr u16 flag_ignore_ligatures = 0x0004;
constexpr u16 flag_ignore_marks = 0x0008;
constexpr u16 flag_use_mark_filtering_set = 0x0010;

}

struct OpenTypeLayout::Run {
    ShapingBuffer& buffer;
    OpenTypeLayout const& layout;
    size_t position { 0 };
    u32 mask { 0 };
    u16 lookup_flags { 0 };
    u16 mark_filtering_set { 0 };
    bool is_right_to_left { false };
    bool joiners_count { false };
    int depth { 0 };
    size_t original_size { 0 };
    u32 operations { 0 };
    u32 operations_allowed { 0 };
    u8 next_ligature_id { 1 };

    [[nodiscard]] bool too_much()
    {
        return ++operations > operations_allowed;
    }

    // Whether this lookup acts as if this glyph isn't there.
    [[nodiscard]] bool is_skipped(ShapingGlyph const& glyph) const
    {
        switch (glyph.glyph_class) {
        case ShapingGlyph::Base:
            if (lookup_flags & flag_ignore_bases)
                return true;
            break;
        case ShapingGlyph::Ligature:
            if (lookup_flags & flag_ignore_ligatures)
                return true;
            break;
        case ShapingGlyph::Mark: {
            if (lookup_flags & flag_ignore_marks)
                return true;
            if (lookup_flags & flag_use_mark_filtering_set)
                return !layout.is_in_mark_set(mark_filtering_set, glyph.glyph);
            u8 const wanted = static_cast<u8>(lookup_flags >> 8);
            if (wanted && wanted != glyph.mark_attachment_class)
                return true;
            break;
        }
        default:
            break;
        }
        // Joiners are looked straight through when positioning. A non-joiner stops substitutions from matching, as that's what it's there for, and a joiner doesn't.
        if (layout.m_which == Table::GPOS)
            return glyph.is_zwj || glyph.is_zwnj;
        return glyph.is_zwj && !joiners_count;
    }

    // The next that counts, if this is what's being looked for: a joiner's looked through unless it's what's wanted. (A family of emoji is a ligature with joiners in it.)
    [[nodiscard]] bool next_matching(size_t& at, GlyphIndex wanted) const
    {
        for (size_t i = at + 1; i < buffer.size(); ++i) {
            auto const& glyph = buffer[i];
            if (glyph.is_zwj && glyph.glyph == wanted && layout.m_which == Table::GSUB) {
                at = i;
                return true;
            }
            if (!is_skipped(glyph)) {
                at = i;
                return glyph.glyph == wanted;
            }
        }
        return false;
    }

    // The next that counts, from here: false if there isn't one before the end.
    [[nodiscard]] bool next_counted(size_t& at) const
    {
        for (size_t i = at + 1; i < buffer.size(); ++i) {
            if (!is_skipped(buffer[i])) {
                at = i;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool previous_counted(size_t& at) const
    {
        for (size_t i = at; i > 0; --i) {
            if (!is_skipped(buffer[i - 1])) {
                at = i - 1;
                return true;
            }
        }
        return false;
    }
};

OpenTypeLayout::OpenTypeLayout(Typeface const& typeface, Table which)
    : m_typeface(typeface)
    , m_which(which)
{
    m_gdef = typeface.offset_of_table(feature_tag("GDEF"));
    m_table = typeface.offset_of_table(which == Table::GSUB ? feature_tag("GSUB") : feature_tag("GPOS"));
    if (m_table == 0)
        return;
    u16 const major = u16_at(m_table);
    if (major != 1) {
        m_table = 0;
        return;
    }
    m_script_list = m_table + u16_at(m_table + 4);
    m_feature_list = m_table + u16_at(m_table + 6);
    m_lookup_list = m_table + u16_at(m_table + 8);
}

bool OpenTypeLayout::select_script(LayoutScript const& wanted)
{
    m_language_system = 0;
    m_script_tag = 0;
    m_required_feature = {};
    if (m_table == 0)
        return false;
    u16 const scripts = u16_at(m_script_list);
    auto const find_script = [&](u32 tag) -> size_t {
        for (u16 i = 0; i < scripts; ++i) {
            size_t const record = m_script_list + 2 + static_cast<size_t>(i) * 6;
            if (u32_at(record) == tag)
                return m_script_list + u16_at(record + 4);
        }
        return 0;
    };
    size_t script = 0;
    bool found_wanted = false;
    for (u32 tag : wanted.tags) {
        if (tag == 0)
            continue;
        script = find_script(tag);
        if (script != 0) {
            m_script_tag = tag;
            found_wanted = true;
            break;
        }
    }
    if (script == 0) {
        u32 const fallbacks[] = { feature_tag("DFLT"), feature_tag("dflt"), feature_tag("latn") };
        for (u32 tag : fallbacks) {
            script = find_script(tag);
            if (script != 0) {
                m_script_tag = tag;
                break;
            }
        }
    }
    if (script == 0)
        return false;

    // Its language, or what it has by default.
    size_t system = 0;
    if (wanted.language != 0) {
        u16 const systems = u16_at(script + 2);
        for (u16 i = 0; i < systems; ++i) {
            size_t const record = script + 4 + static_cast<size_t>(i) * 6;
            if (u32_at(record) == wanted.language) {
                system = script + u16_at(record + 4);
                break;
            }
        }
    }
    if (system == 0 && u16_at(script) != 0)
        system = script + u16_at(script);
    m_language_system = system;
    if (system != 0) {
        u16 const required = u16_at(system + 2);
        if (required != 0xffff)
            m_required_feature = required;
    }
    return found_wanted;
}

void OpenTypeLayout::collect_lookups_of_feature_index(u16 feature_index, Vector<u16>& lookups) const
{
    if (m_table == 0 || feature_index >= u16_at(m_feature_list))
        return;
    size_t const feature = m_feature_list + u16_at(m_feature_list + 2 + static_cast<size_t>(feature_index) * 6 + 4);
    u16 const count = u16_at(feature + 2);
    for (u16 i = 0; i < count; ++i) {
        u16 const lookup = u16_at(feature + 4 + static_cast<size_t>(i) * 2);
        if (!lookups.contains_slow(lookup))
            lookups.append(lookup);
    }
}

void OpenTypeLayout::collect_lookups(u32 wanted, Vector<u16>& lookups) const
{
    if (m_language_system == 0)
        return;
    u16 const count = u16_at(m_language_system + 4);
    for (u16 i = 0; i < count; ++i) {
        u16 const feature_index = u16_at(m_language_system + 6 + static_cast<size_t>(i) * 2);
        if (feature_index >= u16_at(m_feature_list))
            continue;
        if (u32_at(m_feature_list + 2 + static_cast<size_t>(feature_index) * 6) == wanted)
            collect_lookups_of_feature_index(feature_index, lookups);
    }
}

bool OpenTypeLayout::has_feature(u32 wanted) const
{
    Vector<u16> lookups;
    collect_lookups(wanted, lookups);
    return !lookups.is_empty();
}

// ---- Coverage and classes.

int OpenTypeLayout::coverage_index(size_t coverage, GlyphIndex glyph) const
{
    u16 const format = u16_at(coverage);
    if (format == 1) {
        u16 const count = u16_at(coverage + 2);
        u32 low = 0, high = count;
        while (low < high) {
            u32 const middle = (low + high) / 2;
            u16 const value = u16_at(coverage + 4 + static_cast<size_t>(middle) * 2);
            if (value == glyph)
                return static_cast<int>(middle);
            if (value < glyph)
                low = middle + 1;
            else
                high = middle;
        }
        return -1;
    }
    if (format == 2) {
        u16 const count = u16_at(coverage + 2);
        u32 low = 0, high = count;
        while (low < high) {
            u32 const middle = (low + high) / 2;
            size_t const range = coverage + 4 + static_cast<size_t>(middle) * 6;
            if (glyph < u16_at(range))
                high = middle;
            else if (glyph > u16_at(range + 2))
                low = middle + 1;
            else
                return u16_at(range + 4) + (glyph - u16_at(range));
        }
    }
    return -1;
}

u16 OpenTypeLayout::class_of(size_t definition, GlyphIndex glyph) const
{
    u16 const format = u16_at(definition);
    if (format == 1) {
        u16 const start = u16_at(definition + 2);
        u16 const count = u16_at(definition + 4);
        if (glyph >= start && glyph - start < count)
            return u16_at(definition + 6 + static_cast<size_t>(glyph - start) * 2);
        return 0;
    }
    if (format == 2) {
        u16 const count = u16_at(definition + 2);
        u32 low = 0, high = count;
        while (low < high) {
            u32 const middle = (low + high) / 2;
            size_t const range = definition + 4 + static_cast<size_t>(middle) * 6;
            if (glyph < u16_at(range))
                high = middle;
            else if (glyph > u16_at(range + 2))
                low = middle + 1;
            else
                return u16_at(range + 4);
        }
    }
    return 0;
}

bool OpenTypeLayout::is_in_mark_set(u16 set, GlyphIndex glyph) const
{
    if (m_gdef == 0 || u16_at(m_gdef) != 1 || u16_at(m_gdef + 2) < 2)
        return false;
    size_t const sets_offset = u16_at(m_gdef + 12);
    if (sets_offset == 0)
        return false;
    size_t const sets = m_gdef + sets_offset;
    if (u16_at(sets) != 1 || set >= u16_at(sets + 2))
        return false;
    size_t const coverage = sets + u32_at(sets + 4 + static_cast<size_t>(set) * 4);
    return coverage_index(coverage, glyph) >= 0;
}

bool OpenTypeLayout::has_glyph_classes() const
{
    return m_gdef != 0 && u16_at(m_gdef + 4) != 0;
}

void OpenTypeLayout::classify(ShapingBuffer& buffer) const
{
    if (m_gdef == 0)
        return;
    size_t const classes_offset = u16_at(m_gdef + 4);
    size_t const marks_offset = u16_at(m_gdef + 10);
    for (auto& glyph : buffer) {
        if (classes_offset != 0)
            glyph.glyph_class = static_cast<u8>(class_of(m_gdef + classes_offset, glyph.glyph));
        if (marks_offset != 0)
            glyph.mark_attachment_class = static_cast<u8>(class_of(m_gdef + marks_offset, glyph.glyph));
    }
}

// ---- Running a lookup.

void OpenTypeLayout::apply_lookup(u16 lookup_index, ShapingBuffer& buffer, u32 mask, bool is_right_to_left, bool joiners_count) const
{
    if (m_table == 0 || lookup_index >= u16_at(m_lookup_list))
        return;
    Run run { buffer, *this };
    run.mask = mask;
    run.is_right_to_left = is_right_to_left;
    run.joiners_count = joiners_count;
    run.original_size = buffer.size();
    run.operations_allowed = static_cast<u32>(buffer.size() + 8) * most_operations_per_glyph;
    // (Which ligature's which only has to be different within this text: the highest there is now, and on from there.)
    for (auto const& glyph : buffer)
        run.next_ligature_id = max(run.next_ligature_id, static_cast<u8>(glyph.ligature_id + 1));

    size_t const lookup = m_lookup_list + u16_at(m_lookup_list + 2 + static_cast<size_t>(lookup_index) * 2);
    u16 const type = u16_at(lookup);
    bool const is_reverse = m_which == Table::GSUB && (type == 8 || (type == 7 && u16_at(lookup + 6 + 0) != 0 && u16_at(lookup + u16_at(lookup + 6) + 2) == 8));

    if (is_reverse) {
        // From the end, one at a time. Nothing changes how many there are.
        for (size_t i = buffer.size(); i > 0; --i) {
            run.position = i - 1;
            if (buffer[i - 1].mask & mask)
                (void)apply_lookup_at(run, lookup_index);
        }
        return;
    }
    run.position = 0;
    while (run.position < buffer.size()) {
        if (run.too_much())
            return;
        auto const before = run.position;
        bool applied = false;
        if (buffer[run.position].mask & mask)
            applied = apply_lookup_at(run, lookup_index);
        if (!applied || run.position == before)
            run.position = before + 1;
    }
}

bool OpenTypeLayout::apply_lookup_at(Run& run, u16 lookup_index) const
{
    if (lookup_index >= u16_at(m_lookup_list) || run.position >= run.buffer.size())
        return false;
    size_t const lookup = m_lookup_list + u16_at(m_lookup_list + 2 + static_cast<size_t>(lookup_index) * 2);
    u16 const type = u16_at(lookup);
    u16 const flags = u16_at(lookup + 2);
    u16 const count = u16_at(lookup + 4);
    u16 const saved_flags = run.lookup_flags;
    u16 const saved_set = run.mark_filtering_set;
    run.lookup_flags = flags;
    if (flags & flag_use_mark_filtering_set)
        run.mark_filtering_set = u16_at(lookup + 6 + static_cast<size_t>(count) * 2);
    bool applied = false;
    if (!run.is_skipped(run.buffer[run.position])) {
        for (u16 i = 0; i < count && !applied; ++i) {
            size_t const subtable = lookup + u16_at(lookup + 6 + static_cast<size_t>(i) * 2);
            applied = apply_subtable(run, type, subtable);
        }
    }
    run.lookup_flags = saved_flags;
    run.mark_filtering_set = saved_set;
    return applied;
}

bool OpenTypeLayout::apply_subtable(Run& run, u16 type, size_t subtable) const
{
    if (run.too_much())
        return false;
    if (m_which == Table::GSUB)
        return apply_gsub_subtable(run, type, subtable);
    return apply_gpos_subtable(run, type, subtable);
}

// ---- Substitutions.

bool OpenTypeLayout::apply_gsub_subtable(Run& run, u16 type, size_t subtable) const
{
    auto& buffer = run.buffer;
    size_t const at = run.position;
    GlyphIndex const glyph = buffer[at].glyph;
    u16 const glyph_count = m_typeface.glyph_count();

    auto const replace = [&](ShapingGlyph& target, GlyphIndex replacement) {
        target.glyph = replacement < glyph_count ? replacement : 0;
        target.was_substituted = true;
        if (m_gdef != 0 && u16_at(m_gdef + 4) != 0)
            target.glyph_class = static_cast<u8>(class_of(m_gdef + u16_at(m_gdef + 4), target.glyph));
        if (m_gdef != 0 && u16_at(m_gdef + 10) != 0)
            target.mark_attachment_class = static_cast<u8>(class_of(m_gdef + u16_at(m_gdef + 10), target.glyph));
    };

    switch (type) {
    case 1: { // One for one.
        size_t const coverage = subtable + u16_at(subtable + 2);
        int const index = coverage_index(coverage, glyph);
        if (index < 0)
            return false;
        u16 const format = u16_at(subtable);
        if (format == 1) {
            replace(buffer[at], static_cast<GlyphIndex>(glyph + i16_at(subtable + 4)));
        } else if (format == 2) {
            if (index >= u16_at(subtable + 4))
                return false;
            replace(buffer[at], u16_at(subtable + 6 + static_cast<size_t>(index) * 2));
        } else {
            return false;
        }
        run.position = at + 1;
        return true;
    }
    case 2: { // One into several, or none.
        if (u16_at(subtable) != 1)
            return false;
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const sequence = subtable + u16_at(subtable + 6 + static_cast<size_t>(index) * 2);
        u16 const count = u16_at(sequence);
        if (buffer.size() + count > run.original_size + largest_buffer_growth)
            return false;
        if (count == 0) {
            buffer.remove(at);
            run.position = at;
            return true;
        }
        ShapingGlyph const original = buffer[at];
        replace(buffer[at], u16_at(sequence + 2));
        buffer[at].is_multiplied = count > 1;
        for (u16 i = 1; i < count; ++i) {
            ShapingGlyph extra = original;
            replace(extra, u16_at(sequence + 2 + static_cast<size_t>(i) * 2));
            extra.is_multiplied = true;
            // A mark among what it became is on the same part of whatever it's on.
            extra.ligature_component = static_cast<u8>(original.ligature_component ? original.ligature_component : 0);
            buffer.insert(at + i, extra);
        }
        run.position = at + count;
        return true;
    }
    case 3: { // One of a choice: the first, as nothing here chooses.
        if (u16_at(subtable) != 1)
            return false;
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const set = subtable + u16_at(subtable + 6 + static_cast<size_t>(index) * 2);
        if (u16_at(set) == 0)
            return false;
        replace(buffer[at], u16_at(set + 2));
        run.position = at + 1;
        return true;
    }
    case 4: { // Several into one.
        if (u16_at(subtable) != 1)
            return false;
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const set = subtable + u16_at(subtable + 6 + static_cast<size_t>(index) * 2);
        u16 const ligatures = u16_at(set);
        for (u16 l = 0; l < ligatures; ++l) {
            size_t const ligature = set + u16_at(set + 2 + static_cast<size_t>(l) * 2);
            u16 const parts = u16_at(ligature + 2);
            if (parts == 0 || parts > largest_match)
                continue;
            size_t matched[largest_match];
            matched[0] = at;
            size_t cursor = at;
            bool matches = true;
            for (u16 c = 1; c < parts && matches; ++c) {
                matches = run.next_matching(cursor, u16_at(ligature + 4 + static_cast<size_t>(c - 1) * 2)) && (buffer[cursor].mask & run.mask);
                matched[c] = cursor;
            }
            if (!matches)
                continue;
            // A ligature can't be made across a non-joiner, or across something that's already been kept apart. What's been skipped stays, and marks among it go on the part that they were after.
            bool crosses_non_joiner = false;
            for (size_t i = matched[0]; i < matched[parts - 1]; ++i) {
                if (buffer[i].is_zwnj)
                    crosses_non_joiner = true;
            }
            if (crosses_non_joiner)
                continue;

            u8 const id = run.next_ligature_id;
            run.next_ligature_id = static_cast<u8>(id == 255 ? 1 : id + 1);
            bool const only_marks = [&] {
                for (u16 c = 0; c < parts; ++c) {
                    if (buffer[matched[c]].glyph_class != ShapingGlyph::Mark)
                        return false;
                }
                return true;
            }();

            u32 cluster = buffer[at].cluster;
            for (u16 c = 1; c < parts; ++c)
                cluster = min(cluster, buffer[matched[c]].cluster);

            // Marks that were skipped over say which part of it they're on.
            u8 const previous_id = buffer[at].ligature_id;
            for (u16 c = 1; c < parts; ++c) {
                for (size_t i = matched[c - 1] + 1; i < matched[c]; ++i) {
                    if (buffer[i].glyph_class == ShapingGlyph::Mark) {
                        buffer[i].ligature_id = id;
                        buffer[i].ligature_component = static_cast<u8>(c);
                    }
                }
            }
            // And so do the ones that were on the last of what it's made of, after it.
            for (size_t i = matched[parts - 1] + 1; i < buffer.size(); ++i) {
                if (buffer[i].glyph_class != ShapingGlyph::Mark)
                    break;
                u8 const component = buffer[i].ligature_component;
                u8 const last_parts = buffer[matched[parts - 1]].ligature_parts;
                buffer[i].ligature_id = id;
                buffer[i].ligature_component = static_cast<u8>(parts + (component > 0 && last_parts > 0 ? component - 1 : 0));
            }
            (void)previous_id;

            replace(buffer[at], u16_at(ligature));
            buffer[at].cluster = cluster;
            buffer[at].is_ligated = true;
            buffer[at].is_hidden = false;
            buffer[at].ligature_id = only_marks ? 0 : id;
            buffer[at].ligature_component = 0;
            buffer[at].ligature_parts = static_cast<u8>(parts);
            // (A ligature's class is what GDEF says, if there's a GDEF; otherwise it's what it's been made into.)
            if (m_gdef == 0 || u16_at(m_gdef + 4) == 0)
                buffer[at].glyph_class = only_marks ? ShapingGlyph::Mark : ShapingGlyph::Ligature;

            // Everything between goes into the same cluster, so that no selection cuts it apart. From the back, so that places stay right.
            for (size_t i = at + 1; i <= matched[parts - 1]; ++i)
                buffer[i].cluster = cluster;
            for (u16 c = parts; c > 1; --c)
                buffer.remove(matched[c - 1]);
            run.position = at + 1;
            return true;
        }
        return false;
    }
    case 5:
        return apply_context(run, subtable);
    case 6:
        return apply_chain_context(run, subtable);
    case 7: { // Somewhere further off.
        if (u16_at(subtable) != 1)
            return false;
        u16 const real_type = u16_at(subtable + 2);
        if (real_type == 7)
            return false;
        return apply_gsub_subtable(run, real_type, subtable + u32_at(subtable + 4));
    }
    case 8: { // From the end, with what's on both sides.
        if (u16_at(subtable) != 1)
            return false;
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0)
            return false;
        u16 const backtrack = u16_at(subtable + 4);
        size_t cursor = at;
        for (u16 b = 0; b < backtrack; ++b) {
            if (!run.previous_counted(cursor))
                return false;
            if (coverage_index(subtable + u16_at(subtable + 6 + static_cast<size_t>(b) * 2), buffer[cursor].glyph) < 0)
                return false;
        }
        size_t const lookahead_at = subtable + 6 + static_cast<size_t>(backtrack) * 2;
        u16 const lookahead = u16_at(lookahead_at);
        cursor = at;
        for (u16 f = 0; f < lookahead; ++f) {
            if (!run.next_counted(cursor))
                return false;
            if (coverage_index(subtable + u16_at(lookahead_at + 2 + static_cast<size_t>(f) * 2), buffer[cursor].glyph) < 0)
                return false;
        }
        size_t const substitutes = lookahead_at + 2 + static_cast<size_t>(lookahead) * 2;
        if (index >= u16_at(substitutes))
            return false;
        replace(buffer[at], u16_at(substitutes + 2 + static_cast<size_t>(index) * 2));
        return true;
    }
    default:
        return false;
    }
}

// ---- Contexts: shared by substitutions and positions, as they're the same thing.

namespace {

struct ContextRecord {
    u16 sequence_index;
    u16 lookup_index;
};

}

bool OpenTypeLayout::apply_context(Run& run, size_t subtable) const
{
    if (run.depth >= largest_context_depth)
        return false;
    auto& buffer = run.buffer;
    size_t const at = run.position;
    GlyphIndex const glyph = buffer[at].glyph;
    u16 const format = u16_at(subtable);

    size_t matched[largest_match];
    size_t match_count = 0;
    size_t records_at = 0;
    u16 record_count = 0;

    auto const try_rule = [&](size_t rule, auto const& matches_input) -> bool {
        u16 const inputs = u16_at(rule);
        u16 const records = u16_at(rule + 2);
        if (inputs == 0 || inputs > largest_match)
            return false;
        matched[0] = at;
        size_t cursor = at;
        for (u16 i = 1; i < inputs; ++i) {
            if (!run.next_counted(cursor))
                return false;
            if (!matches_input(i, buffer[cursor].glyph, rule + 4 + static_cast<size_t>(i - 1) * 2))
                return false;
            matched[i] = cursor;
        }
        match_count = inputs;
        records_at = rule + 4 + static_cast<size_t>(inputs - 1) * 2;
        record_count = records;
        return true;
    };

    bool found = false;
    if (format == 1) {
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const set = subtable + u16_at(subtable + 6 + static_cast<size_t>(index) * 2);
        u16 const rules = u16_at(set);
        for (u16 r = 0; r < rules && !found; ++r) {
            found = try_rule(set + u16_at(set + 2 + static_cast<size_t>(r) * 2), [&](u16, GlyphIndex candidate, size_t where) {
                return candidate == u16_at(where);
            });
        }
    } else if (format == 2) {
        if (coverage_index(subtable + u16_at(subtable + 2), glyph) < 0)
            return false;
        size_t const definition = subtable + u16_at(subtable + 4);
        u16 const wanted_class = class_of(definition, glyph);
        if (wanted_class >= u16_at(subtable + 6))
            return false;
        u16 const set_offset = u16_at(subtable + 8 + static_cast<size_t>(wanted_class) * 2);
        if (set_offset == 0)
            return false;
        size_t const set = subtable + set_offset;
        u16 const rules = u16_at(set);
        for (u16 r = 0; r < rules && !found; ++r) {
            found = try_rule(set + u16_at(set + 2 + static_cast<size_t>(r) * 2), [&](u16, GlyphIndex candidate, size_t where) {
                return class_of(definition, candidate) == u16_at(where);
            });
        }
    } else if (format == 3) {
        u16 const inputs = u16_at(subtable + 2);
        u16 const records = u16_at(subtable + 4);
        if (inputs == 0 || inputs > largest_match)
            return false;
        if (coverage_index(subtable + u16_at(subtable + 6), glyph) < 0)
            return false;
        matched[0] = at;
        size_t cursor = at;
        for (u16 i = 1; i < inputs; ++i) {
            if (!run.next_counted(cursor))
                return false;
            if (coverage_index(subtable + u16_at(subtable + 6 + static_cast<size_t>(i) * 2), buffer[cursor].glyph) < 0)
                return false;
            matched[i] = cursor;
        }
        match_count = inputs;
        records_at = subtable + 6 + static_cast<size_t>(inputs) * 2;
        record_count = records;
        found = true;
    }
    if (!found)
        return false;

    // Apply each, at the place it says among what matched. What they do can change how many glyphs there are: what's after that moves along.
    size_t end_of_match = matched[match_count - 1] + 1;
    ++run.depth;
    for (u16 r = 0; r < record_count; ++r) {
        size_t const record = records_at + static_cast<size_t>(r) * 4;
        u16 const sequence = u16_at(record);
        u16 const lookup = u16_at(record + 2);
        if (sequence >= match_count)
            continue;
        size_t const place = matched[sequence];
        if (place >= buffer.size())
            continue;
        size_t const size_before = buffer.size();
        size_t const saved_position = run.position;
        run.position = place;
        (void)apply_lookup_at(run, lookup);
        run.position = saved_position;
        auto const delta = static_cast<i64>(buffer.size()) - static_cast<i64>(size_before);
        if (delta == 0)
            continue;
        end_of_match = static_cast<size_t>(static_cast<i64>(end_of_match) + delta);
        if (delta > 0) {
            // What it turned into is all matched now, in its place.
            size_t const extra = min(static_cast<size_t>(delta), largest_match - match_count);
            for (size_t i = match_count; i > sequence + 1u; --i)
                matched[i - 1 + extra] = matched[i - 1] + static_cast<size_t>(delta);
            for (size_t i = 0; i < extra; ++i)
                matched[sequence + 1 + i] = place + 1 + i;
            match_count += extra;
        } else {
            // What it took out isn't. As it goes by the matched positions, those are worked out again, from what's still there.
            size_t const removed = static_cast<size_t>(-delta);
            size_t const after_place = min(removed, match_count - sequence - 1);
            for (size_t i = sequence + 1 + after_place; i < match_count; ++i)
                matched[i - after_place] = matched[i] - removed;
            match_count -= after_place;
        }
    }
    --run.depth;
    // On from what's been looked at. (Nothing more applies to these from this lookup.)
    run.position = max(end_of_match, at + 1);
    return true;
}

bool OpenTypeLayout::apply_chain_context(Run& run, size_t subtable) const
{
    if (run.depth >= largest_context_depth)
        return false;
    auto& buffer = run.buffer;
    size_t const at = run.position;
    GlyphIndex const glyph = buffer[at].glyph;
    u16 const format = u16_at(subtable);

    size_t matched[largest_match];
    size_t match_count = 0;
    size_t records_at = 0;
    u16 record_count = 0;
    bool found = false;

    // By glyphs (format 1) or classes (2), which only differ in how they ask.
    auto const try_rule = [&](size_t rule, auto const& back_ok, auto const& input_ok, auto const& ahead_ok) -> bool {
        u16 const backtrack = u16_at(rule);
        size_t const inputs_at = rule + 2 + static_cast<size_t>(backtrack) * 2;
        u16 const inputs = u16_at(inputs_at);
        if (inputs == 0 || inputs > largest_match)
            return false;
        size_t const lookahead_at = inputs_at + 2 + static_cast<size_t>(inputs - 1) * 2;
        u16 const lookahead = u16_at(lookahead_at);
        size_t const counts_at = lookahead_at + 2 + static_cast<size_t>(lookahead) * 2;

        matched[0] = at;
        size_t cursor = at;
        for (u16 i = 1; i < inputs; ++i) {
            if (!run.next_counted(cursor) || !input_ok(buffer[cursor].glyph, u16_at(inputs_at + 2 + static_cast<size_t>(i - 1) * 2)))
                return false;
            matched[i] = cursor;
        }
        size_t ahead = cursor;
        for (u16 i = 0; i < lookahead; ++i) {
            if (!run.next_counted(ahead) || !ahead_ok(buffer[ahead].glyph, u16_at(lookahead_at + 2 + static_cast<size_t>(i) * 2)))
                return false;
        }
        size_t back = at;
        for (u16 i = 0; i < backtrack; ++i) {
            if (!run.previous_counted(back) || !back_ok(buffer[back].glyph, u16_at(rule + 2 + static_cast<size_t>(i) * 2)))
                return false;
        }
        match_count = inputs;
        record_count = u16_at(counts_at);
        records_at = counts_at + 2;
        return true;
    };

    if (format == 1) {
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const set = subtable + u16_at(subtable + 6 + static_cast<size_t>(index) * 2);
        u16 const rules = u16_at(set);
        auto const same = [](GlyphIndex candidate, u16 value) { return candidate == value; };
        for (u16 r = 0; r < rules && !found; ++r)
            found = try_rule(set + u16_at(set + 2 + static_cast<size_t>(r) * 2), same, same, same);
    } else if (format == 2) {
        if (coverage_index(subtable + u16_at(subtable + 2), glyph) < 0)
            return false;
        size_t const backtrack_classes = subtable + u16_at(subtable + 4);
        size_t const input_classes = subtable + u16_at(subtable + 6);
        size_t const lookahead_classes = subtable + u16_at(subtable + 8);
        u16 const wanted_class = class_of(input_classes, glyph);
        if (wanted_class >= u16_at(subtable + 10))
            return false;
        u16 const set_offset = u16_at(subtable + 12 + static_cast<size_t>(wanted_class) * 2);
        if (set_offset == 0)
            return false;
        size_t const set = subtable + set_offset;
        u16 const rules = u16_at(set);
        for (u16 r = 0; r < rules && !found; ++r) {
            found = try_rule(
                set + u16_at(set + 2 + static_cast<size_t>(r) * 2),
                [&](GlyphIndex candidate, u16 value) { return class_of(u16_at(subtable + 4) ? backtrack_classes : 0, candidate) == value; },
                [&](GlyphIndex candidate, u16 value) { return class_of(input_classes, candidate) == value; },
                [&](GlyphIndex candidate, u16 value) { return class_of(u16_at(subtable + 8) ? lookahead_classes : 0, candidate) == value; });
        }
    } else if (format == 3) {
        u16 const backtrack = u16_at(subtable + 2);
        size_t const inputs_at = subtable + 4 + static_cast<size_t>(backtrack) * 2;
        u16 const inputs = u16_at(inputs_at);
        if (inputs == 0 || inputs > largest_match)
            return false;
        size_t const lookahead_at = inputs_at + 2 + static_cast<size_t>(inputs) * 2;
        u16 const lookahead = u16_at(lookahead_at);
        size_t const counts_at = lookahead_at + 2 + static_cast<size_t>(lookahead) * 2;
        if (coverage_index(subtable + u16_at(inputs_at + 2), glyph) < 0)
            return false;
        matched[0] = at;
        size_t cursor = at;
        for (u16 i = 1; i < inputs; ++i) {
            if (!run.next_counted(cursor) || coverage_index(subtable + u16_at(inputs_at + 2 + static_cast<size_t>(i) * 2), buffer[cursor].glyph) < 0)
                return false;
            matched[i] = cursor;
        }
        size_t ahead = cursor;
        for (u16 i = 0; i < lookahead; ++i) {
            if (!run.next_counted(ahead) || coverage_index(subtable + u16_at(lookahead_at + 2 + static_cast<size_t>(i) * 2), buffer[ahead].glyph) < 0)
                return false;
        }
        size_t back = at;
        for (u16 i = 0; i < backtrack; ++i) {
            if (!run.previous_counted(back) || coverage_index(subtable + u16_at(subtable + 4 + static_cast<size_t>(i) * 2), buffer[back].glyph) < 0)
                return false;
        }
        match_count = inputs;
        record_count = u16_at(counts_at);
        records_at = counts_at + 2;
        found = true;
    }
    if (!found)
        return false;

    size_t end_of_match = matched[match_count - 1] + 1;
    ++run.depth;
    for (u16 r = 0; r < record_count; ++r) {
        size_t const record = records_at + static_cast<size_t>(r) * 4;
        u16 const sequence = u16_at(record);
        u16 const lookup = u16_at(record + 2);
        if (sequence >= match_count)
            continue;
        size_t const place = matched[sequence];
        if (place >= buffer.size())
            continue;
        size_t const size_before = buffer.size();
        size_t const saved_position = run.position;
        run.position = place;
        (void)apply_lookup_at(run, lookup);
        run.position = saved_position;
        auto const delta = static_cast<i64>(buffer.size()) - static_cast<i64>(size_before);
        if (delta == 0)
            continue;
        end_of_match = static_cast<size_t>(static_cast<i64>(end_of_match) + delta);
        if (delta > 0) {
            size_t const extra = min(static_cast<size_t>(delta), largest_match - match_count);
            for (size_t i = match_count; i > sequence + 1u; --i)
                matched[i - 1 + extra] = matched[i - 1] + static_cast<size_t>(delta);
            for (size_t i = 0; i < extra; ++i)
                matched[sequence + 1 + i] = place + 1 + i;
            match_count += extra;
        } else {
            size_t const removed = static_cast<size_t>(-delta);
            size_t const after_place = min(removed, match_count - sequence - 1);
            for (size_t i = sequence + 1 + after_place; i < match_count; ++i)
                matched[i - after_place] = matched[i] - removed;
            match_count -= after_place;
        }
    }
    --run.depth;
    run.position = max(end_of_match, at + 1);
    return true;
}

// ---- Positions.

size_t OpenTypeLayout::value_record_size(u16 format) const
{
    size_t size = 0;
    for (u16 bits = format & 0xff; bits; bits = static_cast<u16>(bits >> 1))
        size += (bits & 1) * 2;
    return size;
}

void OpenTypeLayout::apply_value_record(ShapingGlyph& glyph, size_t, size_t record, u16 format) const
{
    size_t at = record;
    auto const next = [&] {
        i16 const value = i16_at(at);
        at += 2;
        return value;
    };
    if (format & 0x0001)
        glyph.x_offset += next();
    if (format & 0x0002)
        glyph.y_offset += next();
    if (format & 0x0004)
        glyph.x_advance += next();
    if (format & 0x0008)
        glyph.y_advance += next();
    // The rest are for particular sizes, and fonts here aren't hinted.
}

namespace {

struct Anchor {
    i32 x { 0 };
    i32 y { 0 };
};

}

bool OpenTypeLayout::apply_gpos_subtable(Run& run, u16 type, size_t subtable) const
{
    auto& buffer = run.buffer;
    size_t const at = run.position;
    GlyphIndex const glyph = buffer[at].glyph;

    auto const anchor_at = [&](size_t offset) -> Anchor {
        return { i16_at(offset + 2), i16_at(offset + 4) };
    };

    switch (type) {
    case 1: { // On one.
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0)
            return false;
        u16 const format = u16_at(subtable + 4);
        if (u16_at(subtable) == 1) {
            apply_value_record(buffer[at], subtable, subtable + 6, format);
        } else if (u16_at(subtable) == 2) {
            if (index >= u16_at(subtable + 6))
                return false;
            apply_value_record(buffer[at], subtable, subtable + 8 + static_cast<size_t>(index) * value_record_size(format), format);
        } else {
            return false;
        }
        run.position = at + 1;
        return true;
    }
    case 2: { // On a pair.
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0)
            return false;
        size_t second = at;
        if (!run.next_counted(second))
            return false;
        u16 const format = u16_at(subtable);
        u16 const format_1 = u16_at(subtable + 4);
        u16 const format_2 = u16_at(subtable + 6);
        size_t const size_1 = value_record_size(format_1);
        size_t const size_2 = value_record_size(format_2);
        if (format == 1) {
            if (index >= u16_at(subtable + 8))
                return false;
            size_t const set = subtable + u16_at(subtable + 10 + static_cast<size_t>(index) * 2);
            u16 const pairs = u16_at(set);
            size_t const stride = 2 + size_1 + size_2;
            u32 low = 0, high = pairs;
            while (low < high) {
                u32 const middle = (low + high) / 2;
                size_t const pair = set + 2 + static_cast<size_t>(middle) * stride;
                u16 const value = u16_at(pair);
                if (value == buffer[second].glyph) {
                    apply_value_record(buffer[at], set, pair + 2, format_1);
                    apply_value_record(buffer[second], set, pair + 2 + size_1, format_2);
                    run.position = size_2 ? second + 1 : second;
                    return true;
                }
                if (value < buffer[second].glyph)
                    low = middle + 1;
                else
                    high = middle;
            }
            return false;
        }
        if (format == 2) {
            u16 const class_1 = class_of(subtable + u16_at(subtable + 8), glyph);
            u16 const class_2 = class_of(subtable + u16_at(subtable + 10), buffer[second].glyph);
            u16 const count_1 = u16_at(subtable + 12);
            u16 const count_2 = u16_at(subtable + 14);
            if (class_1 >= count_1 || class_2 >= count_2)
                return false;
            size_t const record = subtable + 16 + (static_cast<size_t>(class_1) * count_2 + class_2) * (size_1 + size_2);
            apply_value_record(buffer[at], subtable, record, format_1);
            apply_value_record(buffer[second], subtable, record + size_1, format_2);
            run.position = size_2 ? second + 1 : second;
            return true;
        }
        return false;
    }
    case 3: { // Joined to the next, as handwriting is.
        if (u16_at(subtable) != 1)
            return false;
        int const index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (index < 0 || index >= u16_at(subtable + 4))
            return false;
        size_t const exit_offset = u16_at(subtable + 6 + static_cast<size_t>(index) * 4 + 2);
        if (exit_offset == 0)
            return false;
        size_t next = at;
        if (!run.next_counted(next))
            return false;
        int const next_index = coverage_index(subtable + u16_at(subtable + 2), buffer[next].glyph);
        if (next_index < 0 || next_index >= u16_at(subtable + 4))
            return false;
        size_t const entry_offset = u16_at(subtable + 6 + static_cast<size_t>(next_index) * 4);
        if (entry_offset == 0)
            return false;
        auto const exit_anchor = anchor_at(subtable + exit_offset);
        auto const entry_anchor = anchor_at(subtable + entry_offset);

        // Each ends where the next begins. Left to right, the first's advance stops at its exit, and the second is moved so that its entry is at its origin; the other way, the reverse.
        auto& first = buffer[at];
        auto& second = buffer[next];
        if (run.is_right_to_left) {
            i32 const distance = exit_anchor.x + first.x_offset;
            first.x_advance -= distance;
            first.x_offset -= distance;
            second.x_advance = entry_anchor.x + second.x_offset;
        } else {
            first.x_advance = exit_anchor.x + first.x_offset;
            i32 const distance = entry_anchor.x + second.x_offset;
            second.x_advance -= distance;
            second.x_offset -= distance;
        }
        // And each is at the height of the other, with the one that's being pulled going by whichever way the lookup says.
        i32 const y = exit_anchor.y - entry_anchor.y;
        if (run.lookup_flags & flag_right_to_left) {
            first.attached_to = static_cast<i32>(next) - static_cast<i32>(at);
            first.attachment = ShapingGlyph::Cursive;
            first.y_offset = -y;
            if (second.attachment == ShapingGlyph::Cursive && second.attached_to == -first.attached_to) {
                second.attached_to = 0;
                second.attachment = ShapingGlyph::NotAttached;
            }
        } else {
            second.attached_to = static_cast<i32>(at) - static_cast<i32>(next);
            second.attachment = ShapingGlyph::Cursive;
            second.y_offset = y;
            if (first.attachment == ShapingGlyph::Cursive && first.attached_to == -second.attached_to) {
                first.attached_to = 0;
                first.attachment = ShapingGlyph::NotAttached;
            }
        }
        run.position = next;
        return true;
    }
    case 4:
    case 5:
    case 6: { // A mark on something.
        if (u16_at(subtable) != 1)
            return false;
        int const mark_index = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (mark_index < 0)
            return false;
        u16 const classes = u16_at(subtable + 6);
        size_t const marks = subtable + u16_at(subtable + 8);
        size_t const bases = subtable + u16_at(subtable + 10);
        if (mark_index >= u16_at(marks))
            return false;
        size_t const mark_record = marks + 2 + static_cast<size_t>(mark_index) * 4;
        u16 const mark_class = u16_at(mark_record);
        if (mark_class >= classes)
            return false;
        Anchor const mark_anchor = anchor_at(marks + u16_at(mark_record + 2));

        // What it goes on: back past anything else that's skipped. Marks are ignored for this unless they're what's being looked for.
        size_t base = at;
        size_t const saved_flags_guard = 0;
        (void)saved_flags_guard;
        bool found_base = false;
        for (size_t i = at; i > 0; --i) {
            auto const& candidate = buffer[i - 1];
            if (candidate.is_zwj || candidate.is_zwnj)
                continue;
            if (type == 6) {
                // A mark on a mark: the one right before, however this lookup's set up.
                if (run.is_skipped(candidate))
                    continue;
                base = i - 1;
                found_base = true;
                break;
            }
            if (candidate.glyph_class == ShapingGlyph::Mark)
                continue;
            if (type == 4 && (candidate.glyph_class == ShapingGlyph::Component))
                continue;
            base = i - 1;
            found_base = true;
            break;
        }
        if (!found_base)
            return false;
        auto const& target = buffer[base];

        Anchor base_anchor;
        if (type == 4) {
            int const base_index = coverage_index(subtable + u16_at(subtable + 4), target.glyph);
            if (base_index < 0 || base_index >= u16_at(bases))
                return false;
            size_t const offset = u16_at(bases + 2 + (static_cast<size_t>(base_index) * classes + mark_class) * 2);
            if (offset == 0)
                return false;
            base_anchor = anchor_at(bases + offset);
        } else if (type == 5) {
            int const ligature_index = coverage_index(subtable + u16_at(subtable + 4), target.glyph);
            if (ligature_index < 0 || ligature_index >= u16_at(bases))
                return false;
            size_t const attach = bases + u16_at(bases + 2 + static_cast<size_t>(ligature_index) * 2);
            u16 const components = u16_at(attach);
            if (components == 0)
                return false;
            // Which part of the ligature: the one it was on before it was one, or its last.
            auto const& mark = buffer[at];
            u16 component = static_cast<u16>(components - 1);
            if (target.ligature_id != 0 && target.ligature_id == mark.ligature_id && mark.ligature_component > 0)
                component = static_cast<u16>(min<u16>(components, mark.ligature_component) - 1);
            size_t const offset = u16_at(attach + 2 + (static_cast<size_t>(component) * classes + mark_class) * 2);
            if (offset == 0)
                return false;
            base_anchor = anchor_at(attach + offset);
        } else {
            // Only marks on the same thing as each other.
            auto const& mark = buffer[at];
            if (target.glyph_class != ShapingGlyph::Mark)
                return false;
            bool same_place = mark.ligature_id == target.ligature_id && mark.ligature_component == target.ligature_component;
            if (!same_place && mark.ligature_id != target.ligature_id && (mark.ligature_id == 0 || target.ligature_id == 0) && (mark.ligature_component == 0 || target.ligature_component == 0))
                same_place = true;
            if (!same_place)
                return false;
            int const base_index = coverage_index(subtable + u16_at(subtable + 4), target.glyph);
            if (base_index < 0 || base_index >= u16_at(bases))
                return false;
            size_t const offset = u16_at(bases + 2 + (static_cast<size_t>(base_index) * classes + mark_class) * 2);
            if (offset == 0)
                return false;
            base_anchor = anchor_at(bases + offset);
        }

        auto& mark = buffer[at];
        mark.x_offset = base_anchor.x - mark_anchor.x;
        mark.y_offset = base_anchor.y - mark_anchor.y;
        mark.attached_to = static_cast<i32>(base) - static_cast<i32>(at);
        mark.attachment = ShapingGlyph::Marked;
        run.position = at + 1;
        return true;
    }
    case 7:
        return apply_context(run, subtable);
    case 8:
        return apply_chain_context(run, subtable);
    case 9: {
        if (u16_at(subtable) != 1)
            return false;
        u16 const real_type = u16_at(subtable + 2);
        if (real_type == 9)
            return false;
        return apply_gpos_subtable(run, real_type, subtable + u32_at(subtable + 4));
    }
    default:
        return false;
    }
}

}
