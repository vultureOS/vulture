/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QueryLanguage.h"
#include <stdlib.h>
#include <time.h>

namespace Index {

namespace {

constexpr Attribute attributes[] = {
    { "kMDItemContentCreationDate"_sv, "created"_sv, true },
    { "kMDItemContentModificationDate"_sv, "modified"_sv, true },
    { "kMDItemContentType"_sv, "content_type"_sv, false },
    { "kMDItemDisplayName"_sv, "name"_sv, false },
    { "kMDItemFSContentChangeDate"_sv, "modified"_sv, true },
    { "kMDItemFSCreationDate"_sv, "created"_sv, true },
    { "kMDItemFSName"_sv, "name"_sv, false },
    { "kMDItemFSSize"_sv, "size"_sv, false },
    { "kMDItemKind"_sv, "kind_description"_sv, false },
    { "kMDItemPath"_sv, "path"_sv, false },
    { "kMDItemPixelHeight"_sv, "pixel_height"_sv, false },
    { "kMDItemPixelWidth"_sv, "pixel_width"_sv, false },
    { "kMDItemVersion"_sv, "version"_sv, false },
};

const Error query_error = Error::from_string_literal("indexd: that isn't a query");

class Translator {
public:
    Translator(StringView text, TranslatedQuery& query, String& message)
        : m_text(text)
        , m_query(query)
        , m_message(message)
    {
    }

    ErrorOr<void> run(StringBuilder& out)
    {
        TRY(parse_or(out));
        skip_spaces();
        if (m_position != m_text.length())
            return fail("something after the end of the query"_sv);
        return {};
    }

private:
    Error fail(StringView what)
    {
        m_message = MUST(String::from_view(what));
        return query_error;
    }

    void skip_spaces()
    {
        while (m_position < m_text.length() && m_text[m_position] == ' ')
            ++m_position;
    }

    bool accept(StringView symbol)
    {
        skip_spaces();
        if (!m_text.substring_view(m_position).starts_with(symbol))
            return false;
        m_position += symbol.length();
        return true;
    }

    ErrorOr<void> parse_or(StringBuilder& out)
    {
        TRY(parse_and(out));
        while (accept("||"_sv)) {
            out.append(" OR "_sv);
            TRY(parse_and(out));
        }
        return {};
    }

    ErrorOr<void> parse_and(StringBuilder& out)
    {
        TRY(parse_term(out));
        while (accept("&&"_sv)) {
            out.append(" AND "_sv);
            TRY(parse_term(out));
        }
        return {};
    }

    ErrorOr<void> parse_term(StringBuilder& out)
    {
        if (accept("("_sv)) {
            out.append('(');
            TRY(parse_or(out));
            if (!accept(")"_sv))
                return fail("a ( with no )"_sv);
            out.append(')');
            return {};
        }
        return parse_comparison(out);
    }

    ErrorOr<void> parse_comparison(StringBuilder& out)
    {
        skip_spaces();
        size_t start = m_position;
        while (m_position < m_text.length() && (m_text[m_position] == '_' || (m_text[m_position] >= 'a' && m_text[m_position] <= 'z') || (m_text[m_position] >= 'A' && m_text[m_position] <= 'Z') || (m_text[m_position] >= '0' && m_text[m_position] <= '9')))
            ++m_position;
        auto name = m_text.substring_view(start, m_position - start);
        Attribute const* attribute = nullptr;
        for (auto const& candidate : attributes) {
            if (candidate.name == name)
                attribute = &candidate;
        }
        if (!attribute) {
            StringBuilder message;
            message.appendff("no attribute is called {}", name);
            return fail(message.view());
        }

        StringView op;
        StringView const operators[] = { "=="_sv, "!="_sv, "<="_sv, ">="_sv, "<"_sv, ">"_sv };
        for (auto candidate : operators) {
            if (accept(candidate)) {
                op = candidate;
                break;
            }
        }
        if (op.is_empty())
            return fail("expected a comparison"_sv);

        skip_spaces();
        if (m_position < m_text.length() && m_text[m_position] == '"')
            return parse_string_comparison(out, *attribute, op);
        return parse_number_comparison(out, *attribute, op);
    }

    ErrorOr<void> parse_string_comparison(StringBuilder& out, Attribute const& attribute, StringView op)
    {
        ++m_position;
        StringBuilder value;
        while (m_position < m_text.length() && m_text[m_position] != '"') {
            if (m_text[m_position] == '\\' && m_position + 1 < m_text.length())
                ++m_position;
            value.append(m_text[m_position++]);
        }
        if (m_position >= m_text.length())
            return fail("a \" with no end"_sv);
        ++m_position;
        // c: whatever the case; d: whatever the accents (which folding has as well).
        bool ignore_case = false;
        while (m_position < m_text.length() && (m_text[m_position] == 'c' || m_text[m_position] == 'd' || m_text[m_position] == 'w')) {
            if (m_text[m_position] == 'c')
                ignore_case = true;
            ++m_position;
        }
        auto pattern = value.view();
        bool const wildcard = pattern.contains('*');
        if (wildcard && op != "=="_sv && op != "!="_sv)
            return fail("a wildcard can only be equal or not"_sv);
        if (ignore_case)
            out.appendff("lower({})", attribute.column);
        else
            out.append(attribute.column);
        if (wildcard) {
            out.append(op == "=="_sv ? " GLOB "_sv : " NOT GLOB "_sv);
        } else {
            out.appendff(" {} ", op == "=="_sv ? "="_sv : op);
        }
        out.append(ignore_case ? "lower(?)"_sv : "?"_sv);
        m_query.parameters.append(Database::Value::text(pattern));
        return {};
    }

    ErrorOr<void> parse_number_comparison(StringBuilder& out, Attribute const& attribute, StringView op)
    {
        skip_spaces();
        i64 value = 0;
        if (m_text.substring_view(m_position).starts_with("$time."_sv)) {
            value = TRY(parse_time());
        } else {
            size_t start = m_position;
            if (m_position < m_text.length() && m_text[m_position] == '-')
                ++m_position;
            while (m_position < m_text.length() && m_text[m_position] >= '0' && m_text[m_position] <= '9')
                ++m_position;
            auto number = m_text.substring_view(start, m_position - start).to_number<i64>();
            if (!number.has_value())
                return fail("expected a number, or a date, or something in quotes"_sv);
            value = number.value();
        }
        out.appendff("{} {} ?", attribute.column, op == "=="_sv ? "="_sv : op);
        m_query.parameters.append(Database::Value::integer(value));
        return {};
    }

    ErrorOr<i64> parse_time()
    {
        m_position += 6;
        size_t start = m_position;
        while (m_position < m_text.length() && m_text[m_position] >= 'a' && m_text[m_position] <= 'z')
            ++m_position;
        auto which = m_text.substring_view(start, m_position - start);
        i64 argument = 0;
        if (accept("("_sv)) {
            size_t argument_start = m_position;
            while (m_position < m_text.length() && m_text[m_position] != ')')
                ++m_position;
            auto text = m_text.substring_view(argument_start, m_position - argument_start).trim_whitespace();
            if (!accept(")"_sv))
                return fail("a ( with no )"_sv);
            if (!text.is_empty())
                argument = text.to_number<i64>().value_or(0);
        }
        i64 const now = static_cast<i64>(time(nullptr));
        if (which == "now"_sv)
            return now + argument;
        if (which == "today"_sv)
            return now - now % 86400 + argument * 86400;
        return fail("$time.now() and $time.today() are what there is"_sv);
    }

    StringView m_text;
    TranslatedQuery& m_query;
    String& m_message;
    size_t m_position { 0 };
};

}

Span<Attribute const> known_attributes()
{
    return { attributes, sizeof(attributes) / sizeof(attributes[0]) };
}

bool looks_like_query_language(StringView text)
{
    return text.contains("kMDItem"_sv);
}

ErrorOr<TranslatedQuery> translate_query(StringView text, String& message)
{
    TranslatedQuery query;
    StringBuilder where;
    Translator translator(text, query, message);
    TRY(translator.run(where));
    query.where = TRY(where.to_string());
    return query;
}

}
