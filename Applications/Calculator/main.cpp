/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Engine.h"
#include <Desktop.h>
#include <LibUI/Application.h>
#include <signal.h>
#include <stdio.h>

// The four functions, in a window that is all display and keys.

using namespace UI;
using namespace Gfx;
using Calculator::Engine;
using Calculator::Operation;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

enum class KeyKind : u8 {
    Clear,
    Negate,
    Percent,
    Operator,
    Digit,
    Point,
    Equals,
};

struct KeyDefinition {
    StringView title;
    KeyKind kind;
    int row;
    int column;
    int span;
    int digit;
    Operation operation;
};

// Along the rows, as they sit.
constexpr KeyDefinition s_keys[] = {
    { "AC"_sv, KeyKind::Clear, 0, 0, 1, 0, Operation::None },
    { "\xC2\xB1"_sv, KeyKind::Negate, 0, 1, 1, 0, Operation::None },
    { "%"_sv, KeyKind::Percent, 0, 2, 1, 0, Operation::None },
    { "\xC3\xB7"_sv, KeyKind::Operator, 0, 3, 1, 0, Operation::Divide },
    { "7"_sv, KeyKind::Digit, 1, 0, 1, 7, Operation::None },
    { "8"_sv, KeyKind::Digit, 1, 1, 1, 8, Operation::None },
    { "9"_sv, KeyKind::Digit, 1, 2, 1, 9, Operation::None },
    { "\xC3\x97"_sv, KeyKind::Operator, 1, 3, 1, 0, Operation::Multiply },
    { "4"_sv, KeyKind::Digit, 2, 0, 1, 4, Operation::None },
    { "5"_sv, KeyKind::Digit, 2, 1, 1, 5, Operation::None },
    { "6"_sv, KeyKind::Digit, 2, 2, 1, 6, Operation::None },
    { "\xE2\x88\x92"_sv, KeyKind::Operator, 2, 3, 1, 0, Operation::Subtract },
    { "1"_sv, KeyKind::Digit, 3, 0, 1, 1, Operation::None },
    { "2"_sv, KeyKind::Digit, 3, 1, 1, 2, Operation::None },
    { "3"_sv, KeyKind::Digit, 3, 2, 1, 3, Operation::None },
    { "+"_sv, KeyKind::Operator, 3, 3, 1, 0, Operation::Add },
    { "0"_sv, KeyKind::Digit, 4, 0, 2, 0, Operation::None },
    { "."_sv, KeyKind::Point, 4, 2, 1, 0, Operation::None },
    { "="_sv, KeyKind::Equals, 4, 3, 1, 0, Operation::None },
};

class CalculatorView final : public View {
public:
    CalculatorView() = default;

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("calculator.background"_sv));

        // The display: as big as fits.
        float display_height = t.metric("calculator.displayHeight"_sv);
        float padding = t.metric("calculator.displayPadding"_sv);
        auto text = m_engine.display();
        FontSpec font;
        font.size = t.metric("calculator.displayFont"_sv);
        float room = width() - 2 * padding;
        float smallest = t.metric("calculator.smallestDisplayFont"_sv);
        while (font.size > smallest && Appearance::text_width(font, text.view()) > room)
            font.size -= 1;
        float text_width = Appearance::text_width(font, text.view());
        graphics.draw_text(text.view(), font, { width() - padding - text_width, display_height - padding }, t.color("calculator.text"_sv));

        FontSpec key_font;
        key_font.size = t.metric("calculator.keyFont"_sv);
        float gap = t.metric("calculator.gap"_sv);
        graphics.fill({ 0, display_height - gap, width(), height() - display_height + gap }, t.color("calculator.gap"_sv));
        for (size_t i = 0; i < array_size(s_keys); ++i) {
            auto const& key = s_keys[i];
            auto rect = key_rect(key);
            bool pressed = static_cast<int>(i) == m_pressed;
            graphics.fill(rect, key_color(key, pressed));
            StringView title = key.title;
            if (key.kind == KeyKind::Clear && !m_engine.shows_all_clear())
                title = "C"_sv;
            graphics.draw_text_centered(title, key_font, rect, t.color("calculator.text"_sv));
        }
    }

    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }

    bool mouse_down(MouseEvent const& event) override
    {
        m_pressed = key_at(event.position);
        if (m_pressed < 0) {
            // Everything else moves the window, as a title bar would.
            window()->move_by_pointer();
            return true;
        }
        set_needs_display();
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (m_pressed < 0)
            return;
        // Off the key it started on: not pressed, for now.
        int under = key_at(event.position);
        if ((under == m_pressed) != m_press_inside) {
            m_press_inside = under == m_pressed;
            set_needs_display();
        }
    }

    void mouse_up(MouseEvent const& event) override
    {
        int pressed = m_pressed;
        m_pressed = -1;
        m_press_inside = true;
        if (pressed >= 0 && key_at(event.position) == pressed)
            press(s_keys[pressed]);
        set_needs_display();
    }

    bool key_down(KeyEvent const& event) override
    {
        if (event.has_command())
            return false;
        if (event.code == KeyBackspace || event.code == KeyDelete) {
            m_engine.backspace();
            changed();
            return true;
        }
        if (event.code == KeyEscape) {
            m_engine.clear();
            changed();
            return true;
        }
        if (event.code == KeyEnter) {
            m_engine.equals();
            changed();
            return true;
        }
        if (event.text.length() != 1)
            return false;
        char c = event.text[0];
        if (c >= '0' && c <= '9')
            m_engine.digit(c - '0');
        else if (c == '.' || c == ',')
            m_engine.decimal_point();
        else if (c == '+')
            m_engine.operation(Operation::Add);
        else if (c == '-')
            m_engine.operation(Operation::Subtract);
        else if (c == '*' || c == 'x')
            m_engine.operation(Operation::Multiply);
        else if (c == '/')
            m_engine.operation(Operation::Divide);
        else if (c == '=')
            m_engine.equals();
        else if (c == '%')
            m_engine.percent();
        else if (c == 'c' || c == 'C')
            m_engine.clear();
        else if (c == 'n')
            m_engine.negate();
        else
            return false;
        changed();
        return true;
    }

    bool perform_command(StringView command) override
    {
        if (command == "copy"_sv) {
            Platform::the().copy_text(m_engine.plain_display().view());
            return true;
        }
        if (command == "paste"_sv) {
            auto text = Platform::the().paste_text();
            if (!text.is_error() && m_engine.set_from_text(text.value().view()))
                changed();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_perform_command(StringView command) const override
    {
        return command == "copy"_sv || command == "paste"_sv;
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::StaticText; }
    [[nodiscard]] String accessibility_value() const override { return m_engine.plain_display(); }

private:
    [[nodiscard]] FloatRect key_rect(KeyDefinition const& key) const
    {
        auto const& t = tokens();
        float top = t.metric("calculator.displayHeight"_sv);
        float key_height = t.metric("calculator.keyHeight"_sv);
        float gap = t.metric("calculator.gap"_sv);
        float key_width = (width() - 3 * gap) / 4;
        float x = static_cast<float>(key.column) * (key_width + gap);
        float y = top + static_cast<float>(key.row) * (key_height + gap);
        return { x, y, static_cast<float>(key.span) * key_width + static_cast<float>(key.span - 1) * gap, key_height };
    }

    [[nodiscard]] int key_at(FloatPoint point) const
    {
        for (size_t i = 0; i < array_size(s_keys); ++i) {
            auto rect = key_rect(s_keys[i]);
            if (point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height)
                return static_cast<int>(i);
        }
        return -1;
    }

    [[nodiscard]] Color key_color(KeyDefinition const& key, bool pressed) const
    {
        auto const& t = tokens();
        pressed = pressed && m_press_inside;
        switch (key.kind) {
        case KeyKind::Digit:
        case KeyKind::Point:
            return t.color(pressed ? "calculator.digitPressed"_sv : "calculator.digit"_sv);
        case KeyKind::Clear:
        case KeyKind::Negate:
        case KeyKind::Percent:
            return t.color(pressed ? "calculator.functionPressed"_sv : "calculator.function"_sv);
        case KeyKind::Operator:
            if (!pressed && m_engine.active_operation() == key.operation)
                return t.color("calculator.operatorActive"_sv);
            [[fallthrough]];
        case KeyKind::Equals:
            return t.color(pressed ? "calculator.operatorPressed"_sv : "calculator.operator"_sv);
        }
        return {};
    }

    void press(KeyDefinition const& key)
    {
        switch (key.kind) {
        case KeyKind::Clear:
            m_engine.clear();
            break;
        case KeyKind::Negate:
            m_engine.negate();
            break;
        case KeyKind::Percent:
            m_engine.percent();
            break;
        case KeyKind::Operator:
            m_engine.operation(key.operation);
            break;
        case KeyKind::Digit:
            m_engine.digit(key.digit);
            break;
        case KeyKind::Point:
            m_engine.decimal_point();
            break;
        case KeyKind::Equals:
            m_engine.equals();
            break;
        }
    }

    void changed() { set_needs_display(); }

    Engine m_engine;
    int m_pressed { -1 };
    bool m_press_inside { true };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Calculator"_sv, "com.vulture.Calculator"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Calculator: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());

    auto const& t = tokens();
    int width = static_cast<int>(t.metric("calculator.width"_sv));
    float key_height = t.metric("calculator.keyHeight"_sv);
    float gap = t.metric("calculator.gap"_sv);
    int height = static_cast<int>(t.metric("calculator.displayHeight"_sv) + 5 * key_height + 4 * gap);
    auto& window = application->make_window<UI::Window>("Calculator"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowUnifiedTitle);
    window.center_on_screen();
    auto& view = window.set_content<CalculatorView>();
    window.on_resize = [&view](FloatSize) { view.set_needs_display(); };
    (void)window.make_first_responder(&view);
    window.show();
    printf("calculator: ready\n");
    fflush(stdout);
    return application->exec();
}
