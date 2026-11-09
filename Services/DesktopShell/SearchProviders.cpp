/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SearchProviders.h"
#include <LibBase/Math.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace DesktopShell {

namespace {

// Numbers, + - * / % ^, brackets, and a few things a calculator's expected to know.
class Calculator {
public:
    explicit Calculator(StringView text)
        : m_text(text)
    {
    }

    Optional<double> run()
    {
        auto value = sum();
        skip_spaces();
        if (!value.has_value() || m_position != m_text.length() || !m_did_something)
            return {};
        return value;
    }

private:
    void skip_spaces()
    {
        while (m_position < m_text.length() && m_text[m_position] == ' ')
            ++m_position;
    }

    bool accept(char c)
    {
        skip_spaces();
        if (m_position < m_text.length() && m_text[m_position] == c) {
            ++m_position;
            return true;
        }
        return false;
    }

    Optional<double> sum()
    {
        auto left = product();
        while (left.has_value()) {
            if (accept('+')) {
                auto right = product();
                if (!right.has_value())
                    return {};
                left = left.value() + right.value();
                m_did_something = true;
            } else if (accept('-')) {
                auto right = product();
                if (!right.has_value())
                    return {};
                left = left.value() - right.value();
                m_did_something = true;
            } else {
                break;
            }
        }
        return left;
    }

    Optional<double> product()
    {
        auto left = power();
        while (left.has_value()) {
            if (accept('*') || accept('x')) {
                auto right = power();
                if (!right.has_value())
                    return {};
                left = left.value() * right.value();
                m_did_something = true;
            } else if (accept('/')) {
                auto right = power();
                if (!right.has_value() || right.value() == 0)
                    return {};
                left = left.value() / right.value();
                m_did_something = true;
            } else if (accept('%')) {
                auto right = power();
                if (!right.has_value() || right.value() == 0)
                    return {};
                left = fmod(left.value(), right.value());
                m_did_something = true;
            } else {
                break;
            }
        }
        return left;
    }

    Optional<double> power()
    {
        auto base = unary();
        if (base.has_value() && accept('^')) {
            auto exponent = power();
            if (!exponent.has_value())
                return {};
            m_did_something = true;
            return pow(base.value(), exponent.value());
        }
        return base;
    }

    Optional<double> unary()
    {
        if (accept('-')) {
            auto value = unary();
            if (!value.has_value())
                return {};
            return -value.value();
        }
        if (accept('+'))
            return unary();
        return primary();
    }

    Optional<double> primary()
    {
        skip_spaces();
        if (accept('(')) {
            auto value = sum();
            if (!value.has_value() || !accept(')'))
                return {};
            m_did_something = true;
            return value;
        }
        if (m_position < m_text.length() && m_text[m_position] >= 'a' && m_text[m_position] <= 'z')
            return named();
        size_t const start = m_position;
        while (m_position < m_text.length() && ((m_text[m_position] >= '0' && m_text[m_position] <= '9') || m_text[m_position] == '.'))
            ++m_position;
        if (m_position == start)
            return {};
        auto digits = MUST(String::from_view(m_text.substring_view(start, m_position - start)));
        char* end = nullptr;
        double const value = strtod(digits.characters(), &end);
        if (*end != 0)
            return {};
        return value;
    }

    Optional<double> named()
    {
        size_t const start = m_position;
        while (m_position < m_text.length() && m_text[m_position] >= 'a' && m_text[m_position] <= 'z')
            ++m_position;
        auto name = m_text.substring_view(start, m_position - start);
        if (name == "pi"_sv) {
            m_did_something = true;
            return 3.14159265358979323846;
        }
        if (!accept('('))
            return {};
        auto argument = sum();
        if (!argument.has_value() || !accept(')'))
            return {};
        m_did_something = true;
        if (name == "sqrt"_sv && argument.value() >= 0)
            return sqrt(argument.value());
        if (name == "sin"_sv)
            return sin(argument.value());
        if (name == "cos"_sv)
            return cos(argument.value());
        if (name == "abs"_sv)
            return argument.value() < 0 ? -argument.value() : argument.value();
        return {};
    }

    StringView m_text;
    size_t m_position { 0 };
    bool m_did_something { false };
};

constexpr StringView settings_panes[] = {
    "Appearance"_sv, "Accessibility"_sv, "Control Center"_sv, "Wallpaper"_sv, "Screen Saver"_sv, "Desktop & Dock"_sv, "Displays"_sv, "Network"_sv, "Sound"_sv, "Keyboard"_sv,
    "Mouse"_sv, "Trackpad"_sv, "Language & Region"_sv, "Notifications"_sv, "Focus"_sv, "Privacy & Security"_sv, "Lock Screen"_sv, "Users & Groups"_sv, "Login Items"_sv, "Storage"_sv,
    "Startup Disk"_sv, "About"_sv
};

bool starts_with_ignoring_case(StringView text, StringView prefix)
{
    return text.length() >= prefix.length() && text.substring_view(0, prefix.length()).equals_ignoring_ascii_case(prefix);
}

}

Optional<String> calculate(StringView expression)
{
    auto const result = Calculator(expression.trim_whitespace()).run();
    if (!result.has_value() || result.value() != result.value())
        return {};
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.12g", result.value());
    return MUST(String::from_view(StringView(buffer, strlen(buffer))));
}

Vector<StringView> settings_panes_matching(StringView text)
{
    Vector<StringView> found;
    auto wanted = text.trim_whitespace();
    if (wanted.length() < 2)
        return found;
    for (auto pane : settings_panes) {
        bool matches = false;
        size_t start = 0;
        for (size_t i = 0; i <= pane.length(); ++i) {
            if (i == pane.length() || pane[i] == ' ') {
                if (starts_with_ignoring_case(pane.substring_view(start), wanted))
                    matches = true;
                start = i + 1;
            }
        }
        if (matches)
            found.append(pane);
    }
    return found;
}

}
