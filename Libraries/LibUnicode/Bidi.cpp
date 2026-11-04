/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

// Which way text reads, when it has more than one direction in it: UAX #9, with its rule numbers. It works out a level for each character; odd levels read right to left.

namespace Unicode {

using namespace Tables;
using BC = BidiClass;

BidiClass bidi_class_of(CodePoint code)
{
    return static_cast<BidiClass>(run_value(Tables::bidi_class, bidi_class_count, code));
}

CodePoint bidi_mirrored(CodePoint code)
{
    auto const* entry = find_entry(bidi_mirror, bidi_mirror_count, 2, code);
    return entry ? entry[1] : code;
}

namespace {

constexpr u8 max_depth = 125;

bool is_isolate_initiator(BC type)
{
    return type == BC::LRI || type == BC::RLI || type == BC::FSI;
}

// X9's, which take no further part.
bool is_removed_by_x9(BC type)
{
    return type == BC::LRE || type == BC::RLE || type == BC::LRO || type == BC::RLO || type == BC::PDF || type == BC::BN;
}

BC direction_of_level(u8 level)
{
    return level & 1 ? BC::R : BC::L;
}

class Resolver {
public:
    Resolver(Span<BC const> classes, Span<CodePoint const> text)
        : m_original(classes)
        , m_text(text)
    {
    }

    ErrorOr<BidiParagraph> run(ParagraphDirection direction)
    {
        BidiParagraph result;
        TRY(result.classes.try_ensure_capacity(m_original.size()));
        for (BC type : m_original)
            TRY(result.classes.try_append(type));
        TRY(m_types.try_ensure_capacity(m_original.size()));
        for (BC type : m_original)
            TRY(m_types.try_append(type));
        TRY(m_levels.try_resize(m_original.size()));
        TRY(m_matching_pdi.try_resize(m_original.size()));
        TRY(match_isolates());

        // P2, P3 (or HL1, if it's been said)
        u8 paragraph_level = 0;
        if (direction == ParagraphDirection::RightToLeft)
            paragraph_level = 1;
        else if (direction == ParagraphDirection::Auto)
            paragraph_level = level_from_first_strong(0, m_original.size());
        m_paragraph_level = paragraph_level;
        result.paragraph_level = paragraph_level;

        TRY(explicit_levels());
        TRY(resolve_sequences());
        implicit_levels();

        for (size_t i = 0; i < m_levels.size(); ++i) {
            if (is_removed_by_x9(m_original[i]))
                m_levels[i] = bidi_level_removed;
        }
        result.levels = move(m_levels);
        return result;
    }

private:
    static constexpr size_t no_match = ~size_t(0);

    // BD9
    ErrorOr<void> match_isolates()
    {
        Vector<size_t> open;
        for (size_t i = 0; i < m_original.size(); ++i)
            m_matching_pdi[i] = no_match;
        for (size_t i = 0; i < m_original.size(); ++i) {
            if (is_isolate_initiator(m_original[i])) {
                TRY(open.try_append(i));
            } else if (m_original[i] == BC::PDI && !open.is_empty()) {
                m_matching_pdi[open.last()] = i;
                (void)open.take_last();
            }
        }
        return {};
    }

    // What the first strong character says, not counting what's in isolates. 0 if there isn't one.
    [[nodiscard]] u8 level_from_first_strong(size_t from, size_t to) const
    {
        for (size_t i = from; i < to; ++i) {
            BC const type = m_original[i];
            if (type == BC::L)
                return 0;
            if (type == BC::R || type == BC::AL)
                return 1;
            if (is_isolate_initiator(type)) {
                if (m_matching_pdi[i] == no_match)
                    return 0;
                i = m_matching_pdi[i];
            }
        }
        return 0;
    }

    struct Status {
        u8 level;
        BC override_status; // ON for none.
        bool isolate;
    };

    // X1-X8
    ErrorOr<void> explicit_levels()
    {
        Vector<Status> stack;
        TRY(stack.try_append({ m_paragraph_level, BC::ON, false }));
        size_t overflow_isolates = 0;
        size_t overflow_embeddings = 0;
        size_t valid_isolates = 0;

        auto const next_odd = [](u8 level) { return static_cast<u8>((level + 1) | 1); };
        auto const next_even = [](u8 level) { return static_cast<u8>((level + 2) & ~1); };

        for (size_t i = 0; i < m_original.size(); ++i) {
            BC const type = m_original[i];
            switch (type) {
            case BC::RLE:
            case BC::LRE:
            case BC::RLO:
            case BC::LRO: {
                m_levels[i] = stack.last().level;
                bool const rightward = type == BC::RLE || type == BC::RLO;
                u8 const level = rightward ? next_odd(stack.last().level) : next_even(stack.last().level);
                if (level <= max_depth && overflow_isolates == 0 && overflow_embeddings == 0) {
                    BC const override_status = type == BC::RLO ? BC::R : (type == BC::LRO ? BC::L : BC::ON);
                    TRY(stack.try_append({ level, override_status, false }));
                } else if (overflow_isolates == 0) {
                    ++overflow_embeddings;
                }
                break;
            }
            case BC::RLI:
            case BC::LRI:
            case BC::FSI: {
                m_levels[i] = stack.last().level;
                if (stack.last().override_status != BC::ON)
                    m_types[i] = stack.last().override_status;
                bool rightward = type == BC::RLI;
                if (type == BC::FSI) {
                    size_t const end = m_matching_pdi[i] == no_match ? m_original.size() : m_matching_pdi[i];
                    rightward = level_from_first_strong(i + 1, end) == 1;
                }
                u8 const level = rightward ? next_odd(stack.last().level) : next_even(stack.last().level);
                if (level <= max_depth && overflow_isolates == 0 && overflow_embeddings == 0) {
                    ++valid_isolates;
                    TRY(stack.try_append({ level, BC::ON, true }));
                } else {
                    ++overflow_isolates;
                }
                break;
            }
            case BC::PDI: {
                if (overflow_isolates > 0) {
                    --overflow_isolates;
                } else if (valid_isolates > 0) {
                    overflow_embeddings = 0;
                    while (!stack.last().isolate)
                        (void)stack.take_last();
                    (void)stack.take_last();
                    --valid_isolates;
                }
                m_levels[i] = stack.last().level;
                if (stack.last().override_status != BC::ON)
                    m_types[i] = stack.last().override_status;
                break;
            }
            case BC::PDF:
                m_levels[i] = stack.last().level;
                if (overflow_isolates > 0) {
                    // Nothing.
                } else if (overflow_embeddings > 0) {
                    --overflow_embeddings;
                } else if (!stack.last().isolate && stack.size() >= 2) {
                    (void)stack.take_last();
                }
                break;
            case BC::B:
                // X8: which ends it all, at the level it started.
                m_levels[i] = m_paragraph_level;
                break;
            case BC::BN:
                m_levels[i] = stack.last().level;
                break;
            default:
                m_levels[i] = stack.last().level;
                if (stack.last().override_status != BC::ON)
                    m_types[i] = stack.last().override_status;
                break;
            }
        }
        return {};
    }

    // X10: into level runs, then those into isolating run sequences, then W, N and everything for each.
    ErrorOr<void> resolve_sequences()
    {
        struct LevelRun {
            size_t first_index_in_order;
            size_t count;
        };
        // Everything that's still in the text, in order.
        Vector<size_t> kept;
        for (size_t i = 0; i < m_original.size(); ++i) {
            if (!is_removed_by_x9(m_original[i]))
                TRY(kept.try_append(i));
        }
        Vector<LevelRun> runs;
        for (size_t i = 0; i < kept.size();) {
            size_t j = i + 1;
            while (j < kept.size() && m_levels[kept[j]] == m_levels[kept[i]])
                ++j;
            TRY(runs.try_append({ i, j - i }));
            i = j;
        }
        // Which run each character starts, for going from an isolate to its match.
        Vector<size_t> run_starting_at;
        TRY(run_starting_at.try_resize(m_original.size()));
        for (size_t i = 0; i < m_original.size(); ++i)
            run_starting_at[i] = no_match;
        for (size_t r = 0; r < runs.size(); ++r)
            run_starting_at[kept[runs[r].first_index_in_order]] = r;

        for (size_t r = 0; r < runs.size(); ++r) {
            size_t const first = kept[runs[r].first_index_in_order];
            // A run that starts with a PDI that's matched is on the end of another sequence.
            if (m_original[first] == BC::PDI && is_matched_pdi(first))
                continue;
            Vector<size_t> sequence;
            size_t current = r;
            for (;;) {
                auto const& run = runs[current];
                for (size_t n = 0; n < run.count; ++n)
                    TRY(sequence.try_append(kept[run.first_index_in_order + n]));
                size_t const last = sequence.last();
                if (!is_isolate_initiator(m_original[last]) || m_matching_pdi[last] == no_match)
                    break;
                size_t const next = run_starting_at[m_matching_pdi[last]];
                if (next == no_match)
                    break;
                current = next;
            }
            TRY(resolve_sequence(sequence));
        }
        return {};
    }

    [[nodiscard]] bool is_matched_pdi(size_t index) const
    {
        // (A PDI with an isolate that opens it. Those are few enough that a look back is fine.)
        size_t depth = 0;
        for (size_t i = index; i > 0; --i) {
            BC const type = m_original[i - 1];
            if (type == BC::PDI) {
                ++depth;
            } else if (is_isolate_initiator(type)) {
                if (depth == 0)
                    return true;
                --depth;
            }
        }
        return false;
    }

    ErrorOr<void> resolve_sequence(Vector<size_t> const& sequence)
    {
        if (sequence.is_empty())
            return {};
        u8 const level = m_levels[sequence[0]];

        // sos: from the level either side of the start. What's before that's been removed doesn't count.
        u8 before_level = m_paragraph_level;
        for (size_t i = sequence[0]; i > 0; --i) {
            if (!is_removed_by_x9(m_original[i - 1])) {
                before_level = m_levels[i - 1];
                break;
            }
        }
        // eos: from what's after the end, unless it's an isolate that doesn't get closed, which goes with the paragraph.
        size_t const last = sequence.last();
        u8 after_level = m_paragraph_level;
        bool const open_ended = is_isolate_initiator(m_original[last]) && m_matching_pdi[last] == no_match;
        if (!open_ended) {
            for (size_t i = last + 1; i < m_original.size(); ++i) {
                if (!is_removed_by_x9(m_original[i])) {
                    after_level = m_levels[i];
                    break;
                }
            }
        }
        BC const sos = direction_of_level(max(level, before_level));
        BC const eos = direction_of_level(max(level, after_level));

        Vector<BC> types;
        TRY(types.try_ensure_capacity(sequence.size()));
        for (size_t index : sequence)
            TRY(types.try_append(m_types[index]));

        weak_types(types, sos, eos);
        TRY(bracket_pairs(sequence, types, sos, level));
        neutral_types(sequence, types, sos, eos, level);

        for (size_t i = 0; i < sequence.size(); ++i)
            m_types[sequence[i]] = types[i];
        return {};
    }

    void weak_types(Vector<BC>& types, BC sos, BC eos)
    {
        size_t const count = types.size();
        // W1
        BC previous = sos;
        for (size_t i = 0; i < count; ++i) {
            if (types[i] == BC::NSM)
                types[i] = is_isolate_initiator(previous) || previous == BC::PDI ? BC::ON : previous;
            previous = types[i];
        }
        // W2
        BC strong = sos;
        for (size_t i = 0; i < count; ++i) {
            if (types[i] == BC::R || types[i] == BC::L || types[i] == BC::AL)
                strong = types[i];
            else if (types[i] == BC::EN && strong == BC::AL)
                types[i] = BC::AN;
        }
        // W3
        for (size_t i = 0; i < count; ++i) {
            if (types[i] == BC::AL)
                types[i] = BC::R;
        }
        // W4
        for (size_t i = 1; i + 1 < count; ++i) {
            if (types[i] == BC::ES && types[i - 1] == BC::EN && types[i + 1] == BC::EN)
                types[i] = BC::EN;
            else if (types[i] == BC::CS && types[i - 1] == BC::EN && types[i + 1] == BC::EN)
                types[i] = BC::EN;
            else if (types[i] == BC::CS && types[i - 1] == BC::AN && types[i + 1] == BC::AN)
                types[i] = BC::AN;
        }
        // W5
        for (size_t i = 0; i < count;) {
            if (types[i] != BC::ET) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < count && types[j] == BC::ET)
                ++j;
            bool const next_to_number = (i > 0 && types[i - 1] == BC::EN) || (j < count && types[j] == BC::EN);
            if (next_to_number) {
                for (size_t k = i; k < j; ++k)
                    types[k] = BC::EN;
            }
            i = j;
        }
        // W6
        for (size_t i = 0; i < count; ++i) {
            if (types[i] == BC::ES || types[i] == BC::ET || types[i] == BC::CS)
                types[i] = BC::ON;
        }
        // W7
        strong = sos;
        for (size_t i = 0; i < count; ++i) {
            if (types[i] == BC::R || types[i] == BC::L)
                strong = types[i];
            else if (types[i] == BC::EN && strong == BC::L)
                types[i] = BC::L;
        }
        (void)eos;
    }

    // N0: brackets take the direction of what's in them.
    ErrorOr<void> bracket_pairs(Vector<size_t> const& sequence, Vector<BC>& types, BC sos, u8 level)
    {
        if (m_text.is_empty())
            return {};
        struct Pair {
            size_t opener;
            size_t closer;
        };
        struct Open {
            CodePoint closing;
            size_t at;
        };
        constexpr size_t most_open = 63;
        Open stack[most_open];
        size_t depth = 0;
        Vector<Pair> pairs;
        // BD16
        for (size_t i = 0; i < sequence.size(); ++i) {
            // A bracket that's been overridden isn't one any more.
            if (types[i] != BC::ON)
                continue;
            CodePoint const code = m_text[sequence[i]];
            auto const* entry = find_entry(bidi_brackets, bidi_brackets_count, 3, canonical_bracket(code));
            if (!entry)
                continue;
            if (entry[2]) {
                if (depth == most_open)
                    break;
                stack[depth++] = { canonical_bracket(entry[1]), i };
            } else {
                for (size_t d = depth; d > 0; --d) {
                    if (stack[d - 1].closing == canonical_bracket(code)) {
                        TRY(pairs.try_append({ stack[d - 1].at, i }));
                        depth = d - 1;
                        break;
                    }
                }
            }
        }
        // By where they open.
        for (size_t i = 1; i < pairs.size(); ++i) {
            Pair const held = pairs[i];
            size_t j = i;
            while (j > 0 && pairs[j - 1].opener > held.opener) {
                pairs[j] = pairs[j - 1];
                --j;
            }
            pairs[j] = held;
        }

        BC const embedding = direction_of_level(level);
        auto const strong_of = [](BC type) -> BC {
            if (type == BC::L)
                return BC::L;
            if (type == BC::R || type == BC::EN || type == BC::AN)
                return BC::R;
            return BC::ON;
        };
        for (auto const& pair : pairs) {
            bool found_embedding = false;
            bool found_opposite = false;
            for (size_t i = pair.opener + 1; i < pair.closer; ++i) {
                BC const strong = strong_of(types[i]);
                if (strong == BC::ON)
                    continue;
                if (strong == embedding) {
                    found_embedding = true;
                    break;
                }
                found_opposite = true;
            }
            BC resolved = BC::ON;
            if (found_embedding) {
                resolved = embedding;
            } else if (found_opposite) {
                // What's before it says which: its own direction if that's the opposite, otherwise the embedding's.
                BC context = sos;
                for (size_t i = pair.opener; i > 0; --i) {
                    BC const strong = strong_of(types[i - 1]);
                    if (strong != BC::ON) {
                        context = strong;
                        break;
                    }
                }
                resolved = context != embedding ? context : embedding;
            }
            if (resolved == BC::ON)
                continue;
            types[pair.opener] = resolved;
            types[pair.closer] = resolved;
            // Marks on them go along with them.
            for (size_t side = 0; side < 2; ++side) {
                size_t const start = side == 0 ? pair.opener : pair.closer;
                for (size_t i = start + 1; i < sequence.size() && m_original[sequence[i]] == BC::NSM; ++i)
                    types[i] = resolved;
            }
        }
        return {};
    }

    // N1, N2
    void neutral_types(Vector<size_t> const& sequence, Vector<BC>& types, BC sos, BC eos, u8 level)
    {
        (void)sequence;
        auto const is_neutral = [](BC type) {
            return type == BC::B || type == BC::S || type == BC::WS || type == BC::ON || type == BC::FSI || type == BC::LRI || type == BC::RLI || type == BC::PDI;
        };
        auto const as_strong = [](BC type) { return type == BC::EN || type == BC::AN ? BC::R : type; };
        BC const embedding = direction_of_level(level);
        size_t const count = types.size();
        for (size_t i = 0; i < count;) {
            if (!is_neutral(types[i])) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < count && is_neutral(types[j]))
                ++j;
            BC const before = i > 0 ? as_strong(types[i - 1]) : sos;
            BC const after = j < count ? as_strong(types[j]) : eos;
            BC const resolved = before == after ? before : embedding;
            for (size_t k = i; k < j; ++k)
                types[k] = resolved;
            i = j;
        }
    }

    // I1, I2
    void implicit_levels()
    {
        for (size_t i = 0; i < m_levels.size(); ++i) {
            if (is_removed_by_x9(m_original[i]))
                continue;
            BC const type = m_types[i];
            if (m_levels[i] & 1) {
                if (type == BC::L || type == BC::EN || type == BC::AN)
                    ++m_levels[i];
            } else {
                if (type == BC::R)
                    ++m_levels[i];
                else if (type == BC::AN || type == BC::EN)
                    m_levels[i] = static_cast<u8>(m_levels[i] + 2);
            }
        }
    }

    // Some brackets are the same as others once they're normalized, and match them. There are only two.
    [[nodiscard]] static CodePoint canonical_bracket(CodePoint code)
    {
        if (code == 0x2329)
            return 0x3008;
        if (code == 0x232a)
            return 0x3009;
        return code;
    }

    Span<BC const> m_original;
    Span<CodePoint const> m_text;
    Vector<BC> m_types;
    Vector<u8> m_levels;
    Vector<size_t> m_matching_pdi;
    u8 m_paragraph_level { 0 };
};

}

ErrorOr<BidiParagraph> resolve_bidi(Span<BidiClass const> classes, ParagraphDirection direction)
{
    return Resolver(classes, {}).run(direction);
}

ErrorOr<BidiParagraph> resolve_bidi(Span<CodePoint const> text, ParagraphDirection direction)
{
    Vector<BC> classes;
    TRY(classes.try_ensure_capacity(text.size()));
    for (CodePoint code : text)
        TRY(classes.try_append(bidi_class_of(code)));
    return Resolver(classes.span(), text).run(direction);
}

ErrorOr<Vector<u8>> reordering_levels(BidiParagraph const& paragraph, size_t start, size_t end)
{
    Vector<u8> levels;
    TRY(levels.try_ensure_capacity(end - start));
    for (size_t i = start; i < end; ++i)
        TRY(levels.try_append(paragraph.levels[i]));
    // L1: separators, and any white space before them or the end of the line, go back to the paragraph's level.
    bool resetting = true;
    for (size_t i = end; i > start; --i) {
        BC const type = paragraph.classes[i - 1];
        u8& level = levels[i - 1 - start];
        if (type == BC::S || type == BC::B) {
            level = paragraph.paragraph_level;
            resetting = true;
        } else if (type == BC::WS || is_isolate_initiator(type) || type == BC::PDI) {
            if (resetting)
                level = paragraph.paragraph_level;
        } else if (!is_removed_by_x9(type)) {
            resetting = false;
        }
    }
    return levels;
}

ErrorOr<Vector<size_t>> visual_order(Span<u8 const> levels)
{
    Vector<size_t> order;
    u8 highest = 0;
    u8 lowest_odd = 0xff;
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] == bidi_level_removed)
            continue;
        TRY(order.try_append(i));
        highest = max(highest, levels[i]);
        if ((levels[i] & 1) && levels[i] < lowest_odd)
            lowest_odd = levels[i];
    }
    if (lowest_odd == 0xff)
        return order;
    // L2: from the highest level to the lowest odd one, each run of at least that level is reversed.
    for (int level = highest; level >= lowest_odd; --level) {
        for (size_t i = 0; i < order.size();) {
            if (levels[order[i]] < level) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < order.size() && levels[order[j]] >= level)
                ++j;
            for (size_t a = i, b = j - 1; a < b; ++a, --b) {
                size_t const held = order[a];
                order[a] = order[b];
                order[b] = held;
            }
            i = j;
        }
    }
    return order;
}

}
