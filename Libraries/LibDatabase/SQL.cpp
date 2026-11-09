/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibDatabase/Platform.h>
#include <LibDatabase/SQL.h>

namespace Database::SQL {

namespace {

enum class TokenKind : u8 {
    End,
    Word,
    QuotedName,
    String,
    Blob,
    Integer,
    Real,
    Parameter,
    Symbol,
};

struct Token {
    TokenKind kind { TokenKind::End };
    StringView text;
    size_t start { 0 };
    // The contents of a quoted thing, with its doubled quotes made single.
    String unquoted;
};

const Error syntax_error = Error::from_string_literal("database: that isn't valid SQL");

bool is_word_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<u8>(c) >= 0x80;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_word_part(char c)
{
    return is_word_start(c) || is_digit(c) || c == '$';
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

class Parser {
public:
    Parser(StringView text, String& message)
        : m_text(text)
        , m_message(message)
    {
    }

    ErrorOr<Statement> parse_statement();

private:
    ErrorOr<Token> read_token();
    ErrorOr<void> advance();
    bool is_word(StringView word) const { return m_token.kind == TokenKind::Word && m_token.text.equals_ignoring_ascii_case(word); }
    bool is_symbol(StringView symbol) const { return m_token.kind == TokenKind::Symbol && m_token.text == symbol; }
    ErrorOr<bool> accept_word(StringView);
    ErrorOr<bool> accept_symbol(StringView);
    ErrorOr<void> expect_word(StringView);
    ErrorOr<void> expect_symbol(StringView);
    ErrorOr<String> parse_name();
    Error fail(StringView what);

    ErrorOr<ExpressionPointer> parse_expression();
    ErrorOr<ExpressionPointer> parse_or();
    ErrorOr<ExpressionPointer> parse_and();
    ErrorOr<ExpressionPointer> parse_not();
    ErrorOr<ExpressionPointer> parse_comparison();
    ErrorOr<ExpressionPointer> parse_bits();
    ErrorOr<ExpressionPointer> parse_additive();
    ErrorOr<ExpressionPointer> parse_multiplicative();
    ErrorOr<ExpressionPointer> parse_concatenation();
    ErrorOr<ExpressionPointer> parse_unary();
    ErrorOr<ExpressionPointer> parse_primary();
    ErrorOr<ExpressionPointer> parse_case();
    ErrorOr<ExpressionPointer> parse_function(String name);

    ErrorOr<NonnullOwnPtr<Select>> parse_select();
    ErrorOr<void> parse_create(Statement&);
    ErrorOr<void> parse_column_definition(ColumnDefinition&);
    ErrorOr<void> parse_table_constraint(CreateTable&);
    ErrorOr<NonnullOwnPtr<Insert>> parse_insert();
    ErrorOr<NonnullOwnPtr<Update>> parse_update();
    ErrorOr<NonnullOwnPtr<Delete>> parse_delete();
    ErrorOr<void> parse_drop(Statement&);
    ErrorOr<void> parse_alter(Statement&);
    ErrorOr<void> parse_pragma(Statement&);
    ErrorOr<ConflictAction> parse_conflict_clause();

    StringView m_text;
    String& m_message;
    size_t m_position { 0 };
    Token m_token;
    size_t m_parameters { 0 };
};

Error Parser::fail(StringView what)
{
    StringBuilder message;
    message.append(what);
    message.append(" near \""_sv);
    message.append(m_token.kind == TokenKind::End ? "the end"_sv : m_token.text);
    message.append("\""_sv);
    m_message = MUST(message.to_string());
    return syntax_error;
}

ErrorOr<Token> Parser::read_token()
{
    // Spaces, and comments, which are as good as spaces.
    for (;;) {
        while (m_position < m_text.length() && (m_text[m_position] == ' ' || (m_text[m_position] >= '\t' && m_text[m_position] <= '\r')))
            ++m_position;
        if (m_position + 1 < m_text.length() && m_text[m_position] == '-' && m_text[m_position + 1] == '-') {
            while (m_position < m_text.length() && m_text[m_position] != '\n')
                ++m_position;
            continue;
        }
        if (m_position + 1 < m_text.length() && m_text[m_position] == '/' && m_text[m_position + 1] == '*') {
            m_position += 2;
            while (m_position + 1 < m_text.length() && !(m_text[m_position] == '*' && m_text[m_position + 1] == '/'))
                ++m_position;
            m_position = min(m_position + 2, m_text.length());
            continue;
        }
        break;
    }

    Token token;
    token.start = m_position;
    if (m_position >= m_text.length())
        return token;
    char c = m_text[m_position];

    // X'00ff': a blob.
    if ((c == 'x' || c == 'X') && m_position + 1 < m_text.length() && m_text[m_position + 1] == '\'') {
        size_t end = m_position + 2;
        StringBuilder bytes;
        while (end < m_text.length() && m_text[end] != '\'') {
            if (end + 1 >= m_text.length() || hex_digit(m_text[end]) < 0 || hex_digit(m_text[end + 1]) < 0)
                return fail("bad blob"_sv);
            bytes.append(static_cast<char>(hex_digit(m_text[end]) * 16 + hex_digit(m_text[end + 1])));
            end += 2;
        }
        if (end >= m_text.length())
            return fail("unfinished blob"_sv);
        m_position = end + 1;
        token.kind = TokenKind::Blob;
        token.text = m_text.substring_view(token.start, m_position - token.start);
        token.unquoted = TRY(bytes.to_string());
        return token;
    }

    if (is_word_start(c)) {
        while (m_position < m_text.length() && is_word_part(m_text[m_position]))
            ++m_position;
        token.kind = TokenKind::Word;
        token.text = m_text.substring_view(token.start, m_position - token.start);
        return token;
    }

    if (is_digit(c) || (c == '.' && m_position + 1 < m_text.length() && is_digit(m_text[m_position + 1]))) {
        bool real = false;
        while (m_position < m_text.length() && is_digit(m_text[m_position]))
            ++m_position;
        if (m_position < m_text.length() && m_text[m_position] == '.') {
            real = true;
            ++m_position;
            while (m_position < m_text.length() && is_digit(m_text[m_position]))
                ++m_position;
        }
        if (m_position < m_text.length() && (m_text[m_position] == 'e' || m_text[m_position] == 'E')) {
            size_t after = m_position + 1;
            if (after < m_text.length() && (m_text[after] == '+' || m_text[after] == '-'))
                ++after;
            if (after < m_text.length() && is_digit(m_text[after])) {
                real = true;
                m_position = after;
                while (m_position < m_text.length() && is_digit(m_text[m_position]))
                    ++m_position;
            }
        }
        token.kind = real ? TokenKind::Real : TokenKind::Integer;
        token.text = m_text.substring_view(token.start, m_position - token.start);
        return token;
    }

    if (c == '\'' || c == '"' || c == '`' || c == '[') {
        char closing = c == '[' ? ']' : c;
        StringBuilder contents;
        size_t end = m_position + 1;
        for (;;) {
            if (end >= m_text.length())
                return fail("unfinished string"_sv);
            if (m_text[end] == closing) {
                if (closing != ']' && end + 1 < m_text.length() && m_text[end + 1] == closing) {
                    contents.append(closing);
                    end += 2;
                    continue;
                }
                break;
            }
            contents.append(m_text[end]);
            ++end;
        }
        m_position = end + 1;
        token.kind = c == '\'' ? TokenKind::String : TokenKind::QuotedName;
        token.text = m_text.substring_view(token.start, m_position - token.start);
        token.unquoted = TRY(contents.to_string());
        return token;
    }

    if (c == '?') {
        ++m_position;
        while (m_position < m_text.length() && is_digit(m_text[m_position]))
            ++m_position;
        token.kind = TokenKind::Parameter;
        token.text = m_text.substring_view(token.start, m_position - token.start);
        return token;
    }

    static constexpr StringView two_character_symbols[] = { "||"_sv, "<="_sv, ">="_sv, "<>"_sv, "!="_sv, "=="_sv, "<<"_sv, ">>"_sv };
    if (m_position + 1 < m_text.length()) {
        auto two = m_text.substring_view(m_position, 2);
        for (auto symbol : two_character_symbols) {
            if (two == symbol) {
                m_position += 2;
                token.kind = TokenKind::Symbol;
                token.text = two;
                return token;
            }
        }
    }
    if (StringView("()+-*/%=<>,.;&|~"_sv).contains(c)) {
        ++m_position;
        token.kind = TokenKind::Symbol;
        token.text = m_text.substring_view(token.start, 1);
        return token;
    }
    token.text = m_text.substring_view(token.start, 1);
    m_token = move(token);
    return fail("unrecognized character"_sv);
}

ErrorOr<void> Parser::advance()
{
    m_token = TRY(read_token());
    return {};
}

ErrorOr<bool> Parser::accept_word(StringView word)
{
    if (!is_word(word))
        return false;
    TRY(advance());
    return true;
}

ErrorOr<bool> Parser::accept_symbol(StringView symbol)
{
    if (!is_symbol(symbol))
        return false;
    TRY(advance());
    return true;
}

ErrorOr<void> Parser::expect_word(StringView word)
{
    if (!TRY(accept_word(word)))
        return fail("syntax error"_sv);
    return {};
}

ErrorOr<void> Parser::expect_symbol(StringView symbol)
{
    if (!TRY(accept_symbol(symbol)))
        return fail("syntax error"_sv);
    return {};
}

ErrorOr<String> Parser::parse_name()
{
    if (m_token.kind == TokenKind::QuotedName || m_token.kind == TokenKind::String) {
        auto name = move(m_token.unquoted);
        TRY(advance());
        return name;
    }
    if (m_token.kind != TokenKind::Word)
        return fail("expected a name"_sv);
    auto name = TRY(String::from_view(m_token.text));
    TRY(advance());
    return name;
}

ExpressionPointer make_expression(ExpressionKind kind)
{
    auto expression = MUST(try_make<Expression>());
    expression->kind = kind;
    return expression;
}

ExpressionPointer make_binary(Operator op, ExpressionPointer left, ExpressionPointer right)
{
    auto expression = make_expression(ExpressionKind::Binary);
    expression->op = op;
    expression->children.append(move(left));
    expression->children.append(move(right));
    return expression;
}

ErrorOr<ExpressionPointer> Parser::parse_expression()
{
    return parse_or();
}

ErrorOr<ExpressionPointer> Parser::parse_or()
{
    auto left = TRY(parse_and());
    while (TRY(accept_word("OR"_sv)))
        left = make_binary(Operator::Or, move(left), TRY(parse_and()));
    return left;
}

ErrorOr<ExpressionPointer> Parser::parse_and()
{
    auto left = TRY(parse_not());
    while (TRY(accept_word("AND"_sv)))
        left = make_binary(Operator::And, move(left), TRY(parse_not()));
    return left;
}

ErrorOr<ExpressionPointer> Parser::parse_not()
{
    if (TRY(accept_word("NOT"_sv))) {
        auto expression = make_expression(ExpressionKind::Not);
        expression->children.append(TRY(parse_not()));
        return expression;
    }
    return parse_comparison();
}

ErrorOr<ExpressionPointer> Parser::parse_comparison()
{
    auto left = TRY(parse_bits());
    for (;;) {
        if (is_symbol("="_sv) || is_symbol("=="_sv)) {
            TRY(advance());
            left = make_binary(Operator::Equal, move(left), TRY(parse_bits()));
        } else if (is_symbol("!="_sv) || is_symbol("<>"_sv)) {
            TRY(advance());
            left = make_binary(Operator::NotEqual, move(left), TRY(parse_bits()));
        } else if (is_symbol("<"_sv)) {
            TRY(advance());
            left = make_binary(Operator::Less, move(left), TRY(parse_bits()));
        } else if (is_symbol("<="_sv)) {
            TRY(advance());
            left = make_binary(Operator::LessOrEqual, move(left), TRY(parse_bits()));
        } else if (is_symbol(">"_sv)) {
            TRY(advance());
            left = make_binary(Operator::Greater, move(left), TRY(parse_bits()));
        } else if (is_symbol(">="_sv)) {
            TRY(advance());
            left = make_binary(Operator::GreaterOrEqual, move(left), TRY(parse_bits()));
        } else if (is_word("IS"_sv)) {
            TRY(advance());
            bool negated = TRY(accept_word("NOT"_sv));
            left = make_binary(negated ? Operator::IsNot : Operator::Is, move(left), TRY(parse_bits()));
        } else if (is_word("ISNULL"_sv) || is_word("NOTNULL"_sv)) {
            bool negated = is_word("NOTNULL"_sv);
            TRY(advance());
            auto expression = make_expression(ExpressionKind::IsNull);
            expression->negated = negated;
            expression->children.append(move(left));
            left = move(expression);
        } else if (is_word("NOT"_sv) || is_word("LIKE"_sv) || is_word("GLOB"_sv) || is_word("BETWEEN"_sv) || is_word("IN"_sv)) {
            bool negated = TRY(accept_word("NOT"_sv));
            if (is_word("LIKE"_sv) || is_word("GLOB"_sv)) {
                bool glob = is_word("GLOB"_sv);
                TRY(advance());
                auto expression = make_expression(ExpressionKind::Like);
                expression->negated = negated;
                expression->glob = glob;
                expression->children.append(move(left));
                expression->children.append(TRY(parse_bits()));
                left = move(expression);
            } else if (TRY(accept_word("BETWEEN"_sv))) {
                auto expression = make_expression(ExpressionKind::Between);
                expression->negated = negated;
                expression->children.append(move(left));
                expression->children.append(TRY(parse_bits()));
                TRY(expect_word("AND"_sv));
                expression->children.append(TRY(parse_bits()));
                left = move(expression);
            } else if (TRY(accept_word("IN"_sv))) {
                auto expression = make_expression(ExpressionKind::In);
                expression->negated = negated;
                expression->children.append(move(left));
                TRY(expect_symbol("("_sv));
                if (!is_symbol(")"_sv)) {
                    do {
                        expression->children.append(TRY(parse_expression()));
                    } while (TRY(accept_symbol(","_sv)));
                }
                TRY(expect_symbol(")"_sv));
                left = move(expression);
            } else {
                return fail("syntax error"_sv);
            }
        } else {
            return left;
        }
    }
}

ErrorOr<ExpressionPointer> Parser::parse_bits()
{
    auto left = TRY(parse_additive());
    for (;;) {
        if (TRY(accept_symbol("&"_sv)))
            left = make_binary(Operator::BitAnd, move(left), TRY(parse_additive()));
        else if (TRY(accept_symbol("|"_sv)))
            left = make_binary(Operator::BitOr, move(left), TRY(parse_additive()));
        else if (TRY(accept_symbol("<<"_sv)))
            left = make_binary(Operator::ShiftLeft, move(left), TRY(parse_additive()));
        else if (TRY(accept_symbol(">>"_sv)))
            left = make_binary(Operator::ShiftRight, move(left), TRY(parse_additive()));
        else
            return left;
    }
}

ErrorOr<ExpressionPointer> Parser::parse_additive()
{
    auto left = TRY(parse_multiplicative());
    for (;;) {
        if (TRY(accept_symbol("+"_sv)))
            left = make_binary(Operator::Add, move(left), TRY(parse_multiplicative()));
        else if (TRY(accept_symbol("-"_sv)))
            left = make_binary(Operator::Subtract, move(left), TRY(parse_multiplicative()));
        else
            return left;
    }
}

ErrorOr<ExpressionPointer> Parser::parse_multiplicative()
{
    auto left = TRY(parse_concatenation());
    for (;;) {
        if (TRY(accept_symbol("*"_sv)))
            left = make_binary(Operator::Multiply, move(left), TRY(parse_concatenation()));
        else if (TRY(accept_symbol("/"_sv)))
            left = make_binary(Operator::Divide, move(left), TRY(parse_concatenation()));
        else if (TRY(accept_symbol("%"_sv)))
            left = make_binary(Operator::Modulo, move(left), TRY(parse_concatenation()));
        else
            return left;
    }
}

ErrorOr<ExpressionPointer> Parser::parse_concatenation()
{
    auto left = TRY(parse_unary());
    while (TRY(accept_symbol("||"_sv)))
        left = make_binary(Operator::Concatenate, move(left), TRY(parse_unary()));
    return left;
}

ErrorOr<ExpressionPointer> Parser::parse_unary()
{
    if (TRY(accept_symbol("-"_sv))) {
        auto expression = make_expression(ExpressionKind::Negate);
        expression->children.append(TRY(parse_unary()));
        return expression;
    }
    if (TRY(accept_symbol("+"_sv)))
        return parse_unary();
    if (TRY(accept_symbol("~"_sv))) {
        auto expression = make_expression(ExpressionKind::BitwiseNot);
        expression->children.append(TRY(parse_unary()));
        return expression;
    }
    return parse_primary();
}

ErrorOr<ExpressionPointer> Parser::parse_case()
{
    auto expression = make_expression(ExpressionKind::Case);
    if (!is_word("WHEN"_sv)) {
        expression->has_operand = true;
        expression->children.append(TRY(parse_expression()));
    }
    while (TRY(accept_word("WHEN"_sv))) {
        expression->children.append(TRY(parse_expression()));
        TRY(expect_word("THEN"_sv));
        expression->children.append(TRY(parse_expression()));
    }
    if (TRY(accept_word("ELSE"_sv))) {
        expression->has_else = true;
        expression->children.append(TRY(parse_expression()));
    }
    TRY(expect_word("END"_sv));
    return expression;
}

ErrorOr<ExpressionPointer> Parser::parse_function(String name)
{
    auto expression = make_expression(ExpressionKind::Function);
    expression->name = move(name);
    TRY(expect_symbol("("_sv));
    if (TRY(accept_symbol("*"_sv))) {
        expression->children.append(make_expression(ExpressionKind::Star));
    } else if (!is_symbol(")"_sv)) {
        expression->distinct = TRY(accept_word("DISTINCT"_sv));
        do {
            expression->children.append(TRY(parse_expression()));
        } while (TRY(accept_symbol(","_sv)));
    }
    TRY(expect_symbol(")"_sv));
    return expression;
}

ErrorOr<ExpressionPointer> Parser::parse_primary()
{
    switch (m_token.kind) {
    case TokenKind::Integer: {
        auto expression = make_expression(ExpressionKind::Literal);
        if (auto number = m_token.text.to_number<i64>(); number.has_value()) {
            expression->literal = Value::integer(number.value());
        } else {
            auto terminated = TRY(String::from_view(m_token.text));
            expression->literal = Value::real(strtod(terminated.characters(), nullptr));
        }
        TRY(advance());
        return expression;
    }
    case TokenKind::Real: {
        auto expression = make_expression(ExpressionKind::Literal);
        auto terminated = TRY(String::from_view(m_token.text));
        expression->literal = Value::real(strtod(terminated.characters(), nullptr));
        TRY(advance());
        return expression;
    }
    case TokenKind::String: {
        auto expression = make_expression(ExpressionKind::Literal);
        expression->literal = Value::text(m_token.unquoted.view());
        TRY(advance());
        return expression;
    }
    case TokenKind::Blob: {
        auto expression = make_expression(ExpressionKind::Literal);
        expression->literal = Value::blob(m_token.unquoted.view().bytes());
        TRY(advance());
        return expression;
    }
    case TokenKind::Parameter: {
        auto expression = make_expression(ExpressionKind::Parameter);
        if (m_token.text.length() > 1) {
            auto number = m_token.text.substring_view(1).to_number<u32>();
            if (!number.has_value() || number.value() == 0)
                return fail("bad parameter"_sv);
            expression->parameter = number.value() - 1;
            m_parameters = max(m_parameters, static_cast<size_t>(number.value()));
        } else {
            expression->parameter = m_parameters++;
        }
        TRY(advance());
        return expression;
    }
    case TokenKind::Symbol: {
        if (TRY(accept_symbol("("_sv))) {
            auto expression = TRY(parse_expression());
            TRY(expect_symbol(")"_sv));
            return expression;
        }
        return fail("syntax error"_sv);
    }
    case TokenKind::QuotedName:
    case TokenKind::Word: {
        if (m_token.kind == TokenKind::Word) {
            if (TRY(accept_word("NULL"_sv)))
                return make_expression(ExpressionKind::Literal);
            if (is_word("TRUE"_sv) || is_word("FALSE"_sv)) {
                auto expression = make_expression(ExpressionKind::Literal);
                expression->literal = Value::integer(is_word("TRUE"_sv) ? 1 : 0);
                TRY(advance());
                return expression;
            }
            if (TRY(accept_word("CASE"_sv)))
                return parse_case();
            if (TRY(accept_word("CAST"_sv))) {
                TRY(expect_symbol("("_sv));
                auto expression = make_expression(ExpressionKind::Cast);
                expression->children.append(TRY(parse_expression()));
                TRY(expect_word("AS"_sv));
                StringBuilder type;
                while (m_token.kind == TokenKind::Word) {
                    if (type.length())
                        type.append(' ');
                    type.append(m_token.text);
                    TRY(advance());
                }
                expression->name = TRY(type.to_string());
                TRY(expect_symbol(")"_sv));
                return expression;
            }
        }
        auto first = TRY(parse_name());
        if (is_symbol("("_sv))
            return parse_function(move(first));
        auto expression = make_expression(ExpressionKind::Column);
        if (is_symbol("."_sv)) {
            // t.* isn't an expression: it's left for the list of results to read.
            size_t const position = m_position;
            Token const dot = m_token;
            TRY(advance());
            if (is_symbol("*"_sv)) {
                m_position = position;
                m_token = dot;
                expression->name = move(first);
                return expression;
            }
            expression->table = move(first);
            expression->name = TRY(parse_name());
        } else {
            expression->name = move(first);
        }
        return expression;
    }
    default:
        return fail("syntax error"_sv);
    }
}

ErrorOr<NonnullOwnPtr<Select>> Parser::parse_select()
{
    TRY(expect_word("SELECT"_sv));
    auto select = TRY(try_make<Select>());
    select->distinct = TRY(accept_word("DISTINCT"_sv));
    if (!select->distinct)
        (void)TRY(accept_word("ALL"_sv));
    do {
        ResultColumn column;
        if (TRY(accept_symbol("*"_sv))) {
            column.star = true;
            select->columns.append(move(column));
            continue;
        }
        auto expression = TRY(parse_expression());
        // t.*: read as a column t, and then a dot and a star.
        if (expression->kind == ExpressionKind::Column && expression->table.is_empty() && is_symbol("."_sv)) {
            TRY(advance());
            TRY(expect_symbol("*"_sv));
            column.star = true;
            column.star_table = expression->name;
            select->columns.append(move(column));
            continue;
        }
        if (TRY(accept_word("AS"_sv)))
            column.alias = TRY(parse_name());
        else if (m_token.kind == TokenKind::QuotedName || (m_token.kind == TokenKind::Word && !is_word("FROM"_sv) && !is_word("WHERE"_sv) && !is_word("GROUP"_sv) && !is_word("ORDER"_sv) && !is_word("LIMIT"_sv) && !is_word("HAVING"_sv)))
            column.alias = TRY(parse_name());
        column.expression = move(expression);
        select->columns.append(move(column));
    } while (TRY(accept_symbol(","_sv)));

    auto is_join_word = [&] { return is_word("JOIN"_sv) || is_word("LEFT"_sv) || is_word("INNER"_sv) || is_word("CROSS"_sv) || is_word("ON"_sv); };
    auto is_clause_word = [&] { return is_word("WHERE"_sv) || is_word("GROUP"_sv) || is_word("ORDER"_sv) || is_word("LIMIT"_sv) || is_word("HAVING"_sv); };

    if (TRY(accept_word("FROM"_sv))) {
        select->has_from = true;
        select->table = TRY(parse_name());
        if (TRY(accept_word("AS"_sv)))
            select->alias = TRY(parse_name());
        else if (m_token.kind == TokenKind::Word && !is_join_word() && !is_clause_word())
            select->alias = TRY(parse_name());
        for (;;) {
            Join join;
            if (TRY(accept_symbol(","_sv))) {
                // The same as a join with no condition.
            } else if (TRY(accept_word("LEFT"_sv))) {
                (void)TRY(accept_word("OUTER"_sv));
                TRY(expect_word("JOIN"_sv));
                join.left = true;
            } else if (TRY(accept_word("INNER"_sv)) || TRY(accept_word("CROSS"_sv))) {
                TRY(expect_word("JOIN"_sv));
            } else if (!TRY(accept_word("JOIN"_sv))) {
                break;
            }
            join.table = TRY(parse_name());
            if (TRY(accept_word("AS"_sv)))
                join.alias = TRY(parse_name());
            else if (m_token.kind == TokenKind::Word && !is_join_word() && !is_clause_word())
                join.alias = TRY(parse_name());
            if (TRY(accept_word("ON"_sv)))
                join.on = TRY(parse_expression());
            select->joins.append(move(join));
        }
    }
    if (TRY(accept_word("WHERE"_sv)))
        select->where = TRY(parse_expression());
    if (TRY(accept_word("GROUP"_sv))) {
        TRY(expect_word("BY"_sv));
        do {
            select->group_by.append(TRY(parse_expression()));
        } while (TRY(accept_symbol(","_sv)));
        if (TRY(accept_word("HAVING"_sv)))
            select->having = TRY(parse_expression());
    }
    if (TRY(accept_word("ORDER"_sv))) {
        TRY(expect_word("BY"_sv));
        do {
            auto expression = TRY(parse_expression());
            bool descending = false;
            if (TRY(accept_word("DESC"_sv)))
                descending = true;
            else
                (void)TRY(accept_word("ASC"_sv));
            select->order_by.append(OrderTerm { move(expression), descending });
        } while (TRY(accept_symbol(","_sv)));
    }
    if (TRY(accept_word("LIMIT"_sv))) {
        select->limit = TRY(parse_expression());
        if (TRY(accept_word("OFFSET"_sv))) {
            select->offset = TRY(parse_expression());
        } else if (TRY(accept_symbol(","_sv))) {
            // LIMIT skip, count.
            select->offset = move(select->limit);
            select->limit = TRY(parse_expression());
        }
    }
    return select;
}

ErrorOr<void> Parser::parse_column_definition(ColumnDefinition& column)
{
    column.name = TRY(parse_name());
    StringBuilder type;
    while (m_token.kind == TokenKind::Word && !is_word("PRIMARY"_sv) && !is_word("NOT"_sv) && !is_word("UNIQUE"_sv) && !is_word("DEFAULT"_sv) && !is_word("NULL"_sv) && !is_word("CHECK"_sv) && !is_word("COLLATE"_sv) && !is_word("REFERENCES"_sv)) {
        if (type.length())
            type.append(' ');
        type.append(m_token.text);
        TRY(advance());
    }
    // VARCHAR(40): the number means nothing here.
    if (TRY(accept_symbol("("_sv))) {
        type.append('(');
        while (!is_symbol(")"_sv)) {
            if (m_token.kind == TokenKind::End)
                return fail("unfinished type"_sv);
            type.append(m_token.text);
            TRY(advance());
        }
        type.append(')');
        TRY(advance());
    }
    column.type = TRY(type.to_string());
    for (;;) {
        if (TRY(accept_word("PRIMARY"_sv))) {
            TRY(expect_word("KEY"_sv));
            column.primary_key = true;
            if (!TRY(accept_word("ASC"_sv)))
                (void)TRY(accept_word("DESC"_sv));
            column.autoincrement = TRY(accept_word("AUTOINCREMENT"_sv));
        } else if (TRY(accept_word("NOT"_sv))) {
            TRY(expect_word("NULL"_sv));
            column.not_null = true;
        } else if (TRY(accept_word("NULL"_sv))) {
            // Which is what it was going to be anyway.
        } else if (TRY(accept_word("UNIQUE"_sv))) {
            column.unique = true;
        } else if (TRY(accept_word("DEFAULT"_sv))) {
            if (TRY(accept_symbol("("_sv))) {
                column.default_value = TRY(parse_expression());
                TRY(expect_symbol(")"_sv));
            } else {
                column.default_value = TRY(parse_unary());
            }
        } else {
            return {};
        }
    }
}

ErrorOr<void> Parser::parse_table_constraint(CreateTable& table)
{
    bool primary = false;
    if (TRY(accept_word("PRIMARY"_sv))) {
        TRY(expect_word("KEY"_sv));
        primary = true;
    } else {
        TRY(expect_word("UNIQUE"_sv));
    }
    TRY(expect_symbol("("_sv));
    Vector<String> columns;
    do {
        columns.append(TRY(parse_name()));
        if (!TRY(accept_word("ASC"_sv)))
            (void)TRY(accept_word("DESC"_sv));
    } while (TRY(accept_symbol(","_sv)));
    TRY(expect_symbol(")"_sv));
    if (primary)
        table.primary_key = move(columns);
    else
        table.unique_sets.append(move(columns));
    return {};
}

ErrorOr<void> Parser::parse_create(Statement& statement)
{
    TRY(expect_word("CREATE"_sv));
    bool unique = TRY(accept_word("UNIQUE"_sv));
    if (TRY(accept_word("INDEX"_sv))) {
        statement.kind = StatementKind::CreateIndex;
        auto index = TRY(try_make<CreateIndex>());
        index->unique = unique;
        if (TRY(accept_word("IF"_sv))) {
            TRY(expect_word("NOT"_sv));
            TRY(expect_word("EXISTS"_sv));
            index->if_not_exists = true;
        }
        index->name = TRY(parse_name());
        TRY(expect_word("ON"_sv));
        index->table = TRY(parse_name());
        TRY(expect_symbol("("_sv));
        do {
            index->columns.append(TRY(parse_name()));
            if (!TRY(accept_word("ASC"_sv)))
                (void)TRY(accept_word("DESC"_sv));
        } while (TRY(accept_symbol(","_sv)));
        TRY(expect_symbol(")"_sv));
        statement.create_index = move(index);
        return {};
    }
    if (unique)
        return fail("syntax error"_sv);
    TRY(expect_word("TABLE"_sv));
    statement.kind = StatementKind::CreateTable;
    auto table = TRY(try_make<CreateTable>());
    if (TRY(accept_word("IF"_sv))) {
        TRY(expect_word("NOT"_sv));
        TRY(expect_word("EXISTS"_sv));
        table->if_not_exists = true;
    }
    table->name = TRY(parse_name());
    TRY(expect_symbol("("_sv));
    do {
        if (is_word("PRIMARY"_sv) || is_word("UNIQUE"_sv)) {
            TRY(parse_table_constraint(*table));
            continue;
        }
        ColumnDefinition column;
        TRY(parse_column_definition(column));
        table->columns.append(move(column));
    } while (TRY(accept_symbol(","_sv)));
    TRY(expect_symbol(")"_sv));
    statement.create_table = move(table);
    return {};
}

ErrorOr<ConflictAction> Parser::parse_conflict_clause()
{
    if (!TRY(accept_word("OR"_sv)))
        return ConflictAction::Abort;
    if (TRY(accept_word("REPLACE"_sv)))
        return ConflictAction::Replace;
    if (TRY(accept_word("IGNORE"_sv)))
        return ConflictAction::Ignore;
    if (TRY(accept_word("ABORT"_sv)))
        return ConflictAction::Abort;
    return fail("syntax error"_sv);
}

ErrorOr<NonnullOwnPtr<Insert>> Parser::parse_insert()
{
    auto insert = TRY(try_make<Insert>());
    if (TRY(accept_word("REPLACE"_sv))) {
        insert->on_conflict = ConflictAction::Replace;
    } else {
        TRY(expect_word("INSERT"_sv));
        insert->on_conflict = TRY(parse_conflict_clause());
    }
    TRY(expect_word("INTO"_sv));
    insert->table = TRY(parse_name());
    if (TRY(accept_symbol("("_sv))) {
        do {
            insert->columns.append(TRY(parse_name()));
        } while (TRY(accept_symbol(","_sv)));
        TRY(expect_symbol(")"_sv));
    }
    if (TRY(accept_word("DEFAULT"_sv))) {
        TRY(expect_word("VALUES"_sv));
        insert->default_values = true;
        return insert;
    }
    if (is_word("SELECT"_sv)) {
        insert->select = TRY(parse_select());
        return insert;
    }
    TRY(expect_word("VALUES"_sv));
    do {
        TRY(expect_symbol("("_sv));
        Vector<ExpressionPointer> row;
        do {
            row.append(TRY(parse_expression()));
        } while (TRY(accept_symbol(","_sv)));
        TRY(expect_symbol(")"_sv));
        insert->rows.append(move(row));
    } while (TRY(accept_symbol(","_sv)));
    return insert;
}

ErrorOr<NonnullOwnPtr<Update>> Parser::parse_update()
{
    auto update = TRY(try_make<Update>());
    TRY(expect_word("UPDATE"_sv));
    update->on_conflict = TRY(parse_conflict_clause());
    update->table = TRY(parse_name());
    TRY(expect_word("SET"_sv));
    do {
        auto column = TRY(parse_name());
        TRY(expect_symbol("="_sv));
        update->assignments.append(Assignment { move(column), TRY(parse_expression()) });
    } while (TRY(accept_symbol(","_sv)));
    if (TRY(accept_word("WHERE"_sv)))
        update->where = TRY(parse_expression());
    return update;
}

ErrorOr<NonnullOwnPtr<Delete>> Parser::parse_delete()
{
    auto remove = TRY(try_make<Delete>());
    TRY(expect_word("DELETE"_sv));
    TRY(expect_word("FROM"_sv));
    remove->table = TRY(parse_name());
    if (TRY(accept_word("WHERE"_sv)))
        remove->where = TRY(parse_expression());
    return remove;
}

ErrorOr<void> Parser::parse_drop(Statement& statement)
{
    TRY(expect_word("DROP"_sv));
    if (TRY(accept_word("TABLE"_sv))) {
        statement.kind = StatementKind::DropTable;
    } else {
        TRY(expect_word("INDEX"_sv));
        statement.kind = StatementKind::DropIndex;
    }
    if (TRY(accept_word("IF"_sv))) {
        TRY(expect_word("EXISTS"_sv));
        statement.if_exists = true;
    }
    statement.name = TRY(parse_name());
    return {};
}

ErrorOr<void> Parser::parse_alter(Statement& statement)
{
    TRY(expect_word("ALTER"_sv));
    TRY(expect_word("TABLE"_sv));
    statement.name = TRY(parse_name());
    TRY(expect_word("ADD"_sv));
    (void)TRY(accept_word("COLUMN"_sv));
    statement.kind = StatementKind::AddColumn;
    auto column = TRY(try_make<ColumnDefinition>());
    TRY(parse_column_definition(*column));
    statement.added_column = move(column);
    return {};
}

ErrorOr<void> Parser::parse_pragma(Statement& statement)
{
    TRY(expect_word("PRAGMA"_sv));
    if (!TRY(accept_word("integrity_check"_sv)))
        return fail("unknown pragma"_sv);
    statement.kind = StatementKind::IntegrityCheck;
    return {};
}

ErrorOr<Statement> Parser::parse_statement()
{
    TRY(advance());
    size_t const start = m_token.start;
    Statement statement;
    if (is_word("SELECT"_sv)) {
        statement.kind = StatementKind::Select;
        statement.select = TRY(parse_select());
    } else if (is_word("CREATE"_sv)) {
        TRY(parse_create(statement));
    } else if (is_word("INSERT"_sv) || is_word("REPLACE"_sv)) {
        statement.kind = StatementKind::Insert;
        statement.insert = TRY(parse_insert());
    } else if (is_word("UPDATE"_sv)) {
        statement.kind = StatementKind::Update;
        statement.update = TRY(parse_update());
    } else if (is_word("DELETE"_sv)) {
        statement.kind = StatementKind::Delete;
        statement.remove = TRY(parse_delete());
    } else if (is_word("DROP"_sv)) {
        TRY(parse_drop(statement));
    } else if (is_word("ALTER"_sv)) {
        TRY(parse_alter(statement));
    } else if (is_word("PRAGMA"_sv)) {
        TRY(parse_pragma(statement));
    } else if (TRY(accept_word("BEGIN"_sv))) {
        (void)TRY(accept_word("TRANSACTION"_sv));
        statement.kind = StatementKind::Begin;
    } else if (TRY(accept_word("COMMIT"_sv)) || TRY(accept_word("END"_sv))) {
        (void)TRY(accept_word("TRANSACTION"_sv));
        statement.kind = StatementKind::Commit;
    } else if (TRY(accept_word("ROLLBACK"_sv))) {
        (void)TRY(accept_word("TRANSACTION"_sv));
        statement.kind = StatementKind::Rollback;
    } else {
        return fail("syntax error"_sv);
    }
    size_t const end = m_token.start;
    (void)TRY(accept_symbol(";"_sv));
    if (m_token.kind != TokenKind::End)
        return fail("syntax error"_sv);
    statement.parameter_count = m_parameters;
    auto used = m_text.substring_view(start, end - start).trim_whitespace();
    statement.text = TRY(String::from_view(used));
    return statement;
}

}

ErrorOr<Statement> parse(StringView text, String& message)
{
    Parser parser(text, message);
    return parser.parse_statement();
}

}
