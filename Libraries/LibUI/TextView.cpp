/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Appearance.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TextView.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

ScrollView* scroll_view_of(View* view)
{
    // Clip view, then scroll view.
    auto* clip = view->parent();
    return clip ? clip->parent() ? clip->parent()->as_scroll_view() : nullptr : nullptr;
}

}

TextView::TextView()
{
    m_storage.on_change = [this] {
        relayout_lines();
        if (parent())
            parent()->set_needs_layout();
        if (auto* scroll = scroll_view_of(this))
            scroll->set_needs_layout();
        set_needs_display();
        if (on_change)
            on_change();
    };
    relayout_lines();
}

TextView::~TextView()
{
    stop_blink();
}

void TextView::set_text(StringView text)
{
    m_storage.set_text(text);
    m_storage.move_caret(0, false);
    set_needs_display();
}

void TextView::set_font(FontSpec font)
{
    m_font = font;
    relayout_lines();
    set_needs_display();
}

void TextView::set_wraps(bool wraps)
{
    m_wraps = wraps;
    relayout_lines();
    set_needs_display();
}

void TextView::set_editable(bool editable)
{
    m_editable = editable;
    set_needs_display();
}

void TextView::set_text_inset(float inset)
{
    m_inset = inset;
    relayout_lines();
}

void TextView::relayout_lines()
{
    m_lines.clear();
    auto text = m_storage.text();
    float available = width() - m_inset * 2;
    m_laid_out_width = width();
    size_t start = 0;
    while (true) {
        size_t paragraph_end = start;
        while (paragraph_end < text.length() && text[paragraph_end] != '\n')
            ++paragraph_end;
        if (!m_wraps || available <= 0 || Appearance::text_width(m_font, text.substring_view(start, paragraph_end - start)) <= available) {
            m_lines.append(Line { start, paragraph_end });
        } else {
            size_t line_start = start;
            while (line_start < paragraph_end) {
                // As many characters as fit, by halves; then back to a space.
                size_t low = m_storage.next_character(line_start);
                size_t high = paragraph_end;
                while (low < high) {
                    size_t middle = low + (high - low + 1) / 2;
                    while (middle < paragraph_end && (static_cast<u8>(text[middle]) & 0xC0) == 0x80)
                        ++middle;
                    if (Appearance::text_width(m_font, text.substring_view(line_start, middle - line_start)) <= available) {
                        low = middle;
                    } else {
                        if (middle >= high)
                            --high;
                        else
                            high = middle - 1;
                        while (high > low && (static_cast<u8>(text[high]) & 0xC0) == 0x80)
                            --high;
                    }
                }
                size_t line_end = low;
                if (line_end < paragraph_end) {
                    size_t back = line_end;
                    while (back > line_start && text[back - 1] != ' ')
                        --back;
                    if (back > line_start)
                        line_end = back;
                }
                m_lines.append(Line { line_start, line_end });
                line_start = line_end;
            }
        }
        if (paragraph_end >= text.length())
            break;
        start = paragraph_end + 1;
    }
}

FloatSize TextView::intrinsic_size() const
{
    float line = Appearance::line_height(m_font);
    float inset = m_inset;
    float height_needed = static_cast<float>(m_lines.size()) * line + inset * 2;
    if (m_wraps)
        return { -1, height_needed };
    float widest = 0;
    for (auto const& entry : m_lines)
        widest = max(widest, Appearance::text_width(m_font, m_storage.text().substring_view(entry.start, entry.end - entry.start)));
    return { widest + inset * 2 + 2, height_needed };
}

void TextView::layout()
{
    if (m_laid_out_width != width()) {
        relayout_lines();
        if (auto* scroll = scroll_view_of(this))
            scroll->set_needs_layout();
    }
}

size_t TextView::line_of(size_t offset) const
{
    size_t low = 0;
    size_t high = m_lines.size();
    while (low + 1 < high) {
        size_t middle = (low + high) / 2;
        if (m_lines[middle].start <= offset)
            low = middle;
        else
            high = middle;
    }
    return low;
}

float TextView::x_in_line(Line const& line, size_t offset) const
{
    return Appearance::text_width(m_font, m_storage.text().substring_view(line.start, min(offset, line.end) - line.start));
}

size_t TextView::offset_in_line(Line const& line, float x) const
{
    size_t best = line.start;
    float best_distance = x < 0 ? -x : x;
    size_t at = line.start;
    while (at < line.end) {
        at = m_storage.next_character(at);
        float here = x_in_line(line, at);
        float distance = here > x ? here - x : x - here;
        if (distance < best_distance) {
            best = at;
            best_distance = distance;
        } else if (here > x) {
            break;
        }
    }
    return best;
}

FloatRect TextView::caret_rect(size_t offset) const
{
    float line_height = Appearance::line_height(m_font);
    size_t index = line_of(offset);
    float x = m_inset + x_in_line(m_lines[index], offset);
    float y = m_inset + static_cast<float>(index) * line_height;
    return { x, y, tokens().metric("field.caretWidth"_sv), line_height };
}

size_t TextView::offset_at(FloatPoint point) const
{
    float line_height = Appearance::line_height(m_font);
    int index = static_cast<int>((point.y - m_inset) / line_height);
    index = clamp(index, 0, static_cast<int>(m_lines.size()) - 1);
    return offset_in_line(m_lines[static_cast<size_t>(index)], point.x - m_inset);
}

void TextView::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto clip = graphics.painter().clip_bounds().to<float>();
    auto origin = graphics.painter().transform().map(FloatPoint { 0, 0 });
    float scale = graphics.scale();
    float visible_top = (clip.y - origin.y) / scale;
    float visible_bottom = (clip.bottom() - origin.y) / scale;
    graphics.fill({ 0, max(visible_top, 0.0f), width(), visible_bottom - max(visible_top, 0.0f) }, t.color("textBackground"_sv));

    float line_height = Appearance::line_height(m_font);
    float ascent = Appearance::ascent(m_font);
    int first = max(static_cast<int>((visible_top - m_inset) / line_height), 0);
    int last = min(static_cast<int>((visible_bottom - m_inset) / line_height) + 1, static_cast<int>(m_lines.size()) - 1);
    bool focused = is_first_responder();
    bool key = window() && window()->is_key() && focused;
    auto text = m_storage.text();
    size_t selection_start = m_storage.selection_start();
    size_t selection_end = m_storage.selection_end();
    for (int i = first; i <= last; ++i) {
        auto const& line = m_lines[static_cast<size_t>(i)];
        float y = m_inset + static_cast<float>(i) * line_height;
        if (m_storage.has_selection() && selection_start <= line.end && selection_end >= line.start) {
            float x0 = selection_start > line.start ? x_in_line(line, selection_start) : 0;
            float x1;
            if (selection_end > line.end)
                x1 = width() - m_inset * 2;
            else
                x1 = x_in_line(line, selection_end);
            graphics.fill({ m_inset + x0, y, x1 - x0, line_height }, t.color(key ? "selectedText"_sv : "selectedTextInactive"_sv));
        }
        graphics.draw_text(text.substring_view(line.start, line.end - line.start), m_font, { m_inset, y + ascent }, t.color("label"_sv));
    }
    if (focused && m_editable && m_caret_shown && !m_storage.has_selection()) {
        auto rect = caret_rect(m_storage.caret());
        rect.x = graphics.snap(rect.x);
        graphics.fill(rect, t.color("text"_sv));
    }
}

void TextView::restart_blink()
{
    stop_blink();
    m_caret_shown = true;
    set_needs_display();
    if (!motion_enabled())
        return;
    auto every = static_cast<u64>(tokens().metric("field.caretBlinkMilliseconds"_sv));
    m_blink_timer = Platform::the().add_timer(every, true, [this] {
        m_caret_shown = !m_caret_shown;
        set_needs_display(caret_rect(m_storage.caret()).inflated(1));
    });
}

void TextView::stop_blink()
{
    if (m_blink_timer >= 0) {
        Platform::the().cancel_timer(m_blink_timer);
        m_blink_timer = -1;
    }
}

void TextView::did_become_first_responder()
{
    restart_blink();
}

void TextView::did_resign_first_responder()
{
    stop_blink();
    m_caret_shown = false;
    m_storage.end_undo_group();
    set_needs_display();
}

void TextView::scroll_caret_into_view()
{
    if (auto* scroll = scroll_view_of(this))
        scroll->scroll_rect_into_view(caret_rect(m_storage.caret()).inflated(m_inset));
}

void TextView::moved(bool)
{
    scroll_caret_into_view();
    restart_blink();
    set_needs_display();
}

bool TextView::key_down(KeyEvent const& event)
{
    auto& storage = m_storage;
    bool shift = event.has_shift();
    auto keep_goal = [&](auto const& move_it) {
        move_it();
        moved(shift);
        return true;
    };
    auto lost_goal = [&](size_t to) {
        m_goal_x = -1;
        storage.move_caret(to, shift);
        moved(shift);
        return true;
    };
    auto vertical = [&](int by) {
        size_t index = line_of(storage.caret());
        if (m_goal_x < 0)
            m_goal_x = x_in_line(m_lines[index], storage.caret());
        int target = static_cast<int>(index) + by;
        if (target < 0) {
            storage.move_caret(0, shift);
            return;
        }
        if (target >= static_cast<int>(m_lines.size())) {
            storage.move_caret(storage.length(), shift);
            return;
        }
        storage.move_caret(offset_in_line(m_lines[static_cast<size_t>(target)], m_goal_x), shift);
    };

    if (event.has_command()) {
        switch (event.code) {
        case KeyA:
            return perform_command("selectAll"_sv);
        case KeyC:
            return perform_command("copy"_sv);
        case KeyX:
            return perform_command("cut"_sv);
        case KeyV:
            return perform_command("paste"_sv);
        case KeyZ:
            return perform_command(shift ? "redo"_sv : "undo"_sv);
        case KeyUp:
            return lost_goal(0);
        case KeyDown:
            return lost_goal(storage.length());
        case KeyLeft:
            return lost_goal(m_lines[line_of(storage.caret())].start);
        case KeyRight:
            return lost_goal(m_lines[line_of(storage.caret())].end);
        default:
            return false;
        }
    }
    switch (event.code) {
    case KeyLeft:
        if (!shift && storage.has_selection() && !event.has_option())
            return lost_goal(storage.selection_start());
        return lost_goal(event.has_option() ? storage.previous_word(storage.caret()) : storage.previous_character(storage.caret()));
    case KeyRight:
        if (!shift && storage.has_selection() && !event.has_option())
            return lost_goal(storage.selection_end());
        return lost_goal(event.has_option() ? storage.next_word(storage.caret()) : storage.next_character(storage.caret()));
    case KeyUp:
        return keep_goal([&] { vertical(-1); });
    case KeyDown:
        return keep_goal([&] { vertical(1); });
    case KeyHome:
        return lost_goal(0);
    case KeyEnd:
        return lost_goal(storage.length());
    case KeyPageUp:
    case KeyPageDown: {
        auto* scroll = scroll_view_of(this);
        int lines = scroll ? static_cast<int>(scroll->height() / Appearance::line_height(m_font)) : 20;
        return keep_goal([&] { vertical(event.code == KeyPageUp ? -lines : lines); });
    }
    default:
        break;
    }
    if (!m_editable)
        return false;
    switch (event.code) {
    case KeyBackspace:
        m_goal_x = -1;
        storage.delete_backward(event.has_option() ? storage.previous_word(storage.caret()) : storage.previous_character(storage.caret()));
        moved(false);
        return true;
    case KeyDelete:
        m_goal_x = -1;
        storage.delete_forward(event.has_option() ? storage.next_word(storage.caret()) : storage.next_character(storage.caret()));
        moved(false);
        return true;
    case KeyEnter:
        m_goal_x = -1;
        storage.replace_selection("\n"_sv, TextStorage::EditKind::Other);
        storage.end_undo_group();
        moved(false);
        return true;
    case KeyTab:
        m_goal_x = -1;
        storage.replace_selection("\t"_sv, TextStorage::EditKind::Typing);
        moved(false);
        return true;
    case KeyEscape:
        return false;
    default:
        break;
    }
    if (!event.text.is_empty() && !event.has_control()) {
        auto first = static_cast<u8>(event.text[0]);
        if (first >= 0x20 && first != 0x7F) {
            m_goal_x = -1;
            storage.replace_selection(event.text, TextStorage::EditKind::Typing);
            moved(false);
            return true;
        }
    }
    return false;
}

bool TextView::can_perform_command(StringView command) const
{
    if (command == "selectAll"_sv)
        return m_storage.length() > 0;
    if (command == "copy"_sv)
        return m_storage.has_selection();
    if (command == "cut"_sv)
        return m_editable && m_storage.has_selection();
    if (command == "paste"_sv)
        return m_editable;
    if (command == "undo"_sv)
        return m_editable && m_storage.can_undo();
    if (command == "redo"_sv)
        return m_editable && m_storage.can_redo();
    return false;
}

bool TextView::perform_command(StringView command)
{
    if (command == "selectAll"_sv) {
        m_storage.select_all();
        set_needs_display();
        return true;
    }
    if (command == "copy"_sv) {
        if (m_storage.has_selection())
            Platform::the().copy_text(m_storage.selected_text());
        return true;
    }
    if (command == "cut"_sv) {
        if (m_editable && m_storage.has_selection()) {
            Platform::the().copy_text(m_storage.selected_text());
            m_storage.replace_selection({});
        }
        return true;
    }
    if (command == "paste"_sv) {
        auto text = Platform::the().paste_text();
        if (!text.is_error() && m_editable)
            m_storage.replace_selection(text.value().view());
        moved(false);
        return true;
    }
    if (command == "undo"_sv) {
        if (m_editable)
            m_storage.undo();
        moved(false);
        return true;
    }
    if (command == "redo"_sv) {
        if (m_editable)
            m_storage.redo();
        moved(false);
        return true;
    }
    return false;
}

bool TextView::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    m_goal_x = -1;
    size_t at = offset_at(event.position);
    m_selecting = true;
    m_select_unit = min(event.click_count, 3);
    switch (m_select_unit) {
    case 1:
        m_storage.move_caret(at, event.modifiers & WS::ModifierShift);
        m_select_from = m_storage.anchor();
        break;
    case 2:
        m_storage.word_at(at, m_unit_start, m_unit_end);
        m_storage.select(m_unit_start, m_unit_end);
        break;
    default:
        m_unit_start = m_storage.line_start(at);
        m_unit_end = m_storage.line_end(at);
        m_storage.select(m_unit_start, min(m_unit_end + 1, m_storage.length()));
        break;
    }
    restart_blink();
    return true;
}

void TextView::mouse_dragged(MouseEvent const& event)
{
    if (!m_selecting)
        return;
    size_t at = offset_at(event.position);
    if (m_select_unit == 1) {
        m_storage.select(m_select_from, at);
    } else if (m_select_unit == 2) {
        size_t start, end;
        m_storage.word_at(at, start, end);
        if (at < m_unit_start)
            m_storage.select(m_unit_end, start);
        else
            m_storage.select(m_unit_start, max(end, m_unit_end));
    } else {
        size_t start = m_storage.line_start(at);
        size_t end = m_storage.line_end(at);
        if (at < m_unit_start)
            m_storage.select(m_unit_end, start);
        else
            m_storage.select(m_unit_start, max(end, m_unit_end));
    }
    scroll_caret_into_view();
    set_needs_display();
}

void TextView::mouse_up(MouseEvent const&)
{
    m_selecting = false;
}

String TextView::accessibility_value() const
{
    return MUST(String::from_view(m_storage.text()));
}

}
