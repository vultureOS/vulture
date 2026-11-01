/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/TextStorage.h>

namespace UI {

namespace {

bool is_word_character(char c)
{
    auto byte = static_cast<u8>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '\'' || byte >= 0x80;
}

}

void TextStorage::set_text(StringView text)
{
    m_text.clear();
    MUST(m_text.try_resize(text.length()));
    if (text.length() > 0)
        __builtin_memcpy(m_text.data(), text.characters_without_null_termination(), text.length());
    m_caret = m_anchor = text.length();
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
}

size_t TextStorage::next_character(size_t at) const
{
    if (at >= m_text.size())
        return m_text.size();
    ++at;
    while (at < m_text.size() && (static_cast<u8>(m_text[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

size_t TextStorage::previous_character(size_t at) const
{
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && (static_cast<u8>(m_text[at]) & 0xC0) == 0x80)
        --at;
    return at;
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

void TextStorage::apply(size_t position, size_t removed_length, StringView inserted)
{
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
                changed();
                return;
            }
        }
    }

    Edit edit { start, MUST(String::from_view(text().substring_view(start, removed_length))), MUST(String::from_view(inserted)), m_caret, m_anchor, kind };
    apply(start, removed_length, inserted);
    m_caret = m_anchor = start + inserted.length();
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
    m_caret = m_anchor = edit.position + edit.inserted.length();
    m_undo.append(move(edit));
    m_last_kind = EditKind::Other;
    changed();
}

}
