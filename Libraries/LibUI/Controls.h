/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Appearance.h>
#include <LibUI/Graphics.h>
#include <LibUI/View.h>

// The controls: buttons and what acts like one, sliders, and things that only show
// how much. Each comes in the four sizes, with the sizes and colors in the theme.

namespace UI {

class Control : public View {
public:
    [[nodiscard]] ControlSize control_size() const { return m_size; }
    void set_control_size(ControlSize);

    // What pressing it does.
    Function<void()> on_action;

    // Tabbing reaches text fields and lists; other controls too with this on (System
    // Settings' "Keyboard navigation").
    static void set_full_keyboard_access(bool);
    [[nodiscard]] static bool full_keyboard_access();
    [[nodiscard]] bool accepts_first_responder() const override;

    [[nodiscard]] Insets overflow() const override { return { 2.5f, 2.5f, 3, 2.5f }; }
    bool key_down(KeyEvent const&) override;

protected:
    Control() = default;

    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    void mouse_entered() override;
    void mouse_exited() override;

    // A press, let go still inside. By default: on_action.
    virtual void activate();

    [[nodiscard]] bool is_pressed() const { return m_pressed; }
    [[nodiscard]] bool is_hovered() const { return m_hovered; }
    // In a window that has the keyboard: otherwise accent colors go grey.
    [[nodiscard]] bool looks_active() const;

private:
    ControlSize m_size { ControlSize::Regular };
    bool m_tracking { false };
    bool m_pressed { false };
    bool m_hovered { false };
};

enum class LabelStyle : u8 {
    Primary,
    Secondary,
    Tertiary,
};

// Text that is only read: one line, or wrapped to its width.
class Label : public View {
public:
    explicit Label(StringView text = {}, FontSpec = {});

    void set_text(StringView);
    [[nodiscard]] StringView text() const { return m_text.view(); }
    void set_font(FontSpec);
    void set_font_named(StringView);
    [[nodiscard]] FontSpec font() const { return m_font; }
    void set_style(LabelStyle);
    // A token by name, instead of a style.
    void set_color_token(StringView);
    void set_alignment(TextAlignment);
    void set_wraps(bool);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::StaticText; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_text.view())); }

private:
    [[nodiscard]] Vector<StringView> wrapped_lines(float width) const;

    String m_text;
    FontSpec m_font;
    LabelStyle m_style { LabelStyle::Primary };
    String m_color_token;
    TextAlignment m_alignment { TextAlignment::Left };
    bool m_wraps { false };
};

enum class ButtonStyle : u8 {
    Push,
    // The one Enter presses: the accent's.
    Default,
    Help,
    Circular,
    Textured,
    Disclosure,
    RoundedDisclosure,
    Borderless,
};

class Button : public Control {
public:
    explicit Button(StringView title = {}, ButtonStyle = ButtonStyle::Push);

    void set_title(StringView);
    [[nodiscard]] StringView title() const { return m_title.view(); }
    void set_style(ButtonStyle);
    [[nodiscard]] ButtonStyle style() const { return m_style; }
    void set_symbol(Symbol);
    // A disclosure button's: open or not.
    void set_expanded(bool);
    [[nodiscard]] bool is_expanded() const { return m_expanded; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override;
    bool accessibility_press() override;

protected:
    void activate() override;

private:
    String m_title;
    ButtonStyle m_style;
    Symbol m_symbol { Symbol::Plus };
    bool m_has_symbol { false };
    bool m_expanded { false };
};

enum class ToggleState : u8 {
    Off,
    On,
    Mixed,
};

class Checkbox : public Control {
public:
    explicit Checkbox(StringView title = {}, ToggleState = ToggleState::Off);

    void set_state(ToggleState);
    [[nodiscard]] ToggleState state() const { return m_state; }
    [[nodiscard]] bool is_on() const { return m_state == ToggleState::On; }
    void set_title(StringView);
    [[nodiscard]] StringView title() const { return m_title.view(); }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Gfx::FloatRect focus_ring_rect() const override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Checkbox; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }
    [[nodiscard]] String accessibility_value() const override;
    bool accessibility_press() override;

protected:
    void activate() override;

private:
    String m_title;
    ToggleState m_state;
};

// Those with the same parent and group are one choice: choosing one clears the rest.
class RadioButton : public Control {
public:
    explicit RadioButton(StringView title = {}, bool on = false, int group = 0);

    void set_on(bool);
    [[nodiscard]] bool is_on() const { return m_on; }
    void set_title(StringView);
    [[nodiscard]] StringView title() const { return m_title.view(); }
    [[nodiscard]] RadioButton* as_radio_button() override { return this; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Gfx::FloatRect focus_ring_rect() const override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::RadioButton; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }
    [[nodiscard]] String accessibility_value() const override;
    bool accessibility_press() override;

protected:
    void activate() override;

private:
    String m_title;
    bool m_on;
    int m_group;
};

class Switch : public Control {
public:
    explicit Switch(bool on = false);

    void set_on(bool, bool animated = true);
    [[nodiscard]] bool is_on() const { return m_on; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] float focus_ring_radius() const override { return height() / 2; }
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Switch; }
    [[nodiscard]] String accessibility_value() const override;
    bool accessibility_press() override;

protected:
    void activate() override;

private:
    bool m_on;
    float m_position;
};

class SegmentedControl : public Control {
public:
    enum class Mode : u8 {
        // One at a time.
        SelectOne,
        // Any number.
        SelectAny,
        // Pressed only while pressed.
        Momentary,
    };

    SegmentedControl() = default;

    void add_segment(StringView title);
    void set_mode(Mode mode) { m_mode = mode; }
    void set_selected(size_t index, bool selected = true);
    [[nodiscard]] bool is_selected(size_t index) const;
    // The first selected.
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] size_t segment_count() const { return m_segments.size(); }

    // The segment's index, each time one changes.
    Function<void(size_t)> on_change;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::SegmentedControl; }
    [[nodiscard]] String accessibility_value() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    bool key_down(KeyEvent const&) override;

private:
    struct Segment {
        String title;
        bool selected { false };
    };
    [[nodiscard]] Gfx::FloatRect segment_rect(size_t) const;
    [[nodiscard]] int segment_at(Gfx::FloatPoint) const;

    Vector<Segment> m_segments;
    Mode m_mode { Mode::SelectOne };
    int m_pressed_segment { -1 };
};

class Slider : public Control {
public:
    Slider(float minimum = 0, float maximum = 1, float value = 0);

    void set_value(float);
    [[nodiscard]] float value() const { return m_value; }
    void set_range(float minimum, float maximum);
    // Marks along it, and the value stops at them.
    void set_tick_count(int);
    void set_vertical(bool);
    [[nodiscard]] bool is_vertical() const { return m_vertical; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Insets overflow() const override { return { 4, 4, 4, 4 }; }
    [[nodiscard]] float focus_ring_radius() const override { return height() / 2; }
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Slider; }
    [[nodiscard]] String accessibility_value() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    bool key_down(KeyEvent const&) override;

private:
    [[nodiscard]] float position_of_value() const;
    void set_from_point(Gfx::FloatPoint);

    float m_minimum;
    float m_maximum;
    float m_value;
    int m_ticks { 0 };
    bool m_vertical { false };
    bool m_dragging { false };
};

class Stepper : public Control {
public:
    Stepper(int minimum = 0, int maximum = 100, int value = 0);

    void set_value(int);
    [[nodiscard]] int value() const { return m_value; }
    void set_wraps(bool wraps) { m_wraps = wraps; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Stepper; }
    [[nodiscard]] String accessibility_value() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    bool key_down(KeyEvent const&) override;

private:
    void step(int by);
    void stop_repeating();

    int m_minimum;
    int m_maximum;
    int m_value;
    bool m_wraps { false };
    int m_pressed_part { 0 };
    int m_timer { -1 };
};

// How far along: as a bar, or with no end in sight as one that sweeps.
class ProgressBar : public View {
public:
    explicit ProgressBar(float value = 0);
    ~ProgressBar() override;

    void set_value(float);
    [[nodiscard]] float value() const { return m_value; }
    void set_indeterminate(bool);
    void set_control_size(ControlSize);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ProgressIndicator; }
    [[nodiscard]] String accessibility_value() const override;

private:
    float m_value;
    float m_phase { 0 };
    bool m_indeterminate { false };
    ControlSize m_size { ControlSize::Regular };
    int m_timer { -1 };
};

class Spinner : public View {
public:
    explicit Spinner(ControlSize = ControlSize::Regular);
    ~Spinner() override;

    void set_running(bool);
    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ProgressIndicator; }

private:
    ControlSize m_size;
    int m_step { 0 };
    int m_timer { -1 };
};

class Separator : public View {
public:
    explicit Separator(bool vertical = false);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;

private:
    bool m_vertical;
};

class ColorWell : public Control {
public:
    explicit ColorWell(Gfx::Color = { 0, 122, 255, 255 });

    void set_color(Gfx::Color);
    [[nodiscard]] Gfx::Color color() const { return m_color; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ColorWell; }
    [[nodiscard]] String accessibility_value() const override;

private:
    Gfx::Color m_color;
};

// A titled group with a faint background.
class Box : public View {
public:
    explicit Box(StringView title = {});

    void set_title(StringView);
    [[nodiscard]] View& content() { return *m_content; }

    void draw(Graphics&) override;
    void layout() override;
    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }

private:
    String m_title;
    View* m_content { nullptr };
};

// A bar, as of how full a disk is.
class LevelIndicator : public View {
public:
    explicit LevelIndicator(float value = 0, float warning = 0.8f, float critical = 0.9f);

    void set_value(float);
    [[nodiscard]] float value() const { return m_value; }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ProgressIndicator; }
    [[nodiscard]] String accessibility_value() const override;

private:
    float m_value;
    float m_warning;
    float m_critical;
};

}
