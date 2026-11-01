/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>
#include <LibUI/TextField.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

}

TextField::TextField(StringView text, FieldStyle style)
    : m_style(style)
{
    m_storage.set_text(text);
    m_storage.on_change = [this] {
        scroll_caret_into_view();
        set_needs_display();
        if (on_change)
            on_change();
    };
}

TextField::~TextField()
{
    stop_blink();
}

void TextField::set_text(StringView text)
{
    m_storage.set_text(text);
    set_needs_display();
}

void TextField::set_placeholder(StringView text)
{
    m_placeholder = MUST(String::from_view(text));
    set_needs_display();
}

void TextField::set_style(FieldStyle style)
{
    m_style = style;
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void TextField::set_secure(bool secure)
{
    m_secure = secure;
    set_needs_display();
}

void TextField::set_editable(bool editable)
{
    m_editable = editable;
    set_needs_display();
}

void TextField::select_all()
{
    m_storage.select_all();
    set_needs_display();
}

StringView TextField::shown_text() const
{
    if (!m_secure)
        return m_storage.text();
    StringBuilder builder;
    auto text = m_storage.text();
    for (size_t i = 0; i < text.length(); ++i) {
        if ((static_cast<u8>(text[i]) & 0xC0) != 0x80)
            builder.append("\xE2\x80\xA2"_sv);
    }
    m_dots = MUST(builder.to_string());
    return m_dots.view();
}

FloatSize TextField::intrinsic_size() const
{
    auto const& t = tokens();
    if (m_style == FieldStyle::Rounded)
        return { -1, t.metric("field.roundedHeight"_sv) };
    return { -1, t.metric("field"_sv, control_size(), "height"_sv) };
}

float TextField::text_baseline() const
{
    auto const& t = tokens();
    if (m_style == FieldStyle::Rounded)
        return t.metric("field.roundedBaseline"_sv);
    return t.metric("field"_sv, control_size(), "baseline"_sv);
}

float TextField::baseline() const
{
    return text_baseline();
}

float TextField::text_left() const
{
    auto const& t = tokens();
    if (m_style == FieldStyle::Rounded)
        return t.metric("field.roundedPadding"_sv);
    return t.metric("field"_sv, control_size(), "padding"_sv);
}

float TextField::text_right_margin() const
{
    return text_left();
}

float TextField::focus_ring_radius() const
{
    return m_style == FieldStyle::Rounded ? tokens().metric("field.roundedRadius"_sv) : 0;
}

void TextField::draw_face(Graphics& graphics)
{
    auto const& t = tokens();
    if (m_style == FieldStyle::Plain)
        return;
    // The edge is drawn a point in: the frame holds the whole face.
    float top = m_style == FieldStyle::Rounded ? 0 : t.metric("field"_sv, control_size(), "faceTop"_sv);
    FloatRect shape { 1, top + 1, width() - 2, height() - top - 2 };
    float radius = m_style == FieldStyle::Rounded ? t.metric("field.roundedRadius"_sv) - 1 : 0;
    auto paint = BezelPaint::from_tokens(is_enabled() ? "fieldFace"_sv : "fieldFaceDisabled"_sv);
    graphics.draw_bezel(shape, radius, paint);
}

void TextField::draw(Graphics& graphics)
{
    auto const& t = tokens();
    draw_face(graphics);
    auto font = t.control_font(control_size());
    float left = text_left();
    float available = width() - left - text_right_margin();
    float baseline_y = text_baseline();
    auto shown = shown_text();

    graphics.save();
    graphics.clip({ left - 1, 0, available + 2, height() });

    bool focused = is_first_responder();
    if (shown.is_empty() && !m_placeholder.is_empty()) {
        graphics.draw_text(m_placeholder.view(), font, { left, baseline_y }, t.color("placeholder"_sv));
    } else {
        if (m_storage.has_selection()) {
            float x0 = x_of_offset(m_storage.selection_start());
            float x1 = x_of_offset(m_storage.selection_end());
            float line = Appearance::line_height(font);
            float top = baseline_y - Appearance::ascent(font);
            bool key = window() && window()->is_key() && focused;
            graphics.fill({ left + x0 - m_scroll, top, x1 - x0, line }, t.color(key ? "selectedText"_sv : "selectedTextInactive"_sv));
        }
        graphics.draw_text(shown, font, { left - m_scroll, baseline_y }, t.color(is_enabled() ? text_color_token() : "tertiaryLabel"_sv));
    }
    if (focused && m_caret_shown && !m_storage.has_selection()) {
        float caret_x = graphics.snap(left + x_of_offset(m_storage.caret()) - m_scroll);
        float caret_width = t.metric("field.caretWidth"_sv);
        float line = Appearance::line_height(font);
        graphics.fill({ caret_x, baseline_y - Appearance::ascent(font), caret_width, line }, t.color("text"_sv));
    }
    graphics.restore();
    draw_decorations(graphics);
}

float TextField::x_of_offset(size_t offset) const
{
    auto font = tokens().control_font(control_size());
    if (!m_secure)
        return Appearance::text_width(font, m_storage.text().substring_view(0, offset));
    // As many dots as characters before it.
    size_t characters = 0;
    auto text = m_storage.text();
    for (size_t i = 0; i < offset && i < text.length(); ++i) {
        if ((static_cast<u8>(text[i]) & 0xC0) != 0x80)
            ++characters;
    }
    return static_cast<float>(characters) * Appearance::text_width(font, "\xE2\x80\xA2"_sv);
}

size_t TextField::offset_at_x(float x) const
{
    // The boundary nearest it; every prefix is shaped, so kerning counts.
    size_t best = 0;
    float best_distance = x < 0 ? -x : x;
    size_t at = 0;
    while (at < m_storage.length()) {
        at = m_storage.next_character(at);
        float distance = x_of_offset(at) - x;
        if (distance < 0)
            distance = -distance;
        if (distance < best_distance) {
            best = at;
            best_distance = distance;
        } else if (x_of_offset(at) > x) {
            break;
        }
    }
    return best;
}

void TextField::scroll_caret_into_view()
{
    float available = width() - text_left() - text_right_margin();
    if (available <= 0)
        return;
    float caret = x_of_offset(m_storage.caret());
    if (caret - m_scroll > available)
        m_scroll = caret - available + 1;
    else if (caret < m_scroll)
        m_scroll = caret;
    float total = x_of_offset(m_storage.length());
    if (total <= available)
        m_scroll = 0;
    else if (m_scroll > total - available + 1)
        m_scroll = total - available + 1;
}

void TextField::restart_blink()
{
    stop_blink();
    m_caret_shown = true;
    set_needs_display();
    if (!motion_enabled())
        return;
    auto every = static_cast<u64>(tokens().metric("field.caretBlinkMilliseconds"_sv));
    m_blink_timer = Platform::the().add_timer(every, true, [this] {
        m_caret_shown = !m_caret_shown;
        set_needs_display();
    });
}

void TextField::stop_blink()
{
    if (m_blink_timer >= 0) {
        Platform::the().cancel_timer(m_blink_timer);
        m_blink_timer = -1;
    }
}

void TextField::did_become_first_responder()
{
    restart_blink();
    m_storage.select_all();
    set_needs_display();
}

void TextField::did_resign_first_responder()
{
    stop_blink();
    m_caret_shown = false;
    m_storage.end_undo_group();
    m_storage.move_caret(m_storage.caret(), false);
    set_needs_display();
    if (on_commit)
        on_commit();
}

bool TextField::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft || !m_editable)
        return false;
    size_t at = offset_at_x(event.position.x - text_left() + m_scroll);
    m_selecting = true;
    m_select_unit = min(event.click_count, 3);
    switch (m_select_unit) {
    case 1:
        m_storage.move_caret(at, event.modifiers & WS::ModifierShift);
        m_select_from = m_storage.anchor();
        break;
    case 2:
        m_storage.word_at(at, m_word_start, m_word_end);
        m_storage.select(m_word_start, m_word_end);
        break;
    default:
        m_storage.select_all();
        break;
    }
    // (Becoming first responder selects it all: that happens after this, on the first
    // click only; the click's own effect wins.)
    m_pending_click_selection = m_select_unit == 1 && !is_first_responder();
    m_pending_click_offset = at;
    restart_blink();
    return true;
}

void TextField::mouse_dragged(MouseEvent const& event)
{
    if (!m_selecting)
        return;
    if (m_pending_click_selection) {
        m_pending_click_selection = false;
        m_storage.move_caret(m_pending_click_offset, false);
        m_select_from = m_pending_click_offset;
    }
    size_t at = offset_at_x(event.position.x - text_left() + m_scroll);
    if (m_select_unit == 1) {
        m_storage.select(m_select_from, at);
    } else if (m_select_unit == 2) {
        size_t start, end;
        m_storage.word_at(at, start, end);
        if (at < m_word_start)
            m_storage.select(m_word_end, start);
        else
            m_storage.select(m_word_start, max(end, m_word_end));
    }
    scroll_caret_into_view();
    set_needs_display();
}

void TextField::mouse_up(MouseEvent const&)
{
    m_selecting = false;
    if (m_pending_click_selection) {
        m_pending_click_selection = false;
        m_storage.move_caret(m_pending_click_offset, false);
        set_needs_display();
    }
}

bool TextField::key_down(KeyEvent const& event)
{
    if (!m_editable || !is_enabled())
        return false;
    bool shift = event.has_shift();
    auto& storage = m_storage;
    auto after_move = [&] {
        scroll_caret_into_view();
        restart_blink();
        return true;
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
        case KeyLeft:
        case KeyHome:
            storage.move_caret(0, shift);
            return after_move();
        case KeyRight:
        case KeyEnd:
            storage.move_caret(storage.length(), shift);
            return after_move();
        case KeyBackspace:
            storage.delete_backward(0);
            return true;
        default:
            return false;
        }
    }

    switch (event.code) {
    case KeyLeft:
        if (!shift && storage.has_selection() && !event.has_option())
            storage.move_caret(storage.selection_start(), false);
        else
            storage.move_caret(event.has_option() ? storage.previous_word(storage.caret()) : storage.previous_character(storage.caret()), shift);
        return after_move();
    case KeyRight:
        if (!shift && storage.has_selection() && !event.has_option())
            storage.move_caret(storage.selection_end(), false);
        else
            storage.move_caret(event.has_option() ? storage.next_word(storage.caret()) : storage.next_character(storage.caret()), shift);
        return after_move();
    case KeyUp:
    case KeyHome:
        storage.move_caret(0, shift);
        return after_move();
    case KeyDown:
    case KeyEnd:
        storage.move_caret(storage.length(), shift);
        return after_move();
    case KeyBackspace:
        storage.delete_backward(event.has_option() ? storage.previous_word(storage.caret()) : storage.previous_character(storage.caret()));
        restart_blink();
        return true;
    case KeyDelete:
        storage.delete_forward(event.has_option() ? storage.next_word(storage.caret()) : storage.next_character(storage.caret()));
        restart_blink();
        return true;
    case KeyEnter:
        storage.end_undo_group();
        if (on_commit)
            on_commit();
        // A form's default button goes on to get this.
        return false;
    case KeyTab:
    case KeyEscape:
        return false;
    default:
        break;
    }
    if (!event.text.is_empty() && !event.has_control()) {
        auto first = static_cast<u8>(event.text[0]);
        if (first >= 0x20 && first != 0x7F) {
            storage.replace_selection(event.text, TextStorage::EditKind::Typing);
            restart_blink();
            return true;
        }
    }
    return false;
}

bool TextField::can_perform_command(StringView command) const
{
    if (command == "selectAll"_sv)
        return m_storage.length() > 0;
    if (command == "copy"_sv || command == "cut"_sv)
        return !m_secure && m_storage.has_selection();
    if (command == "paste"_sv)
        return m_editable;
    if (command == "undo"_sv)
        return m_storage.can_undo();
    if (command == "redo"_sv)
        return m_storage.can_redo();
    return false;
}

bool TextField::perform_command(StringView command)
{
    if (command == "selectAll"_sv) {
        select_all();
        return true;
    }
    if (command == "copy"_sv) {
        if (m_storage.has_selection() && !m_secure)
            Platform::the().copy_text(m_storage.selected_text());
        return true;
    }
    if (command == "cut"_sv) {
        if (m_storage.has_selection() && !m_secure) {
            Platform::the().copy_text(m_storage.selected_text());
            m_storage.replace_selection({});
        }
        return true;
    }
    if (command == "paste"_sv) {
        auto text = Platform::the().paste_text();
        if (!text.is_error() && m_editable) {
            // One line: a pasted newline ends it.
            auto pasted = text.value().view();
            size_t length = 0;
            while (length < pasted.length() && pasted[length] != '\n' && pasted[length] != '\r')
                ++length;
            m_storage.replace_selection(pasted.substring_view(0, length));
        }
        return true;
    }
    if (command == "undo"_sv) {
        m_storage.undo();
        return true;
    }
    if (command == "redo"_sv) {
        m_storage.redo();
        return true;
    }
    return false;
}

String TextField::accessibility_label() const
{
    auto label = View::accessibility_label();
    if (!label.is_empty())
        return label;
    return MUST(String::from_view(m_placeholder.view()));
}

String TextField::accessibility_value() const
{
    if (m_secure)
        return {};
    return MUST(String::from_view(m_storage.text()));
}

// ---- Search field

SearchField::SearchField()
    : TextField({}, FieldStyle::Rounded)
{
    set_placeholder("Search"_sv);
}

FloatSize SearchField::intrinsic_size() const
{
    return { -1, tokens().metric("search"_sv, control_size(), "height"_sv) };
}

float SearchField::text_baseline() const
{
    return tokens().metric("search"_sv, control_size(), "baseline"_sv);
}

float SearchField::baseline() const
{
    return text_baseline();
}

float SearchField::text_left() const
{
    return tokens().metric("search"_sv, control_size(), "textLeft"_sv);
}

float SearchField::text_right_margin() const
{
    return tokens().metric("search"_sv, control_size(), "iconSize"_sv) + tokens().metric("search"_sv, control_size(), "iconLeft"_sv) + 2;
}

float SearchField::focus_ring_radius() const
{
    return tokens().metric("search"_sv, control_size(), "radius"_sv);
}

void SearchField::draw_face(Graphics& graphics)
{
    auto const& t = tokens();
    float top = t.metric("search"_sv, control_size(), "faceTop"_sv);
    FloatRect shape { 1, top + 1, width() - 2, height() - top - 2 };
    graphics.draw_bezel(shape, t.metric("search"_sv, control_size(), "radius"_sv) - 1, BezelPaint::from_tokens("fieldFace"_sv));
}

FloatRect SearchField::clear_rect() const
{
    auto const& t = tokens();
    float icon = t.metric("search"_sv, control_size(), "iconSize"_sv);
    return { width() - icon - t.metric("search"_sv, control_size(), "iconLeft"_sv), (height() - icon) / 2, icon, icon };
}

void SearchField::draw_decorations(Graphics& graphics)
{
    auto const& t = tokens();
    float icon = t.metric("search"_sv, control_size(), "iconSize"_sv);
    FloatRect magnifier { t.metric("search"_sv, control_size(), "iconLeft"_sv), (height() - icon) / 2, icon, icon };
    graphics.draw_symbol(Symbol::Magnifier, magnifier, t.color("search.symbol"_sv), 1.1f);
    if (!text().is_empty())
        graphics.draw_symbol(Symbol::ClearCircle, clear_rect(), t.color("search.clear"_sv));
}

bool SearchField::mouse_down(MouseEvent const& event)
{
    if (!text().is_empty() && clear_rect().contains(event.position)) {
        set_text({});
        if (on_change)
            on_change();
        return true;
    }
    return TextField::mouse_down(event);
}

}
