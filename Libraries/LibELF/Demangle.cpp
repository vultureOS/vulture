/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Array.h>
#include <LibELF/Demangle.h>

// A subset of the Itanium C++ ABI mangling grammar, enough for readable
// backtraces. Function parameter types are never printed, so after the name
// the rest of the encoding is ignored.

namespace ELF {

namespace {

struct OperatorName {
    char code[3];
    char const* text;
};

constexpr OperatorName operator_names[] = {
    { "nw", "new" },
    { "na", "new[]" },
    { "dl", "delete" },
    { "da", "delete[]" },
    { "ps", "+" },
    { "ng", "-" },
    { "ad", "&" },
    { "de", "*" },
    { "co", "~" },
    { "pl", "+" },
    { "mi", "-" },
    { "ml", "*" },
    { "dv", "/" },
    { "rm", "%" },
    { "an", "&" },
    { "or", "|" },
    { "eo", "^" },
    { "aS", "=" },
    { "pL", "+=" },
    { "mI", "-=" },
    { "mL", "*=" },
    { "dV", "/=" },
    { "rM", "%=" },
    { "aN", "&=" },
    { "oR", "|=" },
    { "eO", "^=" },
    { "ls", "<<" },
    { "rs", ">>" },
    { "lS", "<<=" },
    { "rS", ">>=" },
    { "eq", "==" },
    { "ne", "!=" },
    { "lt", "<" },
    { "gt", ">" },
    { "le", "<=" },
    { "ge", ">=" },
    { "ss", "<=>" },
    { "nt", "!" },
    { "aa", "&&" },
    { "oo", "||" },
    { "pp", "++" },
    { "mm", "--" },
    { "cm", "," },
    { "pm", "->*" },
    { "pt", "->" },
    { "cl", "()" },
    { "ix", "[]" },
    { "qu", "?" },
};

struct BuiltinType {
    char code;
    char const* name;
};

constexpr BuiltinType builtin_types[] = {
    { 'v', "void" },
    { 'b', "bool" },
    { 'c', "char" },
    { 'a', "signed char" },
    { 'h', "unsigned char" },
    { 's', "short" },
    { 't', "unsigned short" },
    { 'i', "int" },
    { 'j', "unsigned int" },
    { 'l', "long" },
    { 'm', "unsigned long" },
    { 'x', "long long" },
    { 'y', "unsigned long long" },
    { 'f', "float" },
    { 'd', "double" },
    { 'e', "long double" },
    { 'w', "wchar_t" },
};

class Demangler {
public:
    explicit Demangler(StringView input)
        : m_input(input)
    {
    }

    bool run()
    {
        if (!consume("_Z"_sv))
            return false;
        if (!parse_encoding())
            return false;
        // Anything left over is the parameter list or a compiler suffix such as ".cold".
        return !m_output.is_truncated();
    }

    [[nodiscard]] StringView result() const { return m_output.view(); }

private:
    struct Range {
        size_t start;
        size_t end;
    };

    [[nodiscard]] bool at_end() const { return m_position >= m_input.length(); }
    [[nodiscard]] char peek(size_t ahead = 0) const
    {
        return m_position + ahead < m_input.length() ? m_input[m_position + ahead] : '\0';
    }

    bool consume(char character)
    {
        if (peek() != character)
            return false;
        ++m_position;
        return true;
    }

    bool consume(StringView text)
    {
        if (!m_input.substring_view(m_position).starts_with(text))
            return false;
        m_position += text.length();
        return true;
    }

    void emit(StringView text) { m_output.append(text); }

    void add_substitution(size_t start)
    {
        if (m_substitution_count < m_substitutions.size())
            m_substitutions[m_substitution_count++] = { start, m_output.length() };
    }

    bool parse_number(size_t& number)
    {
        if (!is_ascii_digit(static_cast<u8>(peek())))
            return false;
        number = 0;
        while (is_ascii_digit(static_cast<u8>(peek()))) {
            number = number * 10 + static_cast<size_t>(m_input[m_position++] - '0');
            if (number > m_input.length())
                return false;
        }
        return true;
    }

    bool parse_encoding()
    {
        // Special names: virtual tables, type information, guard variables, thunks.
        if (consume("TV"_sv)) {
            emit("vtable for "_sv);
            return parse_name();
        }
        if (consume("TI"_sv)) {
            emit("typeinfo for "_sv);
            return parse_name();
        }
        if (consume("TS"_sv)) {
            emit("typeinfo name for "_sv);
            return parse_name();
        }
        if (consume("GV"_sv)) {
            emit("guard variable for "_sv);
            return parse_name();
        }
        if (consume("Th"_sv)) {
            consume('n');
            size_t offset = 0;
            if (!parse_number(offset) || !consume('_'))
                return false;
            emit("non-virtual thunk to "_sv);
            return parse_encoding();
        }
        return parse_name();
    }

    bool parse_name()
    {
        if (peek() == 'N')
            return parse_nested_name();
        if (peek() == 'Z')
            return parse_local_name();

        size_t start = m_output.length();
        if (consume("St"_sv)) {
            emit("std::"_sv);
            if (!parse_unqualified_name())
                return false;
        } else if (peek() == 'S') {
            if (!parse_substitution())
                return false;
        } else {
            consume('L'); // internal linkage
            if (!parse_unqualified_name())
                return false;
        }
        if (peek() == 'I') {
            add_substitution(start);
            return parse_template_args();
        }
        return true;
    }

    bool parse_nested_name()
    {
        if (!consume('N'))
            return false;
        // CV- and ref-qualifiers of the member function.
        while (peek() == 'r' || peek() == 'V' || peek() == 'K')
            ++m_position;
        if (peek() == 'R' || peek() == 'O')
            ++m_position;

        size_t start = m_output.length();
        bool first = true;
        while (!consume('E')) {
            if (at_end())
                return false;
            if (peek() == 'I') {
                if (first || !parse_template_args())
                    return false;
            } else {
                if (!first)
                    emit("::"_sv);
                if (consume("St"_sv)) {
                    emit("std"_sv);
                } else if (peek() == 'S') {
                    if (!parse_substitution())
                        return false;
                    first = false;
                    continue;
                } else {
                    consume('L');
                    if (!parse_unqualified_name())
                        return false;
                }
            }
            first = false;
            // Every prefix except the complete name is a substitution candidate.
            if (peek() != 'E')
                add_substitution(start);
        }
        return !first;
    }

    bool parse_local_name()
    {
        if (!consume('Z'))
            return false;
        if (!parse_encoding())
            return false;
        // Skip the enclosing function's parameter types.
        if (!skip_until_matching_end())
            return false;
        emit("()::"_sv);
        if (consume('s')) {
            emit("string literal"_sv);
        } else if (!parse_name()) {
            return false;
        }
        // Optional discriminator: _<digit> or __<number>_
        if (consume('_')) {
            if (consume('_')) {
                size_t discriminator = 0;
                if (!parse_number(discriminator) || !consume('_'))
                    return false;
            } else {
                size_t discriminator = 0;
                if (!parse_number(discriminator))
                    return false;
            }
        }
        return true;
    }

    bool parse_unqualified_name()
    {
        char next = peek();
        if (is_ascii_digit(static_cast<u8>(next)))
            return parse_source_name();
        if (next == 'C' || next == 'D')
            return parse_ctor_dtor_name();
        if (next == 'U')
            return parse_unnamed_type_name();
        if (is_ascii_lower_alpha(static_cast<u8>(next)))
            return parse_operator_name();
        return false;
    }

    bool parse_source_name()
    {
        size_t length = 0;
        if (!parse_number(length) || length == 0 || m_position + length > m_input.length())
            return false;
        auto name = m_input.substring_view(m_position, length);
        m_position += length;
        m_last_source_name = name;
        if (name.starts_with("_GLOBAL__N"_sv))
            emit("(anonymous namespace)"_sv);
        else
            emit(name);
        return true;
    }

    bool parse_ctor_dtor_name()
    {
        if (m_last_source_name.is_empty())
            return false;
        if (consume('C')) {
            if (peek() != '1' && peek() != '2' && peek() != '3')
                return false; // Inheriting constructors (CI1/CI2) are not covered.
            ++m_position;
            emit(m_last_source_name);
            return true;
        }
        if (consume('D')) {
            if (peek() != '0' && peek() != '1' && peek() != '2')
                return false;
            ++m_position;
            emit("~"_sv);
            emit(m_last_source_name);
            return true;
        }
        return false;
    }

    bool parse_unnamed_type_name()
    {
        if (consume("Ul"_sv)) {
            // Lambda: Ul <parameter types> E [<number>] _
            if (!skip_until_matching_end())
                return false;
            size_t number = 0;
            bool has_number = parse_number(number);
            if (!consume('_'))
                return false;
            FixedStringBuilder<32> label;
            format_to(label, "{{lambda#{}}}", has_number ? number + 2 : 1);
            emit(label.view());
            return true;
        }
        if (consume("Ut"_sv)) {
            size_t number = 0;
            parse_number(number);
            if (!consume('_'))
                return false;
            emit("{unnamed type}"_sv);
            return true;
        }
        return false;
    }

    bool parse_operator_name()
    {
        if (consume("li"_sv)) {
            emit("operator\"\" "_sv);
            return parse_source_name();
        }
        for (auto const& entry : operator_names) {
            if (consume(StringView(entry.code, 2))) {
                emit("operator"_sv);
                emit(StringView(entry.text));
                return true;
            }
        }
        // Conversion operators ("cv <type>") need a full type parser.
        return false;
    }

    bool parse_substitution()
    {
        if (!consume('S'))
            return false;
        size_t index = 0;
        if (consume('_')) {
            index = 0;
        } else {
            // Base-36 sequence id (digits and capital letters), then '_'.
            size_t sequence = 0;
            bool any = false;
            while (is_ascii_digit(static_cast<u8>(peek())) || is_ascii_upper_alpha(static_cast<u8>(peek()))) {
                char digit = m_input[m_position++];
                sequence = sequence * 36 + static_cast<size_t>(is_ascii_digit(static_cast<u8>(digit)) ? digit - '0' : digit - 'A' + 10);
                any = true;
            }
            if (!any || !consume('_'))
                return false;
            index = sequence + 1;
        }
        if (index >= m_substitution_count)
            return false;
        auto range = m_substitutions[index];
        // Copy out first: emitting appends to the buffer the range points into.
        FixedStringBuilder<256> text;
        text.append(m_output.view().substring_view(range.start, range.end - range.start));
        emit(text.view());
        return true;
    }

    bool parse_template_args()
    {
        if (!consume('I'))
            return false;
        size_t saved_position = m_position;
        size_t saved_length = m_output.length();

        emit("<"_sv);
        bool first = true;
        while (!consume('E')) {
            if (!first)
                emit(", "_sv);
            first = false;
            if (!parse_simple_type()) {
                // Too involved to spell out: show a placeholder instead.
                m_position = saved_position;
                truncate_output(saved_length);
                emit("<...>"_sv);
                return skip_until_matching_end();
            }
        }
        emit(">"_sv);
        return true;
    }

    // Builtin types, class names and pointers/references/const of those.
    bool parse_simple_type()
    {
        if (consume('K')) {
            if (!parse_simple_type())
                return false;
            emit(" const"_sv);
            return true;
        }
        if (consume('P')) {
            if (!parse_simple_type())
                return false;
            emit("*"_sv);
            return true;
        }
        if (consume('R')) {
            if (!parse_simple_type())
                return false;
            emit("&"_sv);
            return true;
        }
        for (auto const& builtin : builtin_types) {
            if (consume(builtin.code)) {
                emit(StringView(builtin.name));
                return true;
            }
        }
        if (peek() == 'N' || is_ascii_digit(static_cast<u8>(peek()))) {
            size_t start = m_output.length();
            bool parsed = peek() == 'N' ? parse_nested_name() : parse_source_name();
            if (parsed)
                add_substitution(start);
            return parsed;
        }
        return false;
    }

    void truncate_output(size_t length)
    {
        FixedStringBuilder<256> kept;
        kept.append(m_output.view().substring_view(0, length));
        m_output.clear();
        m_output.append(kept.view());
        while (m_substitution_count > 0 && m_substitutions[m_substitution_count - 1].end > length)
            --m_substitution_count;
    }

    // Skips mangled text up to and including the 'E' that closes the current
    // construct, stepping over nested constructs and length-prefixed names.
    bool skip_until_matching_end()
    {
        size_t depth = 0;
        while (!at_end()) {
            char next = m_input[m_position];
            if (is_ascii_digit(static_cast<u8>(next))) {
                size_t length = 0;
                if (!parse_number(length) || m_position + length > m_input.length())
                    return false;
                m_position += length;
                continue;
            }
            ++m_position;
            switch (next) {
            case 'E':
                if (depth == 0)
                    return true;
                --depth;
                break;
            case 'I':
            case 'J':
            case 'N':
            case 'X':
            case 'F':
            case 'Z':
                ++depth;
                break;
            case 'S':
                // Substitution: a lowercase abbreviation, or a sequence id ending in '_'.
                if (is_ascii_lower_alpha(static_cast<u8>(peek()))) {
                    ++m_position;
                } else {
                    while (is_ascii_digit(static_cast<u8>(peek())) || is_ascii_upper_alpha(static_cast<u8>(peek())))
                        ++m_position;
                    if (!consume('_'))
                        return false;
                }
                break;
            case 'T':
                // Template parameter: T_ or T<number>_
                while (is_ascii_digit(static_cast<u8>(peek())))
                    ++m_position;
                if (!consume('_'))
                    return false;
                break;
            case 'A':
                // Array: A [<dimension>] _ <element type>
                while (is_ascii_digit(static_cast<u8>(peek())))
                    ++m_position;
                if (!consume('_'))
                    return false;
                break;
            case 'L':
                // Literal: L <type> <value> E, or L _Z <encoding> E
                if (peek() == '_' && peek(1) == 'Z') {
                    m_position += 2;
                    ++depth;
                } else if (peek() == 'N' || is_ascii_digit(static_cast<u8>(peek()))) {
                    ++depth; // A class-typed literal; its name and value close with E.
                } else {
                    while (!at_end() && peek() != 'E')
                        ++m_position;
                    if (!consume('E'))
                        return false;
                }
                break;
            case 'D':
                if (peek() == 't' || peek() == 'T') {
                    ++m_position;
                    ++depth;
                } else if (consume('v')) {
                    while (is_ascii_digit(static_cast<u8>(peek())))
                        ++m_position;
                    if (!consume('_'))
                        return false;
                } else {
                    ++m_position;
                }
                break;
            case 'u':
                // Vendor extended type: u <source-name>
                {
                    size_t length = 0;
                    if (!parse_number(length) || m_position + length > m_input.length())
                        return false;
                    m_position += length;
                }
                break;
            default:
                break;
            }
        }
        return false;
    }

    StringView m_input;
    size_t m_position { 0 };
    FixedStringBuilder<256> m_output;
    StringView m_last_source_name;
    Array<Range, 32> m_substitutions {};
    size_t m_substitution_count { 0 };
};

}

bool demangle(StringView mangled, FormatSink& output)
{
    Demangler demangler(mangled);
    if (!demangler.run())
        return false;
    output.write(demangler.result());
    return true;
}

}
