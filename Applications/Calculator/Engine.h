/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/String.h>
#include <LibBase/StringView.h>

// What a four-function calculator does with key presses: multiplication before
// addition, = again repeats the last step, and % means a share of what it is being
// added to. Nothing here draws. Results are shown to 12 significant digits, which hides
// the error binary fractions add (0.1 + 0.2 shows as 0.3).

namespace Calculator {

enum class Operation : u8 {
    None,
    Add,
    Subtract,
    Multiply,
    Divide,
};

class Engine {
public:
    static constexpr int largest_digits = 12;

    void digit(int value)
    {
        start_entry_if_needed();
        if (m_entry_digits >= largest_digits)
            return;
        if (m_entry_digits == 1 && m_entry[0] == '0' && !m_entry_has_point) {
            m_entry[0] = static_cast<char>('0' + value);
            return;
        }
        m_entry[m_entry_length++] = static_cast<char>('0' + value);
        ++m_entry_digits;
    }

    void decimal_point()
    {
        start_entry_if_needed();
        if (m_entry_has_point)
            return;
        m_entry[m_entry_length++] = '.';
        m_entry_has_point = true;
    }

    void backspace()
    {
        if (!m_entering)
            return;
        if (m_entry_length <= 1) {
            m_entry[0] = '0';
            m_entry_digits = 1;
            return;
        }
        char gone = m_entry[--m_entry_length];
        if (gone == '.')
            m_entry_has_point = false;
        else
            --m_entry_digits;
    }

    void negate()
    {
        if (m_error)
            return;
        if (m_entering)
            m_entry_negative = !m_entry_negative;
        else
            m_value = -m_value;
    }

    // A share of what is being added to; on its own, a hundredth.
    void percent()
    {
        if (m_error)
            return;
        double value = current();
        if (m_pending_count > 0 && (top().operation == Operation::Add || top().operation == Operation::Subtract))
            value = top().value * value / 100;
        else
            value = value / 100;
        m_value = value;
        m_entering = false;
        m_cleared = false;
    }

    void operation(Operation operation)
    {
        if (m_error)
            return;
        // Pressed twice: the second is meant instead.
        if (!m_entering && m_just_chose_operation && m_pending_count > 0) {
            top().operation = operation;
            m_active = operation;
            return;
        }
        double value = current();
        while (m_pending_count > 0 && precedence(top().operation) >= precedence(operation)) {
            value = apply(top().value, top().operation, value);
            --m_pending_count;
            if (m_error)
                return;
        }
        m_pending[m_pending_count++] = { value, operation };
        m_value = value;
        m_entering = false;
        m_just_chose_operation = true;
        m_active = operation;
        m_cleared = false;
    }

    void equals()
    {
        if (m_error)
            return;
        double value = current();
        if (m_pending_count > 0) {
            m_repeat_operation = top().operation;
            m_repeat_operand = value;
            while (m_pending_count > 0) {
                value = apply(top().value, top().operation, value);
                --m_pending_count;
                if (m_error)
                    return;
            }
        } else if (m_repeat_operation != Operation::None) {
            value = apply(value, m_repeat_operation, m_repeat_operand);
            if (m_error)
                return;
        }
        m_value = value;
        m_entering = false;
        m_just_chose_operation = false;
        m_active = Operation::None;
        m_cleared = false;
    }

    // The entry first; pressed again, everything.
    void clear()
    {
        if (!m_error && !shows_all_clear()) {
            m_entering = false;
            m_value = 0;
            m_entry_negative = false;
            m_cleared = true;
            // What was chosen stands, and shows again.
            m_active = m_pending_count > 0 ? top().operation : Operation::None;
            return;
        }
        *this = Engine {};
    }

    // Whether the clear key is "AC" for now, not "C".
    [[nodiscard]] bool shows_all_clear() const
    {
        if (m_error)
            return true;
        if (m_cleared)
            return true;
        return !m_entering && m_value == 0 && m_pending_count == 0 && m_repeat_operation == Operation::None;
    }

    [[nodiscard]] bool has_error() const { return m_error; }
    // The operation waiting for its second number, until that is being typed.
    [[nodiscard]] Operation active_operation() const { return m_entering ? Operation::None : m_active; }

    // With thousands separators, for the eye.
    [[nodiscard]] String display() const
    {
        if (m_error)
            return MUST(String::from_view("Error"_sv));
        if (m_entering)
            return with_separators(entry_view(), m_entry_negative);
        char buffer[40];
        auto text = format_number(m_value, buffer);
        bool negative = !text.is_empty() && text[0] == '-';
        return with_separators(negative ? text.substring_view(1, text.length() - 1) : text, negative);
    }

    // Plain, for the clipboard.
    [[nodiscard]] String plain_display() const
    {
        if (m_error)
            return MUST(String::from_view("Error"_sv));
        StringBuilder builder;
        if (m_entering) {
            if (m_entry_negative)
                builder.append("-"_sv);
            builder.append(entry_view());
        } else {
            char buffer[40];
            builder.append(format_number(m_value, buffer));
        }
        return MUST(builder.to_string());
    }

    // A number from elsewhere: true if that is what it was.
    bool set_from_text(StringView text)
    {
        size_t start = 0;
        bool negative = false;
        if (start < text.length() && text[start] == '-') {
            negative = true;
            ++start;
        }
        if (start >= text.length())
            return false;
        Engine candidate = *this;
        candidate.m_entering = false;
        for (size_t i = start; i < text.length(); ++i) {
            char c = text[i];
            if (c == ',' || c == ' ')
                continue;
            if (c == '.')
                candidate.decimal_point();
            else if (c >= '0' && c <= '9')
                candidate.digit(c - '0');
            else
                return false;
        }
        candidate.m_entry_negative = negative;
        *this = candidate;
        return true;
    }

    // To 12 significant digits, in `buffer`.
    [[nodiscard]] static StringView format_number(double value, char (&buffer)[40])
    {
        size_t length = 0;
        if (value != value || value > 1.7e308 || value < -1.7e308) {
            buffer[0] = 'E', buffer[1] = 'r', buffer[2] = 'r', buffer[3] = 'o', buffer[4] = 'r';
            return { buffer, 5 };
        }
        if (value == 0) {
            buffer[0] = '0';
            return { buffer, 1 };
        }
        double magnitude = value < 0 ? -value : value;
        int exponent = 0;
        while (magnitude >= power_of_ten(exponent + 1))
            ++exponent;
        while (magnitude < power_of_ten(exponent))
            --exponent;
        constexpr int significant = 12;
        double scaled = exponent <= significant - 1 ? magnitude * power_of_ten(significant - 1 - exponent) : magnitude / power_of_ten(exponent - (significant - 1));
        auto digits = static_cast<u64>(scaled + 0.5);
        u64 limit = 1;
        for (int i = 0; i < significant; ++i)
            limit *= 10;
        if (digits >= limit) {
            digits /= 10;
            ++exponent;
        }
        char text[significant];
        for (int i = significant - 1; i >= 0; --i) {
            text[i] = static_cast<char>('0' + digits % 10);
            digits /= 10;
        }
        int used = significant;
        while (used > 1 && text[used - 1] == '0')
            --used;

        if (value < 0)
            buffer[length++] = '-';
        if (exponent >= significant || exponent < -6) {
            buffer[length++] = text[0];
            if (used > 1) {
                buffer[length++] = '.';
                for (int i = 1; i < used; ++i)
                    buffer[length++] = text[i];
            }
            buffer[length++] = 'e';
            int shown = exponent;
            if (shown < 0) {
                buffer[length++] = '-';
                shown = -shown;
            }
            char exponent_digits[4];
            int count = 0;
            do {
                exponent_digits[count++] = static_cast<char>('0' + shown % 10);
                shown /= 10;
            } while (shown > 0);
            while (count > 0)
                buffer[length++] = exponent_digits[--count];
        } else if (exponent >= 0) {
            for (int i = 0; i <= exponent; ++i)
                buffer[length++] = i < used ? text[i] : '0';
            if (used > exponent + 1) {
                buffer[length++] = '.';
                for (int i = exponent + 1; i < used; ++i)
                    buffer[length++] = text[i];
            }
        } else {
            buffer[length++] = '0';
            buffer[length++] = '.';
            for (int i = -1; i > exponent; --i)
                buffer[length++] = '0';
            for (int i = 0; i < used; ++i)
                buffer[length++] = text[i];
        }
        return { buffer, length };
    }

private:
    struct Pending {
        double value;
        Operation operation;
    };

    [[nodiscard]] static int precedence(Operation operation)
    {
        return operation == Operation::Multiply || operation == Operation::Divide ? 2 : 1;
    }

    [[nodiscard]] static double power_of_ten(int exponent)
    {
        bool inverse = exponent < 0;
        int remaining = inverse ? -exponent : exponent;
        double result = 1;
        double base = 10;
        while (remaining > 0) {
            if (remaining & 1)
                result *= base;
            base *= base;
            remaining >>= 1;
        }
        return inverse ? 1 / result : result;
    }

    [[nodiscard]] Pending& top() { return m_pending[m_pending_count - 1]; }
    [[nodiscard]] Pending const& top() const { return m_pending[m_pending_count - 1]; }
    [[nodiscard]] StringView entry_view() const { return { m_entry, m_entry_length }; }

    void start_entry_if_needed()
    {
        if (m_entering)
            return;
        if (m_error)
            *this = Engine {};
        m_entering = true;
        m_cleared = false;
        m_just_chose_operation = false;
        m_entry[0] = '0';
        m_entry_length = 1;
        m_entry_digits = 1;
        m_entry_has_point = false;
        m_entry_negative = false;
        // A new sum, after =.
        if (m_pending_count == 0)
            m_repeat_operation = Operation::None;
    }

    [[nodiscard]] double current() const
    {
        if (!m_entering)
            return m_value;
        // All the digits as a whole number, then the point: both are exact, so the
        // result is as near as a double can be.
        double whole = 0;
        double divisor = 1;
        bool past_point = false;
        for (size_t i = 0; i < m_entry_length; ++i) {
            if (m_entry[i] == '.') {
                past_point = true;
                continue;
            }
            whole = whole * 10 + (m_entry[i] - '0');
            if (past_point)
                divisor *= 10;
        }
        double value = whole / divisor;
        return m_entry_negative ? -value : value;
    }

    double apply(double left, Operation operation, double right)
    {
        switch (operation) {
        case Operation::Add:
            return left + right;
        case Operation::Subtract:
            return left - right;
        case Operation::Multiply:
            return left * right;
        case Operation::Divide:
            if (right == 0) {
                m_error = true;
                m_entering = false;
                m_pending_count = 0;
                m_active = Operation::None;
                return 0;
            }
            return left / right;
        case Operation::None:
            break;
        }
        return right;
    }

    [[nodiscard]] static String with_separators(StringView text, bool negative)
    {
        size_t whole_length = 0;
        while (whole_length < text.length() && text[whole_length] >= '0' && text[whole_length] <= '9')
            ++whole_length;
        StringBuilder builder;
        if (negative)
            builder.append("-"_sv);
        // Scientific notation stays as it is.
        bool group = whole_length == text.length() || text[whole_length] == '.';
        for (size_t i = 0; i < whole_length; ++i) {
            builder.append(text.substring_view(i, 1));
            size_t left = whole_length - i - 1;
            if (group && left > 0 && left % 3 == 0)
                builder.append(","_sv);
        }
        builder.append(text.substring_view(whole_length, text.length() - whole_length));
        return MUST(builder.to_string());
    }

    double m_value { 0 };
    bool m_entering { false };
    char m_entry[largest_digits + 2] {};
    size_t m_entry_length { 0 };
    int m_entry_digits { 0 };
    bool m_entry_has_point { false };
    bool m_entry_negative { false };
    Pending m_pending[3] {};
    size_t m_pending_count { 0 };
    bool m_just_chose_operation { false };
    Operation m_active { Operation::None };
    Operation m_repeat_operation { Operation::None };
    double m_repeat_operand { 0 };
    bool m_cleared { false };
    bool m_error { false };
};

}
