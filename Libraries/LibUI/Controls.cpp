/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibUI/Controls.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;
namespace Math = Base::Math;

namespace {

bool s_full_keyboard_access { false };

Appearance const& tokens()
{
    return Appearance::the();
}

// A whole point up, as AppKit rounds a control's size.
float ceiling(float value)
{
    return static_cast<float>(Math::ceil(static_cast<double>(value)));
}

String number_text(float value)
{
    StringBuilder builder;
    int hundredths = static_cast<int>(value * 100 + (value >= 0 ? 0.5f : -0.5f));
    if (hundredths < 0) {
        builder.append("-"_sv);
        hundredths = -hundredths;
    }
    builder.appendff("{}.{}{}", hundredths / 100, (hundredths / 10) % 10, hundredths % 10);
    return MUST(builder.to_string());
}

}

void Control::set_control_size(ControlSize size)
{
    m_size = size;
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Control::set_full_keyboard_access(bool on)
{
    s_full_keyboard_access = on;
}

bool Control::full_keyboard_access()
{
    return s_full_keyboard_access;
}

bool Control::accepts_first_responder() const
{
    return s_full_keyboard_access;
}

bool Control::looks_active() const
{
    return window() && window()->is_key();
}

bool Control::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    m_tracking = true;
    m_pressed = true;
    set_needs_display();
    return true;
}

void Control::mouse_dragged(MouseEvent const& event)
{
    if (!m_tracking)
        return;
    bool inside = FloatRect { 0, 0, width(), height() }.contains(event.position);
    if (inside != m_pressed) {
        m_pressed = inside;
        set_needs_display();
    }
}

void Control::mouse_up(MouseEvent const& event)
{
    if (!m_tracking)
        return;
    m_tracking = false;
    bool inside = FloatRect { 0, 0, width(), height() }.contains(event.position);
    m_pressed = false;
    set_needs_display();
    if (inside && is_enabled())
        activate();
}

void Control::mouse_entered()
{
    m_hovered = true;
}

void Control::mouse_exited()
{
    m_hovered = false;
}

void Control::activate()
{
    if (on_action)
        on_action();
}

bool Control::key_down(KeyEvent const& event)
{
    if (event.code == KeySpace && !event.has_command() && is_enabled()) {
        activate();
        return true;
    }
    return false;
}

// ---- Label

Label::Label(StringView text, FontSpec font)
    : m_text(MUST(String::from_view(text)))
    , m_font(font)
{
}

void Label::set_text(StringView text)
{
    m_text = MUST(String::from_view(text));
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Label::set_font(FontSpec font)
{
    m_font = font;
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Label::set_font_named(StringView name)
{
    set_font(tokens().named_font(name));
}

void Label::set_style(LabelStyle style)
{
    m_style = style;
    set_needs_display();
}

void Label::set_color_token(StringView token)
{
    m_color_token = MUST(String::from_view(token));
    set_needs_display();
}

void Label::set_alignment(TextAlignment alignment)
{
    m_alignment = alignment;
    set_needs_display();
}

void Label::set_wraps(bool wraps)
{
    m_wraps = wraps;
    if (parent())
        parent()->set_needs_layout();
}

Vector<StringView> Label::wrapped_lines(float available) const
{
    Vector<StringView> lines;
    auto text = m_text.view();
    size_t line_start = 0;
    while (line_start <= text.length()) {
        size_t line_end = line_start;
        size_t last_break = line_start;
        while (line_end < text.length() && text[line_end] != '\n') {
            if (text[line_end] == ' ')
                last_break = line_end;
            size_t next = line_end + 1;
            while (next < text.length() && (static_cast<u8>(text[next]) & 0xC0) == 0x80)
                ++next;
            if (Appearance::text_width(m_font, text.substring_view(line_start, next - line_start)) > available && line_end > line_start) {
                if (last_break > line_start)
                    line_end = last_break;
                break;
            }
            line_end = next;
        }
        lines.append(text.substring_view(line_start, line_end - line_start));
        if (line_end >= text.length())
            break;
        line_start = line_end + 1;
    }
    return lines;
}

FloatSize Label::intrinsic_size() const
{
    float line = Appearance::line_height(m_font);
    if (m_wraps && width() > 0) {
        auto lines = wrapped_lines(width());
        return { -1, ceiling(line * static_cast<float>(lines.size())) };
    }
    return { ceiling(Appearance::text_width(m_font, m_text.view())), ceiling(line) };
}

float Label::baseline() const
{
    return ceiling(Appearance::ascent(m_font));
}

void Label::draw(Graphics& graphics)
{
    Color color;
    if (!m_color_token.is_empty()) {
        color = tokens().color(m_color_token.view());
    } else {
        switch (m_style) {
        case LabelStyle::Primary:
            color = tokens().color(is_enabled() ? "label"_sv : "tertiaryLabel"_sv);
            break;
        case LabelStyle::Secondary:
            color = tokens().color("secondaryLabel"_sv);
            break;
        case LabelStyle::Tertiary:
            color = tokens().color("tertiaryLabel"_sv);
            break;
        }
    }
    float line = Appearance::line_height(m_font);
    float ascent = Appearance::ascent(m_font);
    auto place = [&](StringView text, float y) {
        float text_width = Appearance::text_width(m_font, text);
        float x = 0;
        if (m_alignment == TextAlignment::Center)
            x = (width() - text_width) / 2;
        else if (m_alignment == TextAlignment::Right)
            x = width() - text_width;
        if (text_width > width() && m_alignment == TextAlignment::Left)
            graphics.draw_text_truncated(text, m_font, { 0, y }, width(), color);
        else
            graphics.draw_text(text, m_font, { x, y }, color);
    };
    if (m_wraps) {
        auto lines = wrapped_lines(width());
        float y = ascent;
        for (auto text : lines) {
            place(text, y);
            y += line;
        }
        return;
    }
    place(m_text.view(), ceiling(ascent));
}

// ---- Button

Button::Button(StringView title, ButtonStyle style)
    : m_title(MUST(String::from_view(title)))
    , m_style(style)
{
}

void Button::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Button::set_style(ButtonStyle style)
{
    m_style = style;
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Button::set_symbol(Symbol symbol)
{
    m_symbol = symbol;
    m_has_symbol = true;
    set_needs_display();
}

void Button::set_expanded(bool expanded)
{
    m_expanded = expanded;
    set_needs_display();
}

FloatSize Button::intrinsic_size() const
{
    auto const& t = tokens();
    auto size = control_size();
    switch (m_style) {
    case ButtonStyle::Help:
    case ButtonStyle::Circular:
    case ButtonStyle::RoundedDisclosure: {
        float circle = t.metric("button.circleSize"_sv);
        return { circle, circle };
    }
    case ButtonStyle::Disclosure: {
        float disclosure = t.metric("button.disclosureSize"_sv);
        return { disclosure, disclosure };
    }
    case ButtonStyle::Textured:
        return { ceiling(Appearance::text_width(t.bezel_font(size), m_title.view()) + t.metric("button.texturedPadding"_sv) * 2), t.metric("button"_sv, size, "height"_sv) };
    case ButtonStyle::Borderless:
        return { ceiling(Appearance::text_width(t.bezel_font(size), m_title.view())), t.metric("button"_sv, size, "height"_sv) };
    case ButtonStyle::Push:
    case ButtonStyle::Default:
        break;
    }
    return { ceiling(Appearance::text_width(t.bezel_font(size), m_title.view()) + t.metric("button"_sv, size, "padding"_sv) * 2), t.metric("button"_sv, size, "height"_sv) };
}

float Button::baseline() const
{
    return tokens().metric("button"_sv, control_size(), "baseline"_sv);
}

float Button::focus_ring_radius() const
{
    switch (m_style) {
    case ButtonStyle::Help:
    case ButtonStyle::Circular:
        return height() / 2;
    default:
        return tokens().metric("button"_sv, control_size(), "radius"_sv);
    }
}

void Button::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    FloatRect shape { 0, 0, width(), height() };
    bool enabled = is_enabled();
    auto text_font = t.bezel_font(size);

    auto label_at_baseline = [&](Color color) {
        float text_width = Appearance::text_width(text_font, m_title.view());
        graphics.draw_text(m_title.view(), text_font, { (width() - text_width) / 2, t.metric("button"_sv, size, "baseline"_sv) }, color);
    };

    switch (m_style) {
    case ButtonStyle::Push:
    case ButtonStyle::Default: {
        float radius = t.metric("button"_sv, size, "radius"_sv);
        bool accent = m_style == ButtonStyle::Default && enabled && looks_active();
        StringView group;
        if (!enabled)
            group = "bezelDisabled"_sv;
        else if (accent)
            group = is_pressed() ? "bezelAccentPressed"_sv : "bezelAccent"_sv;
        else
            group = is_pressed() ? "bezelPressed"_sv : "bezel"_sv;
        graphics.draw_bezel(shape, radius, BezelPaint::from_tokens(group));
        label_at_baseline(t.color(!enabled ? "controlTextDisabled"_sv : (accent ? "controlTextDefault"_sv : "controlText"_sv)));
        return;
    }
    case ButtonStyle::Help:
    case ButtonStyle::Circular:
    case ButtonStyle::RoundedDisclosure: {
        bool round = m_style != ButtonStyle::RoundedDisclosure;
        float radius = round ? shape.height / 2 : t.metric("button"_sv, ControlSize::Regular, "radius"_sv);
        auto group = !enabled ? "bezelDisabled"_sv : (is_pressed() ? "bezelPressed"_sv : "bezel"_sv);
        graphics.draw_bezel(shape, radius, BezelPaint::from_tokens(group));
        float symbol_size = t.metric(m_style == ButtonStyle::RoundedDisclosure ? "button.chevronSymbol"_sv : "button.circleSymbol"_sv);
        FloatRect place { (width() - symbol_size) / 2, (height() - symbol_size) / 2, symbol_size, symbol_size };
        auto color = t.color(enabled ? "controlSymbol"_sv : "controlTextDisabled"_sv);
        if (m_style == ButtonStyle::Help)
            graphics.draw_symbol(Symbol::Question, place, color, t.metric("button.symbolWeight"_sv));
        else if (m_style == ButtonStyle::RoundedDisclosure)
            graphics.draw_symbol(m_expanded ? Symbol::ChevronUp : Symbol::ChevronDown, place, color, t.metric("button.symbolWeight"_sv) * 1.5f);
        else if (m_has_symbol)
            graphics.draw_symbol(m_symbol, place, color, t.metric("button.symbolWeight"_sv));
        return;
    }
    case ButtonStyle::Textured: {
        float radius = t.metric("button"_sv, size, "radius"_sv);
        graphics.fill_rounded(shape, radius, t.color(is_pressed() ? "texturedPressed"_sv : "texturedFill"_sv));
        graphics.stroke_rounded(shape, radius, graphics.pixel(), t.color("texturedBorder"_sv));
        label_at_baseline(t.color(enabled ? "controlText"_sv : "controlTextDisabled"_sv));
        return;
    }
    case ButtonStyle::Disclosure:
        graphics.draw_symbol(m_expanded ? Symbol::ChevronDown : Symbol::ChevronRight, shape.translated(t.metric("button.disclosureOffset"_sv), 0), t.color("disclosureSymbol"_sv), t.metric("button.symbolWeight"_sv));
        return;
    case ButtonStyle::Borderless:
        if (is_pressed())
            graphics.fill_rounded(shape, t.metric("button"_sv, size, "radius"_sv), t.color("texturedPressed"_sv));
        if (m_has_symbol)
            graphics.draw_symbol(m_symbol, shape, t.color("controlSymbol"_sv), t.metric("button.symbolWeight"_sv));
        else
            label_at_baseline(t.color(enabled ? "label"_sv : "tertiaryLabel"_sv));
        return;
    }
}

String Button::accessibility_label() const
{
    // One with only a symbol says what it's been told it's called.
    if (m_title.is_empty())
        return View::accessibility_label();
    return MUST(String::from_view(m_title.view()));
}

bool Button::accessibility_press()
{
    if (!is_enabled())
        return false;
    activate();
    return true;
}

void Button::activate()
{
    if (m_style == ButtonStyle::Disclosure || m_style == ButtonStyle::RoundedDisclosure)
        set_expanded(!m_expanded);
    Control::activate();
}

// ---- What a checkbox and a radio button share: a mark and a title beside it.

namespace {

FloatSize toggle_intrinsic_size(ControlSize size, StringView title)
{
    auto const& t = tokens();
    float box = t.metric("checkbox"_sv, size, "size"_sv);
    if (title.is_empty())
        return { box, box };
    auto font = t.control_font(size);
    float line = ceiling(Appearance::line_height(font));
    return { ceiling(box + t.metric("checkbox"_sv, size, "gap"_sv) + Appearance::text_width(font, title)), max(box, line) };
}

void draw_toggle_title(Graphics& graphics, ControlSize size, StringView title, bool enabled, float view_height)
{
    if (title.is_empty())
        return;
    auto const& t = tokens();
    float box = t.metric("checkbox"_sv, size, "size"_sv);
    float box_y = (view_height - box) / 2;
    graphics.draw_text(title, t.control_font(size), { box + t.metric("checkbox"_sv, size, "gap"_sv), box_y + t.metric("checkbox"_sv, size, "baseline"_sv) },
        t.color(enabled ? "label"_sv : "tertiaryLabel"_sv));
}

FloatRect toggle_box(ControlSize size, float view_height)
{
    float box = tokens().metric("checkbox"_sv, size, "size"_sv);
    return { 0, (view_height - box) / 2, box, box };
}

}

Checkbox::Checkbox(StringView title, ToggleState state)
    : m_title(MUST(String::from_view(title)))
    , m_state(state)
{
}

void Checkbox::set_state(ToggleState state)
{
    if (state == m_state)
        return;
    m_state = state;
    set_needs_display();
}

void Checkbox::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

FloatSize Checkbox::intrinsic_size() const
{
    return toggle_intrinsic_size(control_size(), m_title.view());
}

float Checkbox::baseline() const
{
    return (height() - tokens().metric("checkbox"_sv, control_size(), "size"_sv)) / 2 + tokens().metric("checkbox"_sv, control_size(), "baseline"_sv);
}

void Checkbox::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    auto box = toggle_box(size, height());
    float radius = t.metric("checkbox"_sv, size, "radius"_sv);
    bool on = m_state != ToggleState::Off;
    bool enabled = is_enabled();
    if (!enabled)
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens("checkboxDisabled"_sv));
    else if (on && looks_active())
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens(is_pressed() ? "bezelAccentPressed"_sv : "bezelAccent"_sv));
    else
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens(size == ControlSize::Mini ? "checkboxMini"_sv : "checkbox"_sv));
    if (on) {
        auto color = t.color(!enabled ? "controlTextDisabled"_sv : (looks_active() ? "checkboxMark"_sv : "checkboxMarkInactive"_sv));
        graphics.draw_symbol(m_state == ToggleState::On ? Symbol::Checkmark : Symbol::Dash, box, color, t.metric("checkbox.markWeight"_sv));
    }
    draw_toggle_title(graphics, size, m_title.view(), enabled, height());
}

FloatRect Checkbox::focus_ring_rect() const
{
    return toggle_box(control_size(), height());
}

float Checkbox::focus_ring_radius() const
{
    return tokens().metric("checkbox"_sv, control_size(), "radius"_sv);
}

String Checkbox::accessibility_value() const
{
    switch (m_state) {
    case ToggleState::Off:
        return MUST(String::from_view("off"_sv));
    case ToggleState::On:
        return MUST(String::from_view("on"_sv));
    case ToggleState::Mixed:
        return MUST(String::from_view("mixed"_sv));
    }
    VERIFY_NOT_REACHED();
}

bool Checkbox::accessibility_press()
{
    if (!is_enabled())
        return false;
    activate();
    return true;
}

void Checkbox::activate()
{
    set_state(m_state == ToggleState::On ? ToggleState::Off : ToggleState::On);
    Control::activate();
}

RadioButton::RadioButton(StringView title, bool on, int group)
    : m_title(MUST(String::from_view(title)))
    , m_on(on)
    , m_group(group)
{
}

void RadioButton::set_on(bool on)
{
    if (on == m_on)
        return;
    m_on = on;
    set_needs_display();
}

void RadioButton::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

FloatSize RadioButton::intrinsic_size() const
{
    return toggle_intrinsic_size(control_size(), m_title.view());
}

float RadioButton::baseline() const
{
    return (height() - tokens().metric("checkbox"_sv, control_size(), "size"_sv)) / 2 + tokens().metric("checkbox"_sv, control_size(), "baseline"_sv);
}

void RadioButton::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    auto box = toggle_box(size, height());
    bool enabled = is_enabled();
    float radius = box.width / 2;
    if (!enabled)
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens("checkboxDisabled"_sv));
    else if (m_on && looks_active())
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens(is_pressed() ? "bezelAccentPressed"_sv : "bezelAccent"_sv));
    else
        graphics.draw_bezel(box, radius, BezelPaint::from_tokens(size == ControlSize::Mini ? "radioMini"_sv : "radio"_sv));
    if (m_on) {
        auto color = t.color(!enabled ? "controlTextDisabled"_sv : (looks_active() ? "checkboxMark"_sv : "checkboxMarkInactive"_sv));
        graphics.fill_circle({ box.x + box.width / 2, box.y + box.height / 2 }, box.width * t.metric("radio.dotRatio"_sv) / 2, color);
    }
    draw_toggle_title(graphics, size, m_title.view(), enabled, height());
}

FloatRect RadioButton::focus_ring_rect() const
{
    return toggle_box(control_size(), height());
}

float RadioButton::focus_ring_radius() const
{
    return tokens().metric("checkbox"_sv, control_size(), "size"_sv) / 2;
}

String RadioButton::accessibility_value() const
{
    return MUST(String::from_view(m_on ? "on"_sv : "off"_sv));
}

bool RadioButton::accessibility_press()
{
    if (!is_enabled())
        return false;
    activate();
    return true;
}

void RadioButton::activate()
{
    if (m_on)
        return;
    set_on(true);
    if (parent()) {
        for (auto const& sibling : parent()->children()) {
            if (sibling.ptr() == this)
                continue;
            auto* other = sibling->as_radio_button();
            if (other && other->m_group == m_group)
                other->set_on(false);
        }
    }
    Control::activate();
}

// ---- Switch

Switch::Switch(bool on)
    : m_on(on)
    , m_position(on ? 1 : 0)
{
}

void Switch::set_on(bool on, bool animated)
{
    if (on == m_on)
        return;
    m_on = on;
    if (!animated || !motion_enabled()) {
        m_position = on ? 1 : 0;
        set_needs_display();
        return;
    }
    float from = m_position;
    float to = on ? 1 : 0;
    (void)animate(Animation::Timing::spring_with(0.25f, 0.1f), [this, from, to](float progress) {
        m_position = from + (to - from) * progress;
        set_needs_display();
    });
}

FloatSize Switch::intrinsic_size() const
{
    auto size = control_size();
    return { tokens().metric("switch"_sv, size, "width"_sv), tokens().metric("switch"_sv, size, "height"_sv) };
}

void Switch::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    FloatRect track { 0, 0, width(), height() };
    float radius = track.height / 2;
    auto off = t.color("switch.track"_sv);
    auto on = t.color(looks_active() ? "switch.trackOn"_sv : "switch.trackOnInactive"_sv);
    float position = clamp(m_position, 0.0f, 1.0f);
    // Off under, on over, by how far along the knob is.
    graphics.fill_rounded(track, radius, off);
    if (position > 0)
        graphics.fill_rounded(track, radius, on.with_opacity(position));
    graphics.stroke_rounded(track, radius, graphics.pixel(), t.color("switch.trackEdge"_sv));
    if (position < 1) {
        // Shaded a little along the top.
        graphics.save();
        graphics.clip({ 0, 0, width(), 1 });
        graphics.fill_rounded(track, radius, t.color("switch.trackTop"_sv));
        graphics.restore();
    }
    float knob = t.metric("switch"_sv, size, "knob"_sv);
    float inset = t.metric("switch.knobInset"_sv);
    float travel = width() - knob - inset * 2;
    FloatRect place { inset + travel * position, (height() - knob) / 2, knob, knob };
    graphics.draw_bezel(place, knob / 2, BezelPaint::from_tokens("knob"_sv));
    if (is_pressed())
        graphics.fill_circle({ place.x + knob / 2, place.y + knob / 2 }, knob / 2, Color { 0, 0, 0, 15 });
}

String Switch::accessibility_value() const
{
    return MUST(String::from_view(m_on ? "on"_sv : "off"_sv));
}

bool Switch::accessibility_press()
{
    if (!is_enabled())
        return false;
    activate();
    return true;
}

void Switch::activate()
{
    set_on(!m_on);
    Control::activate();
}

// ---- Segmented control

void SegmentedControl::add_segment(StringView title)
{
    m_segments.append(Segment { MUST(String::from_view(title)), false });
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void SegmentedControl::set_selected(size_t index, bool selected)
{
    if (index >= m_segments.size())
        return;
    if (m_mode == Mode::SelectOne && selected) {
        for (auto& segment : m_segments)
            segment.selected = false;
    }
    m_segments[index].selected = selected;
    set_needs_display();
}

bool SegmentedControl::is_selected(size_t index) const
{
    return index < m_segments.size() && m_segments[index].selected;
}

int SegmentedControl::selected_index() const
{
    for (size_t i = 0; i < m_segments.size(); ++i) {
        if (m_segments[i].selected)
            return static_cast<int>(i);
    }
    return -1;
}

FloatSize SegmentedControl::intrinsic_size() const
{
    auto const& t = tokens();
    auto size = control_size();
    auto font = t.bezel_font(size);
    float padding = t.metric("segmented"_sv, size, "padding"_sv);
    float total = 0;
    for (auto const& segment : m_segments)
        total += Appearance::text_width(font, segment.title.view()) + padding * 2;
    return { ceiling(total), t.metric("segmented"_sv, size, "height"_sv) };
}

float SegmentedControl::baseline() const
{
    return tokens().metric("segmented"_sv, control_size(), "baseline"_sv);
}

float SegmentedControl::focus_ring_radius() const
{
    return tokens().metric("segmented"_sv, control_size(), "radius"_sv);
}

FloatRect SegmentedControl::segment_rect(size_t index) const
{
    auto const& t = tokens();
    auto size = control_size();
    auto font = t.bezel_font(size);
    float padding = t.metric("segmented"_sv, size, "padding"_sv);
    float total = 0;
    for (auto const& segment : m_segments)
        total += Appearance::text_width(font, segment.title.view()) + padding * 2;
    // What is beyond the natural width is shared out.
    float extra = m_segments.is_empty() ? 0 : (width() - total) / static_cast<float>(m_segments.size());
    float x = 0;
    for (size_t i = 0; i < index; ++i)
        x += Appearance::text_width(font, m_segments[i].title.view()) + padding * 2 + extra;
    float segment_width = Appearance::text_width(font, m_segments[index].title.view()) + padding * 2 + extra;
    return { x, 0, segment_width, height() };
}

int SegmentedControl::segment_at(FloatPoint point) const
{
    for (size_t i = 0; i < m_segments.size(); ++i) {
        if (segment_rect(i).contains(point))
            return static_cast<int>(i);
    }
    return -1;
}

void SegmentedControl::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    FloatRect shape { 0, 0, width(), height() };
    float radius = t.metric("segmented"_sv, size, "radius"_sv);
    graphics.draw_bezel(shape, radius, BezelPaint::from_tokens("segmentedTrack"_sv));
    auto font = t.bezel_font(size);
    float baseline_y = t.metric("segmented"_sv, size, "baseline"_sv);
    bool enabled = is_enabled();

    for (size_t i = 0; i + 1 < m_segments.size(); ++i) {
        if (m_segments[i].selected || m_segments[i + 1].selected)
            continue;
        float x = graphics.snap(segment_rect(i).right());
        graphics.fill({ x, 4, graphics.pixel() * (graphics.scale() >= 2 ? 2 : 1), height() - 8 }, t.color("segmented.divider"_sv));
    }
    for (size_t i = 0; i < m_segments.size(); ++i) {
        auto rect = segment_rect(i);
        auto const& segment = m_segments[i];
        bool shown_selected = segment.selected || (m_mode == Mode::Momentary && static_cast<int>(i) == m_pressed_segment);
        if (shown_selected) {
            float inset = t.metric("segmented"_sv, size, "thumbInset"_sv);
            FloatRect thumb = graphics.snapped({ rect.x + (i == 0 ? inset : 0), rect.y + inset, rect.width - (i == 0 ? inset : 0) - (i + 1 == m_segments.size() ? inset : 0), rect.height - inset * 2 });
            bool pressed_here = static_cast<int>(i) == m_pressed_segment;
            graphics.draw_bezel(thumb, radius - inset, BezelPaint::from_tokens(pressed_here ? "bezelPressed"_sv : "bezel"_sv));
        } else if (static_cast<int>(i) == m_pressed_segment) {
            graphics.fill_rounded(rect, radius, t.color("segmented.track"_sv));
        }
        float text_width = Appearance::text_width(font, segment.title.view());
        graphics.draw_text(segment.title.view(), font, { rect.x + (rect.width - text_width) / 2, baseline_y }, t.color(enabled ? "controlText"_sv : "controlTextDisabled"_sv));
    }
}

bool SegmentedControl::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    m_pressed_segment = segment_at(event.position);
    set_needs_display();
    return true;
}

void SegmentedControl::mouse_dragged(MouseEvent const& event)
{
    int over = segment_at(event.position);
    if (over != m_pressed_segment) {
        m_pressed_segment = over;
        set_needs_display();
    }
}

void SegmentedControl::mouse_up(MouseEvent const& event)
{
    int over = segment_at(event.position);
    m_pressed_segment = -1;
    set_needs_display();
    if (over < 0 || !is_enabled())
        return;
    auto index = static_cast<size_t>(over);
    switch (m_mode) {
    case Mode::SelectOne:
        if (!m_segments[index].selected)
            set_selected(index);
        break;
    case Mode::SelectAny:
        set_selected(index, !m_segments[index].selected);
        break;
    case Mode::Momentary:
        break;
    }
    if (on_change)
        on_change(index);
    if (on_action)
        on_action();
}

bool SegmentedControl::key_down(KeyEvent const& event)
{
    if (event.code != KeyLeft && event.code != KeyRight)
        return false;
    int current = selected_index();
    int next = current + (event.code == KeyRight ? 1 : -1);
    if (next < 0 || next >= static_cast<int>(m_segments.size()))
        return true;
    set_selected(static_cast<size_t>(next));
    if (on_change)
        on_change(static_cast<size_t>(next));
    return true;
}

void SegmentedControl::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    for (size_t i = 0; i < m_segments.size(); ++i)
        parts.append(Accessibility::part_of(*this, Accessibility::Role::RadioButton, m_segments[i].title.view(), segment_rect(i), m_segments[i].selected ? "on"_sv : "off"_sv, is_enabled()));
}

String SegmentedControl::accessibility_value() const
{
    int selected = selected_index();
    if (selected < 0)
        return {};
    return MUST(String::from_view(m_segments[static_cast<size_t>(selected)].title.view()));
}

// ---- Slider

Slider::Slider(float minimum, float maximum, float value)
    : m_minimum(minimum)
    , m_maximum(maximum)
    , m_value(clamp(value, minimum, maximum))
{
}

void Slider::set_value(float value)
{
    value = clamp(value, m_minimum, m_maximum);
    if (m_ticks > 1) {
        float step = (m_maximum - m_minimum) / static_cast<float>(m_ticks - 1);
        value = m_minimum + static_cast<float>(Math::round(static_cast<double>((value - m_minimum) / step))) * step;
    }
    if (value == m_value)
        return;
    m_value = value;
    set_needs_display();
}

void Slider::set_range(float minimum, float maximum)
{
    m_minimum = minimum;
    m_maximum = maximum;
    set_value(m_value);
}

void Slider::set_tick_count(int ticks)
{
    m_ticks = ticks;
    set_value(m_value);
    set_needs_display();
}

void Slider::set_vertical(bool vertical)
{
    m_vertical = vertical;
    if (parent())
        parent()->set_needs_layout();
}

FloatSize Slider::intrinsic_size() const
{
    float height_token = tokens().metric("slider"_sv, control_size(), "height"_sv);
    if (m_vertical)
        return { height_token, -1 };
    return { -1, height_token };
}

float Slider::position_of_value() const
{
    if (m_maximum <= m_minimum)
        return 0;
    return (m_value - m_minimum) / (m_maximum - m_minimum);
}

void Slider::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    float knob_across = t.metric("slider"_sv, size, "knob"_sv);
    float bar = t.metric("slider"_sv, size, "track"_sv);
    bool ticks = m_ticks > 1;
    float knob_along = ticks ? t.metric("slider.tickKnobWidth"_sv) : knob_across;
    float length = m_vertical ? height() : width();
    float across = m_vertical ? width() : height();
    float at = position_of_value();
    if (m_vertical)
        at = 1 - at;
    // With marks, the knob's middle stops on them; they run a little short of the ends,
    // and the outermost are pushed out to the very ends.
    float tick_pad = t.metric("slider.tickPad"_sv);
    float knob_at = ticks ? tick_pad + (length - tick_pad * 2) * at - knob_along / 2 : (length - knob_along) * at;
    float center = across / 2;

    auto along_rect = [&](float start, float extent, float cross, float thickness) -> FloatRect {
        if (m_vertical)
            return { cross, start, thickness, extent };
        return { start, cross, extent, thickness };
    };

    // The track is not under the knob, which may be translucent.
    auto track = along_rect(0, length, center - bar / 2, bar);
    graphics.save();
    {
        Path outside;
        outside.add_rect({ -8, -8, width() + 16, height() + 16 });
        auto knob_hole = along_rect(knob_at, knob_along, center - knob_across / 2, knob_across);
        outside.add_rounded_rect(knob_hole, min(knob_hole.width, knob_hole.height) / 2);
        graphics.painter().clip_path(outside, FillRule::EvenOdd);
    }
    graphics.fill_rounded(track, bar / 2, t.color("slider.track"_sv));
    graphics.stroke_rounded(track, bar / 2, graphics.pixel(), t.color("slider.trackEdge"_sv));

    if (ticks) {
        float tick_length = t.metric("slider.tickLength"_sv);
        float tick_width = t.metric("slider.tickWidth"_sv);
        for (int i = 0; i < m_ticks; ++i) {
            float position = tick_pad + (length - tick_pad * 2) * static_cast<float>(i) / static_cast<float>(m_ticks - 1) - tick_width / 2;
            if (i == 0)
                position = 0;
            else if (i == m_ticks - 1)
                position = length - tick_width;
            auto tick = along_rect(graphics.snap(position), tick_width, center - tick_length / 2, tick_length);
            graphics.fill_rounded(tick, tick_width / 2, t.color("slider.tick"_sv));
        }
    } else {
        // The part below the value, in the accent.
        auto fill_color = t.color(looks_active() ? "slider.fill"_sv : "slider.fillInactive"_sv);
        FloatRect filled = m_vertical ? along_rect(knob_at + knob_along / 2, length - knob_at - knob_along / 2, center - bar / 2, bar)
                                      : along_rect(0, knob_at + knob_along / 2, center - bar / 2, bar);
        if (is_enabled())
            graphics.fill_rounded(filled, bar / 2, fill_color);
    }
    graphics.restore();
    auto knob_rect = along_rect(knob_at, knob_along, center - knob_across / 2, knob_across);
    float knob_radius = min(knob_rect.width, knob_rect.height) / 2;
    graphics.draw_bezel(knob_rect, knob_radius, BezelPaint::from_tokens("sliderKnob"_sv));
    if (is_pressed())
        graphics.fill_rounded(knob_rect, knob_radius, Color { 0, 0, 0, 15 });
}

void Slider::set_from_point(FloatPoint point)
{
    auto const& t = tokens();
    bool marks = m_ticks > 1;
    float knob = marks ? t.metric("slider.tickPad"_sv) * 2 : t.metric("slider"_sv, control_size(), "knob"_sv);
    float length = (m_vertical ? height() : width()) - knob;
    float at = ((m_vertical ? point.y : point.x) - knob / 2) / max(length, 1.0f);
    if (m_vertical)
        at = 1 - at;
    float old = m_value;
    set_value(m_minimum + clamp(at, 0.0f, 1.0f) * (m_maximum - m_minimum));
    if (old != m_value && on_action)
        on_action();
}

bool Slider::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    m_dragging = true;
    set_from_point(event.position);
    return true;
}

void Slider::mouse_dragged(MouseEvent const& event)
{
    if (m_dragging)
        set_from_point(event.position);
}

void Slider::mouse_up(MouseEvent const&)
{
    m_dragging = false;
    set_needs_display();
}

bool Slider::key_down(KeyEvent const& event)
{
    float step = m_ticks > 1 ? (m_maximum - m_minimum) / static_cast<float>(m_ticks - 1) : (m_maximum - m_minimum) / 20;
    float old = m_value;
    switch (event.code) {
    case KeyLeft:
    case KeyDown:
        set_value(m_value - step);
        break;
    case KeyRight:
    case KeyUp:
        set_value(m_value + step);
        break;
    default:
        return false;
    }
    if (old != m_value && on_action)
        on_action();
    return true;
}

String Slider::accessibility_value() const
{
    return number_text(m_value);
}

// ---- Stepper

Stepper::Stepper(int minimum, int maximum, int value)
    : m_minimum(minimum)
    , m_maximum(maximum)
    , m_value(clamp(value, minimum, maximum))
{
}

void Stepper::set_value(int value)
{
    m_value = clamp(value, m_minimum, m_maximum);
    set_needs_display();
}

FloatSize Stepper::intrinsic_size() const
{
    auto size = control_size();
    return { tokens().metric("stepper"_sv, size, "width"_sv), tokens().metric("stepper"_sv, size, "height"_sv) };
}

void Stepper::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    FloatRect shape { 0, 0, width(), height() };
    float radius = t.metric("stepper"_sv, size, "radius"_sv);
    graphics.draw_bezel(shape, radius, BezelPaint::from_tokens(is_enabled() ? "bezel"_sv : "bezelDisabled"_sv));
    if (m_pressed_part != 0) {
        graphics.save();
        graphics.clip(m_pressed_part > 0 ? FloatRect { 0, 0, width(), height() / 2 } : FloatRect { 0, height() / 2, width(), height() / 2 });
        graphics.fill_rounded(shape, radius, t.color("texturedPressed"_sv));
        graphics.restore();
    }
    auto color = t.color(is_enabled() ? "controlSymbol"_sv : "controlTextDisabled"_sv);
    float half = height() / 2;
    float weight = t.metric("button.symbolWeight"_sv) * 2.2f;
    FloatRect upper { 1.5f, 1.5f, width() - 3, half - 1 };
    FloatRect lower { 1.5f, half - 0.5f, width() - 3, half - 1 };
    graphics.draw_symbol(Symbol::ChevronUp, upper, color, weight);
    graphics.draw_symbol(Symbol::ChevronDown, lower, color, weight);
}

void Stepper::step(int by)
{
    int next = m_value + by;
    if (m_wraps) {
        if (next > m_maximum)
            next = m_minimum;
        if (next < m_minimum)
            next = m_maximum;
    }
    next = clamp(next, m_minimum, m_maximum);
    if (next == m_value)
        return;
    m_value = next;
    if (on_action)
        on_action();
}

void Stepper::stop_repeating()
{
    if (m_timer >= 0) {
        Platform::the().cancel_timer(m_timer);
        m_timer = -1;
    }
}

bool Stepper::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    m_pressed_part = event.position.y < height() / 2 ? 1 : -1;
    set_needs_display();
    step(m_pressed_part);
    auto delay = static_cast<u64>(tokens().metric("stepper.repeatDelayMilliseconds"_sv));
    m_timer = Platform::the().add_timer(delay, false, [this] {
        auto every = static_cast<u64>(tokens().metric("stepper.repeatMilliseconds"_sv));
        m_timer = Platform::the().add_timer(every, true, [this] { step(m_pressed_part); });
    });
    return true;
}

void Stepper::mouse_up(MouseEvent const&)
{
    stop_repeating();
    m_pressed_part = 0;
    set_needs_display();
}

bool Stepper::key_down(KeyEvent const& event)
{
    if (event.code == KeyUp) {
        step(1);
        return true;
    }
    if (event.code == KeyDown) {
        step(-1);
        return true;
    }
    return false;
}

String Stepper::accessibility_value() const
{
    StringBuilder builder;
    builder.appendff("{}", m_value);
    return MUST(builder.to_string());
}

// ---- Progress

ProgressBar::ProgressBar(float value)
    : m_value(clamp(value, 0.0f, 1.0f))
{
}

ProgressBar::~ProgressBar()
{
    if (m_timer >= 0)
        Platform::the().cancel_timer(m_timer);
}

void ProgressBar::set_value(float value)
{
    value = clamp(value, 0.0f, 1.0f);
    if (value == m_value)
        return;
    m_value = value;
    set_needs_display();
}

void ProgressBar::set_indeterminate(bool indeterminate)
{
    if (indeterminate == m_indeterminate)
        return;
    m_indeterminate = indeterminate;
    if (m_timer >= 0) {
        Platform::the().cancel_timer(m_timer);
        m_timer = -1;
    }
    if (indeterminate && motion_enabled()) {
        m_timer = Platform::the().add_timer(16, true, [this] {
            m_phase += 0.012f;
            if (m_phase > 1)
                m_phase -= 1;
            set_needs_display();
        });
    }
    set_needs_display();
}

void ProgressBar::set_control_size(ControlSize size)
{
    m_size = size;
    if (parent())
        parent()->set_needs_layout();
}

FloatSize ProgressBar::intrinsic_size() const
{
    return { -1, tokens().metric("progress"_sv, m_size, "height"_sv) };
}

void ProgressBar::draw(Graphics& graphics)
{
    auto const& t = tokens();
    float bar = t.metric("progress"_sv, m_size, "bar"_sv);
    // Half a point up, when it does not divide evenly: as AppKit rounds.
    FloatRect track { 0, static_cast<float>(Math::floor(static_cast<double>((height() - bar) / 2))), width(), bar };
    float radius = bar / 2;
    graphics.fill_rounded(track, radius, t.color("progress.track"_sv));
    graphics.stroke_rounded(track, radius, graphics.pixel(), t.color("progress.trackEdge"_sv));
    auto fill = t.color("progress.fill"_sv);
    if (m_indeterminate) {
        // A third of it sweeps across.
        float sweep = width() / 3;
        float x = (width() + sweep) * m_phase - sweep;
        graphics.save();
        Path shape;
        shape.add_rounded_rect(track, radius);
        graphics.painter().clip_path(shape);
        graphics.fill_rounded({ x, track.y, sweep, bar }, radius, fill);
        graphics.restore();
        return;
    }
    // Rounded ends and all, it covers the value's share of what is between them.
    if (m_value > 0)
        graphics.fill_rounded({ 0, track.y, (width() - bar) * m_value + bar, bar }, radius, fill);
}

String ProgressBar::accessibility_value() const
{
    return number_text(m_value);
}

Spinner::Spinner(ControlSize size)
    : m_size(size)
{
}

Spinner::~Spinner()
{
    if (m_timer >= 0)
        Platform::the().cancel_timer(m_timer);
}

void Spinner::set_running(bool running)
{
    if (running == (m_timer >= 0))
        return;
    if (m_timer >= 0) {
        Platform::the().cancel_timer(m_timer);
        m_timer = -1;
    }
    // On a layer it goes on whatever this program's doing, which is rather the point of a spinner.
    bool const on_layer = running && motion_enabled() && window() && window()->backend().supports_layers();
    set_wants_layer(on_layer);
    if (running && motion_enabled() && !on_layer) {
        auto every = static_cast<u64>(tokens().metric("progress.spinnerStepMilliseconds"_sv));
        m_timer = Platform::the().add_timer(every, true, [this] {
            m_step = (m_step + 1) % static_cast<int>(tokens().metric("progress.spinnerSpokes"_sv));
            set_needs_display();
        });
    }
}

void Spinner::layer_was_made()
{
    auto const spokes = tokens().metric("progress.spinnerSpokes"_sv);
    rotate_forever(tokens().metric("progress.spinnerStepMilliseconds"_sv) * spokes / 1000, static_cast<int>(spokes));
}

FloatSize Spinner::intrinsic_size() const
{
    float size = tokens().metric(m_size == ControlSize::Regular || m_size == ControlSize::Large ? "progress.spinnerRegular"_sv : "progress.spinnerSmall"_sv);
    return { size, size };
}

void Spinner::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto spinner_color = t.color("progress.spinner"_sv);
    int spokes = static_cast<int>(t.metric("progress.spinnerSpokes"_sv));
    float size = min(width(), height());
    FloatPoint center { width() / 2, height() / 2 };
    float weight = size * t.metric("progress.spinnerWeight"_sv);
    float outer = size / 2 - weight / 2;
    float inner = size * t.metric("progress.spinnerInner"_sv);
    for (int i = 0; i < spokes; ++i) {
        int age = (i - m_step + spokes) % spokes;
        float angle = static_cast<float>(i) * 2 * static_cast<float>(Math::pi) / static_cast<float>(spokes) - static_cast<float>(Math::pi) / 2;
        float s = static_cast<float>(Math::sine(static_cast<double>(angle)));
        float c = static_cast<float>(Math::cosine(static_cast<double>(angle)));
        // Darkest at the head, lightest just ahead of it.
        float fade = age == 0 ? 1 : 0.2f + static_cast<float>(age - 1) / static_cast<float>(spokes - 1) * 0.75f;
        graphics.line({ center.x + c * inner, center.y + s * inner }, { center.x + c * outer, center.y + s * outer }, weight, spinner_color.with_opacity(fade), true);
    }
}

Separator::Separator(bool vertical)
    : m_vertical(vertical)
{
}

FloatSize Separator::intrinsic_size() const
{
    float thickness = tokens().metric("separator.thickness"_sv);
    return m_vertical ? FloatSize { thickness, -1 } : FloatSize { -1, thickness };
}

void Separator::draw(Graphics& graphics)
{
    auto color = tokens().color("separator"_sv);
    if (m_vertical)
        graphics.fill(graphics.snapped({ (width() - 1) / 2, 0, 1, height() }), color);
    else
        graphics.fill(graphics.snapped({ 0, (height() - 1) / 2, width(), 1 }), color);
}

// ---- Color well

ColorWell::ColorWell(Color color)
    : m_color(color)
{
}

void ColorWell::set_color(Color color)
{
    m_color = color;
    set_needs_display();
}

FloatSize ColorWell::intrinsic_size() const
{
    return { tokens().metric("colorWell.width"_sv), tokens().metric("colorWell.height"_sv) };
}

float ColorWell::focus_ring_radius() const
{
    return tokens().metric("colorWell.radius"_sv);
}

void ColorWell::draw(Graphics& graphics)
{
    auto const& t = tokens();
    FloatRect shape { 0, 0, width(), height() };
    graphics.draw_bezel(shape, t.metric("colorWell.radius"_sv), BezelPaint::from_tokens(is_pressed() ? "bezelPressed"_sv : "bezel"_sv));
    float x = t.metric("colorWell.swatchInsetX"_sv);
    float y = t.metric("colorWell.swatchInsetY"_sv);
    float radius = t.metric("colorWell.swatchRadius"_sv);
    FloatRect swatch { x, y, width() - x * 2, height() - y * 2 };
    graphics.fill_rounded(swatch, radius, m_color);
    // The ring sits a point in, top and bottom, and at the edges at the sides.
    graphics.stroke_rounded({ swatch.x, swatch.y + 1, swatch.width, swatch.height - 2 }, radius, 1, t.color("colorWell.swatchBorder"_sv));
}

String ColorWell::accessibility_value() const
{
    StringBuilder builder;
    builder.appendff("{},{},{}", m_color.red, m_color.green, m_color.blue);
    return MUST(builder.to_string());
}

// ---- Box

Box::Box(StringView title)
    : m_title(MUST(String::from_view(title)))
{
    m_content = &add<View>();
}

void Box::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    set_needs_layout();
    set_needs_display();
}

void Box::layout()
{
    auto const& t = tokens();
    float top = m_title.is_empty() ? 0 : t.metric("box.titleHeight"_sv);
    float inset = t.metric("box.contentInset"_sv);
    m_content->set_frame({ inset, top + inset, max(width() - inset * 2, 0.0f), max(height() - top - inset * 2, 0.0f) });
}

FloatSize Box::intrinsic_size() const
{
    auto const& t = tokens();
    auto inner = m_content->preferred_size();
    float top = m_title.is_empty() ? 0 : t.metric("box.titleHeight"_sv);
    float inset = t.metric("box.contentInset"_sv);
    return { max(inner.width, 0.0f) + inset * 2, max(inner.height, 0.0f) + inset * 2 + top };
}

void Box::draw(Graphics& graphics)
{
    auto const& t = tokens();
    float top = m_title.is_empty() ? 0 : t.metric("box.titleHeight"_sv);
    FloatRect shape { 0, top, width(), height() - top };
    float radius = t.metric("box.radius"_sv);
    graphics.fill_rounded(shape, radius, t.color("box.fill"_sv));
    graphics.stroke_rounded(shape, radius, graphics.pixel(), t.color("box.border"_sv));
    if (!m_title.is_empty())
        graphics.draw_text(m_title.view(), FontSpec { t.named_font("small"_sv).size, FontWeight::Medium, false }, { t.metric("box.titleLeft"_sv), t.metric("box.titleBaseline"_sv) }, t.color("label"_sv));
}

// ---- Level

LevelIndicator::LevelIndicator(float value, float warning, float critical)
    : m_value(clamp(value, 0.0f, 1.0f))
    , m_warning(warning)
    , m_critical(critical)
{
}

void LevelIndicator::set_value(float value)
{
    m_value = clamp(value, 0.0f, 1.0f);
    set_needs_display();
}

FloatSize LevelIndicator::intrinsic_size() const
{
    return { -1, tokens().metric("level.height"_sv) };
}

void LevelIndicator::draw(Graphics& graphics)
{
    auto const& t = tokens();
    FloatRect shape { 0, 0, width(), height() };
    float radius = t.metric("level.radius"_sv);
    graphics.fill_rounded(shape, radius, t.color("level.track"_sv));
    auto color = t.color(m_value >= m_critical ? "level.critical"_sv : (m_value >= m_warning ? "level.warning"_sv : "level.normal"_sv));
    if (m_value > 0) {
        graphics.save();
        graphics.clip({ 0, 0, width() * m_value, height() });
        graphics.fill_rounded(shape, radius, color);
        graphics.restore();
    }
    graphics.stroke_rounded(shape, radius, 1, t.color("level.border"_sv));
}

String LevelIndicator::accessibility_value() const
{
    return number_text(m_value);
}

}
