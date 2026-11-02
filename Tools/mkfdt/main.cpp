/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// mkfdt INPUT.dts OUTPUT.dtb
//
// Makes a flattened device tree, for describing a machine to an emulator that wants one and whose firmware isn't
// there to do it. It reads the plain part of the usual source language:
//
//     / {
//         name { // a comment
//             a-string = "text", "more";
//             some-cells = <0x10 2>, <3>;
//             just-there;
//         };
//     };

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fseek(FILE*, long offset, int origin);
long ftell(FILE*);
int fclose(FILE*);
int printf(char const* format, ...);
}

namespace {

constexpr u32 token_begin_node = 1;
constexpr u32 token_end_node = 2;
constexpr u32 token_property = 3;
constexpr u32 token_end = 9;

void put_be32(Vector<u8>& out, u32 value)
{
    out.append(static_cast<u8>(value >> 24));
    out.append(static_cast<u8>(value >> 16));
    out.append(static_cast<u8>(value >> 8));
    out.append(static_cast<u8>(value));
}

void pad_to_word(Vector<u8>& out)
{
    while (out.size() % 4 != 0)
        out.append(0);
}

class Compiler {
public:
    explicit Compiler(StringView source)
        : m_source(source)
    {
    }

    bool compile()
    {
        skip_space();
        if (!accept('/') || (skip_space(), !accept('{')))
            return error("expected the root node, \"/ {\"");
        put_be32(m_structure, token_begin_node);
        put_be32(m_structure, 0); // An empty name, padded.
        if (!node_body())
            return false;
        put_be32(m_structure, token_end);
        skip_space();
        return m_at == m_source.length() || error("something after the root node");
    }

    [[nodiscard]] Vector<u8> finish() const
    {
        constexpr u32 header_size = 40;
        constexpr u32 reservation_block_size = 16;
        u32 const structure_at = header_size + reservation_block_size;
        u32 const strings_at = structure_at + static_cast<u32>(m_structure.size());
        u32 const total = strings_at + static_cast<u32>(m_strings.size());

        Vector<u8> out;
        put_be32(out, 0xd00dfeed);
        put_be32(out, total);
        put_be32(out, structure_at);
        put_be32(out, strings_at);
        put_be32(out, header_size);
        put_be32(out, 17);
        put_be32(out, 16);
        put_be32(out, 0);
        put_be32(out, static_cast<u32>(m_strings.size()));
        put_be32(out, static_cast<u32>(m_structure.size()));
        for (u32 i = 0; i < reservation_block_size; ++i)
            out.append(0);
        for (u8 byte : m_structure)
            out.append(byte);
        for (u8 byte : m_strings)
            out.append(byte);
        return out;
    }

private:
    bool error(char const* what)
    {
        size_t line = 1;
        for (size_t i = 0; i < m_at && i < m_source.length(); ++i)
            line += m_source[i] == '\n';
        printf("mkfdt: line %zu: %s\n", line, what);
        return false;
    }

    void skip_space()
    {
        while (m_at < m_source.length()) {
            char const character = m_source[m_at];
            if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
                ++m_at;
            } else if (character == '/' && m_at + 1 < m_source.length() && m_source[m_at + 1] == '/') {
                while (m_at < m_source.length() && m_source[m_at] != '\n')
                    ++m_at;
            } else {
                return;
            }
        }
    }

    bool accept(char expected)
    {
        if (m_at < m_source.length() && m_source[m_at] == expected) {
            ++m_at;
            return true;
        }
        return false;
    }

    StringView name()
    {
        size_t const start = m_at;
        while (m_at < m_source.length()) {
            char const character = m_source[m_at];
            bool const is_letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            bool const is_digit = character >= '0' && character <= '9';
            if (!is_letter && !is_digit && character != '-' && character != '_' && character != ',' && character != '.' && character != '#' && character != '@' && character != '+')
                break;
            ++m_at;
        }
        return m_source.substring_view(start, m_at - start);
    }

    u32 string_offset(StringView text)
    {
        // The same names over and over: each once.
        for (size_t at = 0; at + text.length() < m_strings.size(); ++at) {
            if (at != 0 && m_strings[at - 1] != 0)
                continue;
            if (m_strings[at + text.length()] != 0)
                continue;
            if (__builtin_memcmp(m_strings.begin() + at, text.characters_without_null_termination(), text.length()) == 0)
                return static_cast<u32>(at);
        }
        u32 const at = static_cast<u32>(m_strings.size());
        for (char character : text)
            m_strings.append(static_cast<u8>(character));
        m_strings.append(0);
        return at;
    }

    bool number(u32& value)
    {
        size_t const start = m_at;
        u64 result = 0;
        u32 base = 10;
        if (m_at + 1 < m_source.length() && m_source[m_at] == '0' && (m_source[m_at + 1] == 'x' || m_source[m_at + 1] == 'X')) {
            base = 16;
            m_at += 2;
        }
        while (m_at < m_source.length()) {
            char const character = m_source[m_at];
            u32 digit;
            if (character >= '0' && character <= '9')
                digit = static_cast<u32>(character - '0');
            else if (base == 16 && character >= 'a' && character <= 'f')
                digit = static_cast<u32>(character - 'a') + 10;
            else if (base == 16 && character >= 'A' && character <= 'F')
                digit = static_cast<u32>(character - 'A') + 10;
            else
                break;
            result = result * base + digit;
            ++m_at;
        }
        if (m_at == start || result > 0xffffffff)
            return false;
        value = static_cast<u32>(result);
        return true;
    }

    bool property_value(Vector<u8>& value)
    {
        for (;;) {
            skip_space();
            if (accept('"')) {
                while (m_at < m_source.length() && m_source[m_at] != '"')
                    value.append(static_cast<u8>(m_source[m_at++]));
                if (!accept('"'))
                    return error("a string that doesn't end");
                value.append(0);
            } else if (accept('<')) {
                for (;;) {
                    skip_space();
                    if (accept('>'))
                        break;
                    u32 cell;
                    if (!number(cell))
                        return error("expected a number, or the closing \">\"");
                    put_be32(value, cell);
                }
            } else {
                return error("expected a string, or <cells>");
            }
            skip_space();
            if (!accept(','))
                return true;
        }
    }

    bool node_body()
    {
        for (;;) {
            skip_space();
            if (accept('}')) {
                skip_space();
                if (!accept(';'))
                    return error("expected \";\" after \"}\"");
                put_be32(m_structure, token_end_node);
                return true;
            }
            auto const item = name();
            if (item.is_empty())
                return error("expected a name");
            skip_space();
            if (accept('{')) {
                put_be32(m_structure, token_begin_node);
                for (char character : item)
                    m_structure.append(static_cast<u8>(character));
                m_structure.append(0);
                pad_to_word(m_structure);
                if (!node_body())
                    return false;
                continue;
            }
            Vector<u8> value;
            if (accept('=')) {
                if (!property_value(value))
                    return false;
                skip_space();
            }
            if (!accept(';'))
                return error("expected \";\" after a property");
            put_be32(m_structure, token_property);
            put_be32(m_structure, static_cast<u32>(value.size()));
            put_be32(m_structure, string_offset(item));
            for (u8 byte : value)
                m_structure.append(byte);
            pad_to_word(m_structure);
        }
    }

    StringView m_source;
    size_t m_at { 0 };
    Vector<u8> m_structure;
    Vector<u8> m_strings;
};

}

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: mkfdt INPUT.dts OUTPUT.dtb\n");
        return 2;
    }
    FILE* input = fopen(argv[1], "rb");
    if (!input) {
        printf("mkfdt: can't open %s\n", argv[1]);
        return 1;
    }
    constexpr int seek_set = 0;
    constexpr int seek_end = 2;
    fseek(input, 0, seek_end);
    long const size = ftell(input);
    fseek(input, 0, seek_set);
    Vector<u8> source;
    if (size < 0 || source.try_resize(static_cast<size_t>(size)).is_error() || fread(source.begin(), 1, source.size(), input) != source.size()) {
        printf("mkfdt: can't read %s\n", argv[1]);
        return 1;
    }
    fclose(input);

    Compiler compiler({ reinterpret_cast<char const*>(source.begin()), source.size() });
    if (!compiler.compile())
        return 1;
    auto const blob = compiler.finish();
    FILE* output = fopen(argv[2], "wb");
    if (!output || fwrite(blob.begin(), 1, blob.size(), output) != blob.size()) {
        printf("mkfdt: can't write %s\n", argv[2]);
        return 1;
    }
    fclose(output);
    return 0;
}
