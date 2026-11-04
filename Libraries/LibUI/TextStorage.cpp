/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Unicode.h>
#include <LibUI/TextStorage.h>

namespace UI {

namespace {

bool is_word_character(char c)
{
    auto byte = static_cast<u8>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '\'' || byte >= 0x80;
}

}

TextInputHooks g_text_input_hooks;

void TextStorage::set_text(StringView text)
{
    m_text.clear();
    MUST(m_text.try_resize(text.length()));
    if (text.length() > 0)
        __builtin_memcpy(m_text.data(), text.characters_without_null_termination(), text.length());
    m_caret = m_anchor = text.length();
    m_marked_length = 0;
    if (m_rich) {
        m_runs.clear();
        m_runs.append(AttributeRun { 0, text.length(), {} });
    }
    m_undo.clear();
    m_redo.clear();
    m_last_kind = EditKind::Other;
    changed();
}

void TextStorage::changed()
{
    if (on_change)
        on_change();
}

void TextStorage::select(size_t anchor, size_t caret)
{
    m_anchor = min(anchor, m_text.size());
    m_caret = min(caret, m_text.size());
    m_last_kind = EditKind::Other;
}

void TextStorage::move_caret(size_t to, bool extend)
{
    to = min(to, m_text.size());
    m_caret = to;
    if (!extend)
        m_anchor = to;
    m_last_kind = EditKind::Other;
    m_pending_attributes = {};
}

// What's next to here isn't one byte, or even one character in Unicode's sense, but what a person would call one: an accent with its letter, a Hindi syllable's parts, a family of emoji.
// Text that's all ASCII can't have any of that, so it's only worked out for what might.
namespace {

constexpr size_t longest_character = 64;

bool is_plain_ascii_byte(char c)
{
    auto const byte = static_cast<u8>(c);
    return byte >= 0x20 && byte < 0x7f;
}

size_t next_code_point_at(Vector<char> const& text, size_t at)
{
    ++at;
    while (at < text.size() && (static_cast<u8>(text[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

size_t previous_code_point_at(Vector<char> const& text, size_t at)
{
    --at;
    while (at > 0 && (static_cast<u8>(text[at]) & 0xC0) == 0x80)
        --at;
    return at;
}

}

size_t TextStorage::next_character(size_t at) const
{
    if (at >= m_text.size())
        return m_text.size();
    if (is_plain_ascii_byte(m_text[at]) && (at + 1 >= m_text.size() || is_plain_ascii_byte(m_text[at + 1])))
        return at + 1;
    // As far ahead as anything could matter.
    size_t end = at;
    for (size_t i = 0; i < longest_character && end < m_text.size(); ++i)
        end = next_code_point_at(m_text, end);
    auto const points = Unicode::code_points_of(StringView(m_text.data() + at, end - at));
    if (points.is_error() || points.value().is_empty())
        return next_code_point_at(m_text, at);
    size_t const boundary = Unicode::next_grapheme_boundary(points.value().span(), 0);
    size_t result = at;
    for (size_t i = 0; i < boundary && result < m_text.size(); ++i)
        result = next_code_point_at(m_text, result);
    return result;
}

size_t TextStorage::previous_character(size_t at) const
{
    if (at == 0)
        return 0;
    if (at > m_text.size())
        at = m_text.size();
    if (is_plain_ascii_byte(m_text[at - 1]) && (at == m_text.size() || is_plain_ascii_byte(m_text[at])) && (at < 2 || is_plain_ascii_byte(m_text[at - 2])))
        return at - 1;
    // From as far back as anything could matter, or the start of the line: boundaries forward from there, up to here.
    size_t start = at;
    for (size_t i = 0; i < longest_character && start > 0; ++i) {
        size_t const before = previous_code_point_at(m_text, start);
        if (m_text[before] == '\n')
            break;
        start = before;
    }
    size_t result = start;
    size_t cursor = start;
    while (cursor < at) {
        result = cursor;
        cursor = next_character(cursor);
    }
    return result;
}

size_t TextStorage::next_word(size_t at) const
{
    while (at < m_text.size() && !is_word_character(m_text[at]))
        at = next_character(at);
    while (at < m_text.size() && is_word_character(m_text[at]))
        at = next_character(at);
    return at;
}

size_t TextStorage::previous_word(size_t at) const
{
    while (at > 0 && !is_word_character(m_text[previous_character(at)]))
        at = previous_character(at);
    while (at > 0 && is_word_character(m_text[previous_character(at)]))
        at = previous_character(at);
    return at;
}

size_t TextStorage::line_start(size_t at) const
{
    while (at > 0 && m_text[at - 1] != '\n')
        --at;
    return at;
}

size_t TextStorage::line_end(size_t at) const
{
    while (at < m_text.size() && m_text[at] != '\n')
        ++at;
    return at;
}

void TextStorage::word_at(size_t at, size_t& start, size_t& end) const
{
    start = end = min(at, m_text.size());
    if (at < m_text.size() && !is_word_character(m_text[at])) {
        end = next_character(at);
        return;
    }
    while (start > 0 && is_word_character(m_text[previous_character(start)]))
        start = previous_character(start);
    while (end < m_text.size() && is_word_character(m_text[end]))
        end = next_character(end);
}

void TextStorage::set_rich(bool rich)
{
    m_rich = rich;
    m_runs.clear();
    m_pending_attributes = {};
    if (rich)
        m_runs.append(AttributeRun { 0, m_text.size(), {} });
    changed();
}

TextAttributes TextStorage::attributes_at(size_t offset) const
{
    // What's before a place is what it takes after: the run that ends there, as much as one that starts there.
    TextAttributes found;
    for (auto const& run : m_runs) {
        if (offset > run.start && offset <= run.end)
            return run.attributes;
        if (offset == run.start && offset == 0)
            found = run.attributes;
    }
    return found;
}

TextAttributes TextStorage::typing_attributes() const
{
    if (m_pending_attributes.has_value())
        return m_pending_attributes.value();
    return attributes_at(has_selection() ? selection_start() + 1 : m_caret);
}

void TextStorage::split_run_at(size_t offset)
{
    for (size_t i = 0; i < m_runs.size(); ++i) {
        if (offset > m_runs[i].start && offset < m_runs[i].end) {
            AttributeRun rest { offset, m_runs[i].end, m_runs[i].attributes };
            m_runs[i].end = offset;
            m_runs.insert(i + 1, rest);
            return;
        }
    }
}

void TextStorage::join_runs()
{
    for (size_t i = 0; i < m_runs.size();) {
        if (m_runs[i].start >= m_runs[i].end && m_runs.size() > 1) {
            m_runs.remove(i);
            continue;
        }
        if (i > 0 && m_runs[i - 1].attributes == m_runs[i].attributes) {
            m_runs[i - 1].end = m_runs[i].end;
            m_runs.remove(i);
            continue;
        }
        ++i;
    }
}

void TextStorage::change_attributes(Function<void(TextAttributes&)> change)
{
    if (!m_rich)
        return;
    if (!has_selection()) {
        auto attributes = typing_attributes();
        change(attributes);
        m_pending_attributes = attributes;
        return;
    }
    Edit edit { selection_start(), MUST(String::from_view(""_sv)), MUST(String::from_view(""_sv)), m_caret, m_anchor, EditKind::Other };
    edit.runs_before = m_runs;
    size_t const start = selection_start();
    size_t const end = selection_end();
    split_run_at(start);
    split_run_at(end);
    for (auto& run : m_runs) {
        if (run.start >= start && run.end <= end)
            change(run.attributes);
    }
    join_runs();
    edit.runs_after = m_runs;
    m_redo.clear();
    m_undo.append(move(edit));
    m_last_kind = EditKind::Other;
    changed();
}

void TextStorage::apply(size_t position, size_t removed_length, StringView inserted)
{
    if (m_rich) {
        // What's typed takes after what's before it, or what's been asked for. What it replaces goes.
        auto const attributes = m_pending_attributes.has_value() ? m_pending_attributes.value() : attributes_at(position);
        m_pending_attributes = {};
        size_t const end = position + removed_length;
        split_run_at(position);
        split_run_at(end);
        for (size_t i = 0; i < m_runs.size();) {
            auto& run = m_runs[i];
            if (run.start >= position && run.end <= end && !(run.start == run.end && m_runs.size() == 1)) {
                m_runs.remove(i);
                continue;
            }
            if (run.start >= end) {
                run.start = run.start - removed_length + inserted.length();
                run.end = run.end - removed_length + inserted.length();
            }
            ++i;
        }
        if (!inserted.is_empty()) {
            size_t place = 0;
            while (place < m_runs.size() && m_runs[place].end <= position)
                ++place;
            m_runs.insert(place, AttributeRun { position, position + inserted.length(), attributes });
        }
        if (m_runs.is_empty())
            m_runs.append(AttributeRun { 0, 0, attributes });
        join_runs();
    }
    size_t old_size = m_text.size();
    size_t new_size = old_size - removed_length + inserted.length();
    if (new_size > old_size)
        MUST(m_text.try_resize(new_size));
    char* data = m_text.data();
    __builtin_memmove(data + position + inserted.length(), data + position + removed_length, old_size - position - removed_length);
    if (inserted.length() > 0)
        __builtin_memcpy(data + position, inserted.characters_without_null_termination(), inserted.length());
    if (new_size < old_size)
        MUST(m_text.try_resize(new_size));
}

void TextStorage::set_marked_text(StringView marked)
{
    if (m_marked_length == 0 && marked.is_empty())
        return;
    size_t const start = m_marked_length ? m_marked_start : selection_start();
    size_t const removed = m_marked_length ? m_marked_length : selection_end() - selection_start();
    apply(start, removed, marked);
    m_marked_start = start;
    m_marked_length = marked.length();
    m_caret = m_anchor = start + marked.length();
    changed();
}

void TextStorage::commit_marked_text(StringView committed)
{
    if (m_marked_length) {
        // What was marked goes away with nothing to show for it, then what it's become goes in as if it had been typed.
        apply(m_marked_start, m_marked_length, {});
        m_caret = m_anchor = m_marked_start;
        m_marked_length = 0;
    }
    replace_selection(committed, EditKind::Typing);
}

void TextStorage::replace_selection(StringView inserted, EditKind kind)
{
    size_t start = selection_start();
    size_t removed_length = selection_end() - start;
    if (removed_length == 0 && inserted.is_empty())
        return;
    m_redo.clear();

    bool joins = kind == m_last_kind && kind != EditKind::Other && !m_undo.is_empty();
    if (joins && kind == EditKind::Typing && removed_length == 0) {
        auto& last = m_undo.last();
        if (last.kind == EditKind::Typing && last.position + last.inserted.length() == start) {
            StringBuilder builder;
            builder.append(last.inserted.view());
            builder.append(inserted);
            last.inserted = MUST(builder.to_string());
            apply(start, 0, inserted);
            m_caret = m_anchor = start + inserted.length();
            if (m_rich)
                last.runs_after = m_runs;
            changed();
            return;
        }
    }
    if (joins && kind == EditKind::Deleting && inserted.is_empty()) {
        auto& last = m_undo.last();
        if (last.kind == EditKind::Deleting && last.inserted.is_empty()) {
            StringBuilder builder;
            if (start + removed_length == last.position) {
                builder.append(text().substring_view(start, removed_length));
                builder.append(last.removed.view());
                last.position = start;
            } else if (start == last.position) {
                builder.append(last.removed.view());
                builder.append(text().substring_view(start, removed_length));
            }
            if (!builder.view().is_empty()) {
                last.removed = MUST(builder.to_string());
                apply(start, removed_length, {});
                m_caret = m_anchor = start;
                if (m_rich)
                    last.runs_after = m_runs;
                changed();
                return;
            }
        }
    }

    Edit edit { start, MUST(String::from_view(text().substring_view(start, removed_length))), MUST(String::from_view(inserted)), m_caret, m_anchor, kind };
    if (m_rich)
        edit.runs_before = m_runs;
    apply(start, removed_length, inserted);
    m_caret = m_anchor = start + inserted.length();
    if (m_rich)
        edit.runs_after = m_runs;
    m_undo.append(move(edit));
    m_last_kind = kind;
    changed();
}

void TextStorage::delete_backward(size_t to)
{
    if (has_selection()) {
        replace_selection({}, EditKind::Other);
        return;
    }
    if (to >= m_caret)
        return;
    m_anchor = to;
    auto kept = m_last_kind;
    replace_selection({}, EditKind::Deleting);
    (void)kept;
}

void TextStorage::delete_forward(size_t to)
{
    if (has_selection()) {
        replace_selection({}, EditKind::Other);
        return;
    }
    if (to <= m_caret)
        return;
    m_anchor = to;
    replace_selection({}, EditKind::Deleting);
}

void TextStorage::undo()
{
    if (m_undo.is_empty())
        return;
    auto edit = m_undo.take_last();
    apply(edit.position, edit.inserted.length(), edit.removed.view());
    if (m_rich && !edit.runs_before.is_empty())
        m_runs = edit.runs_before;
    m_caret = edit.caret_before;
    m_anchor = edit.anchor_before;
    m_redo.append(move(edit));
    m_last_kind = EditKind::Other;
    changed();
}

void TextStorage::redo()
{
    if (m_redo.is_empty())
        return;
    auto edit = m_redo.take_last();
    apply(edit.position, edit.removed.length(), edit.inserted.view());
    if (m_rich && !edit.runs_after.is_empty())
        m_runs = edit.runs_after;
    m_caret = m_anchor = edit.position + edit.inserted.length();
    m_undo.append(move(edit));
    m_last_kind = EditKind::Other;
    changed();
}

}
