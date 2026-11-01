/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Syntax.h"
#include <LibBase/CharacterTypes.h>

namespace Shell {

namespace {

struct Token {
    enum class Type {
        Word,
        Operator,
        Newline,
        End,
    };
    Type type { Type::End };
    String text;
    // An IO number ("2" in "2>file") before a redirection operator.
    int io_number { -1 };
    size_t start { 0 };
    size_t end { 0 };
    // Here-document delimiters: the body read after the line.
    bool here_document_delimiter { false };
    bool strip_tabs { false };
    String here_document;
    bool here_document_quoted { false };
};

bool is_operator_start(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

// Splits the whole input into tokens, reading here-document bodies at the
// end of the lines that announce them.
class Lexer {
public:
    explicit Lexer(StringView source)
        : m_source(source)
    {
    }

    bool tokenize(Vector<Token>& tokens)
    {
        for (;;) {
            Token token;
            if (!next(token))
                return false;
            bool is_end = token.type == Token::Type::End;
            if (token.type == Token::Type::Newline && !m_pending.is_empty()) {
                if (!read_here_documents(tokens))
                    return false;
            }
            if (token.type == Token::Type::Word && m_expect_delimiter) {
                token.here_document_delimiter = true;
                token.strip_tabs = m_strip_tabs;
                m_expect_delimiter = false;
                tokens.append(move(token));
                m_pending.append(tokens.size() - 1);
                continue;
            }
            if (token.type == Token::Type::Operator && (token.text.view() == "<<"_sv || token.text.view() == "<<-"_sv)) {
                m_expect_delimiter = true;
                m_strip_tabs = token.text.view() == "<<-"_sv;
            }
            tokens.append(move(token));
            if (is_end) {
                if (!m_pending.is_empty()) {
                    m_incomplete = true;
                    return false;
                }
                return true;
            }
        }
    }

    [[nodiscard]] bool is_incomplete() const { return m_incomplete; }
    [[nodiscard]] StringView error() const { return m_error; }

private:
    [[nodiscard]] bool at_end() const { return m_position >= m_source.length(); }
    [[nodiscard]] char peek(size_t ahead = 0) const { return m_position + ahead < m_source.length() ? m_source[m_position + ahead] : '\0'; }

    bool next(Token& token)
    {
        // Blanks, line continuations and comments.
        for (;;) {
            while (!at_end() && is_blank(peek()))
                ++m_position;
            if (peek() == '\\' && peek(1) == '\n') {
                m_position += 2;
                continue;
            }
            if (peek() == '#') {
                while (!at_end() && peek() != '\n')
                    ++m_position;
                continue;
            }
            break;
        }
        token.start = m_position;
        if (at_end()) {
            token.type = Token::Type::End;
            token.end = m_position;
            return true;
        }
        if (peek() == '\n') {
            ++m_position;
            token.type = Token::Type::Newline;
            token.end = m_position;
            return true;
        }
        if (is_operator_start(peek()))
            return lex_operator(token);
        // An IO number: digits right before < or >.
        size_t digits = 0;
        while (Base::is_ascii_digit(static_cast<u8>(peek(digits))))
            ++digits;
        if (digits > 0 && digits < 4 && (peek(digits) == '<' || peek(digits) == '>')) {
            int number = 0;
            for (size_t i = 0; i < digits; ++i)
                number = number * 10 + (peek(i) - '0');
            m_position += digits;
            if (!lex_operator(token))
                return false;
            token.io_number = number;
            token.start -= digits;
            return true;
        }
        return lex_word(token);
    }

    bool lex_operator(Token& token)
    {
        static StringView const operators[] = { "<<<"_sv, "<<-"_sv, "&&"_sv, "||"_sv, ";;"_sv, "<<"_sv, ">>"_sv, "<&"_sv, ">&"_sv, "<>"_sv,
            ">|"_sv, "&>"_sv, "|"_sv, "&"_sv, ";"_sv, "<"_sv, ">"_sv, "("_sv, ")"_sv };
        for (auto candidate : operators) {
            if (m_source.substring_view(m_position).starts_with(candidate)) {
                token.type = Token::Type::Operator;
                token.text = MUST(String::from_view(candidate));
                m_position += candidate.length();
                token.end = m_position;
                return true;
            }
        }
        m_error = "unexpected character"_sv;
        return false;
    }

    // Skips a balanced construct opened just before m_position, honoring
    // quotes inside it.
    bool skip_balanced(char open, char close)
    {
        int depth = 1;
        while (!at_end()) {
            char c = peek();
            if (c == '\\') {
                m_position += 2;
                continue;
            }
            if (c == '\'') {
                ++m_position;
                while (!at_end() && peek() != '\'')
                    ++m_position;
                if (at_end())
                    break;
                ++m_position;
                continue;
            }
            if (c == '"') {
                ++m_position;
                if (!skip_double_quoted())
                    return false;
                continue;
            }
            if (c == '`') {
                ++m_position;
                if (!skip_backquoted())
                    return false;
                continue;
            }
            ++m_position;
            if (c == open)
                ++depth;
            else if (c == close && --depth == 0)
                return true;
        }
        m_incomplete = true;
        return false;
    }

    bool skip_backquoted()
    {
        while (!at_end() && peek() != '`') {
            if (peek() == '\\')
                ++m_position;
            ++m_position;
        }
        if (at_end()) {
            m_incomplete = true;
            return false;
        }
        ++m_position;
        return true;
    }

    // Past the opening quote; ends past the closing one.
    bool skip_double_quoted()
    {
        while (!at_end() && peek() != '"') {
            char c = peek();
            if (c == '\\') {
                m_position += 2;
                continue;
            }
            if (c == '$' && peek(1) == '(') {
                m_position += 2;
                if (!skip_balanced('(', ')'))
                    return false;
                continue;
            }
            if (c == '$' && peek(1) == '{') {
                m_position += 2;
                if (!skip_balanced('{', '}'))
                    return false;
                continue;
            }
            if (c == '`') {
                ++m_position;
                if (!skip_backquoted())
                    return false;
                continue;
            }
            ++m_position;
        }
        if (at_end()) {
            m_incomplete = true;
            return false;
        }
        ++m_position;
        return true;
    }

    bool lex_word(Token& token)
    {
        size_t start = m_position;
        while (!at_end()) {
            char c = peek();
            if (is_blank(c) || c == '\n' || is_operator_start(c))
                break;
            if (c == '\\') {
                if (m_position + 1 >= m_source.length()) {
                    ++m_position;
                    break;
                }
                m_position += 2;
                continue;
            }
            if (c == '\'') {
                ++m_position;
                while (!at_end() && peek() != '\'')
                    ++m_position;
                if (at_end()) {
                    m_incomplete = true;
                    return false;
                }
                ++m_position;
                continue;
            }
            if (c == '"') {
                ++m_position;
                if (!skip_double_quoted())
                    return false;
                continue;
            }
            if (c == '`') {
                ++m_position;
                if (!skip_backquoted())
                    return false;
                continue;
            }
            if (c == '$' && peek(1) == '(') {
                m_position += 2;
                if (!skip_balanced('(', ')'))
                    return false;
                continue;
            }
            if (c == '$' && peek(1) == '{') {
                m_position += 2;
                if (!skip_balanced('{', '}'))
                    return false;
                continue;
            }
            ++m_position;
        }
        token.type = Token::Type::Word;
        token.text = MUST(String::from_view(m_source.substring_view(start, m_position - start)));
        token.end = m_position;
        return true;
    }

    // The lines after the newline just read, up to each pending delimiter.
    bool read_here_documents(Vector<Token>& tokens)
    {
        for (size_t index : m_pending) {
            auto& delimiter_token = tokens[index];
            // The delimiter, without quotes; quoting makes the body literal.
            StringBuilder delimiter;
            bool quoted = false;
            for (char c : delimiter_token.text.view()) {
                if (c == '\'' || c == '"' || c == '\\') {
                    quoted = true;
                    continue;
                }
                delimiter.append(c);
            }
            StringBuilder body;
            bool found = false;
            while (!at_end()) {
                size_t line_start = m_position;
                while (!at_end() && peek() != '\n')
                    ++m_position;
                StringView line = m_source.substring_view(line_start, m_position - line_start);
                if (!at_end())
                    ++m_position;
                if (delimiter_token.strip_tabs) {
                    while (!line.is_empty() && line[0] == '\t')
                        line = line.substring_view(1);
                }
                if (line == delimiter.view()) {
                    found = true;
                    break;
                }
                body.append(line);
                body.append('\n');
            }
            if (!found) {
                m_incomplete = true;
                return false;
            }
            delimiter_token.here_document = MUST(body.to_string());
            delimiter_token.here_document_quoted = quoted;
        }
        m_pending.clear();
        return true;
    }

    StringView m_source;
    size_t m_position { 0 };
    Vector<size_t> m_pending;
    bool m_expect_delimiter { false };
    bool m_strip_tabs { false };
    bool m_incomplete { false };
    StringView m_error;
};

bool is_reserved(StringView word)
{
    static StringView const words[] = { "if"_sv, "then"_sv, "else"_sv, "elif"_sv, "fi"_sv, "do"_sv, "done"_sv, "while"_sv, "until"_sv, "for"_sv,
        "in"_sv, "case"_sv, "esac"_sv, "{"_sv, "}"_sv, "!"_sv, "function"_sv };
    for (auto reserved : words) {
        if (word == reserved)
            return true;
    }
    return false;
}

bool is_name(StringView text)
{
    if (text.is_empty() || Base::is_ascii_digit(static_cast<u8>(text[0])))
        return false;
    for (char c : text) {
        if (!Base::is_ascii_alphanumeric(static_cast<u8>(c)) && c != '_')
            return false;
    }
    return true;
}

bool is_assignment(StringView word)
{
    auto equals = word.find('=');
    return equals.has_value() && equals.value() > 0 && is_name(word.substring_view(0, equals.value()));
}

class Parser {
public:
    Parser(StringView source, Vector<Token>&& tokens)
        : m_source(source)
        , m_tokens(move(tokens))
    {
    }

    ParseResult run()
    {
        ParseResult result;
        StringView const terminators[] = { {} };
        auto program = parse_list(Span<StringView const>(terminators, 0));
        if ((m_error.length() != 0) || peek().type != Token::Type::End) {
            result.incomplete = m_incomplete;
            if ((m_error.length() == 0) && peek().type != Token::Type::End)
                m_error = describe_unexpected();
            result.error = MUST(String::from_view(m_error.view()));
            return result;
        }
        result.program = move(program);
        return result;
    }

private:
    Token const& peek(size_t ahead = 0) const
    {
        size_t index = min(m_index + ahead, m_tokens.size() - 1);
        return m_tokens[index];
    }

    Token const& take() { return m_tokens[min(m_index++, m_tokens.size() - 1)]; }

    bool peek_word(StringView word) const { return peek().type == Token::Type::Word && peek().text.view() == word; }
    bool peek_operator(StringView op) const { return peek().type == Token::Type::Operator && peek().text.view() == op; }

    void skip_newlines()
    {
        while (peek().type == Token::Type::Newline)
            ++m_index;
    }

    StringBuilder describe_unexpected() const
    {
        StringBuilder message;
        if (peek().type == Token::Type::End)
            message.append("unexpected end of input"_sv);
        else if (peek().type == Token::Type::Newline)
            message.append("unexpected newline"_sv);
        else
            message.appendff("syntax error near '{}'", peek().text.view());
        return message;
    }

    void fail()
    {
        if ((m_error.length() != 0))
            return;
        m_incomplete = peek().type == Token::Type::End;
        m_error = describe_unexpected();
    }

    bool at_terminator(Span<StringView const> terminators) const
    {
        if (peek().type == Token::Type::End)
            return true;
        if (peek_operator(")"_sv) || peek_operator(";;"_sv))
            return true;
        if (peek().type != Token::Type::Word)
            return false;
        for (auto terminator : terminators) {
            if (peek().text.view() == terminator)
                return true;
        }
        return false;
    }

    NodePointer make(Node::Kind kind)
    {
        auto node = MUST(try_make<Node>());
        node->kind = kind;
        return node;
    }

    void record_text(Node& node, size_t first_token)
    {
        if (first_token >= m_tokens.size() || m_index == 0)
            return;
        size_t start = m_tokens[first_token].start;
        size_t end = m_tokens[min(m_index, m_tokens.size()) - 1].end;
        if (end > start)
            node.text = MUST(String::from_view(m_source.substring_view(start, end - start).trim_whitespace()));
    }

    NodePointer parse_list(Span<StringView const> terminators)
    {
        auto sequence = make(Node::Kind::Sequence);
        size_t first = m_index;
        for (;;) {
            skip_newlines();
            if (at_terminator(terminators) || (m_error.length() != 0))
                break;
            size_t item_start = m_index;
            auto item = parse_and_or();
            if (!item)
                return nullptr;
            if (peek_operator("&"_sv)) {
                take();
                auto background = make(Node::Kind::Background);
                record_text(*item, item_start);
                background->children.append(move(item));
                item = move(background);
            } else if (peek_operator(";"_sv)) {
                take();
            } else if (peek().type == Token::Type::Newline) {
                take();
            } else if (!at_terminator(terminators)) {
                fail();
                return nullptr;
            }
            sequence->children.append(move(item));
        }
        record_text(*sequence, first);
        return sequence;
    }

    NodePointer parse_and_or()
    {
        size_t first = m_index;
        auto left = parse_pipeline();
        while (left && (peek_operator("&&"_sv) || peek_operator("||"_sv))) {
            bool is_and = take().text.view() == "&&"_sv;
            skip_newlines();
            auto right = parse_pipeline();
            if (!right) {
                fail();
                return nullptr;
            }
            auto both = make(is_and ? Node::Kind::And : Node::Kind::Or);
            both->children.append(move(left));
            both->children.append(move(right));
            left = move(both);
        }
        if (left)
            record_text(*left, first);
        return left;
    }

    NodePointer parse_pipeline()
    {
        size_t first = m_index;
        bool negate = false;
        if (peek_word("!"_sv)) {
            take();
            negate = true;
        }
        auto pipeline = make(Node::Kind::Pipeline);
        auto command = parse_command();
        if (!command) {
            fail();
            return nullptr;
        }
        pipeline->children.append(move(command));
        while (peek_operator("|"_sv)) {
            take();
            skip_newlines();
            auto next = parse_command();
            if (!next) {
                fail();
                return nullptr;
            }
            pipeline->children.append(move(next));
        }
        record_text(*pipeline, first);
        if (!negate)
            return pipeline;
        auto inverted = make(Node::Kind::Not);
        inverted->children.append(move(pipeline));
        return inverted;
    }

    bool parse_redirection(Vector<Redirection>& redirections)
    {
        auto const& op = peek();
        if (op.type != Token::Type::Operator)
            return false;
        StringView text = op.text.view();
        Redirection redirection;
        if (text == "<"_sv)
            redirection = { 0, Redirection::Kind::Input, {}, {}, false };
        else if (text == ">"_sv || text == ">|"_sv)
            redirection = { 1, Redirection::Kind::Output, {}, {}, false };
        else if (text == ">>"_sv)
            redirection = { 1, Redirection::Kind::Append, {}, {}, false };
        else if (text == "<>"_sv)
            redirection = { 0, Redirection::Kind::ReadWrite, {}, {}, false };
        else if (text == "<&"_sv)
            redirection = { 0, Redirection::Kind::DuplicateInput, {}, {}, false };
        else if (text == ">&"_sv)
            redirection = { 1, Redirection::Kind::DuplicateOutput, {}, {}, false };
        else if (text == "<<"_sv || text == "<<-"_sv)
            redirection = { 0, Redirection::Kind::HereDocument, {}, {}, false };
        else if (text == "<<<"_sv)
            redirection = { 0, Redirection::Kind::HereString, {}, {}, false };
        else if (text == "&>"_sv)
            redirection = { 1, Redirection::Kind::OutputAndError, {}, {}, false };
        else
            return false;
        if (op.io_number >= 0)
            redirection.fd = op.io_number;
        take();
        if (peek().type != Token::Type::Word) {
            fail();
            return false;
        }
        auto const& target = take();
        redirection.target = target.text;
        if (redirection.kind == Redirection::Kind::HereDocument) {
            redirection.body = target.here_document;
            redirection.literal_body = target.here_document_quoted;
        }
        redirections.append(move(redirection));
        return true;
    }

    void parse_trailing_redirections(Node& node)
    {
        while (peek().type == Token::Type::Operator && (m_error.length() == 0)) {
            if (!parse_redirection(node.redirections))
                break;
        }
    }

    bool expect_word(StringView word)
    {
        skip_newlines();
        if (!peek_word(word)) {
            fail();
            return false;
        }
        take();
        return true;
    }

    NodePointer parse_command()
    {
        size_t first = m_index;
        NodePointer command;
        auto const& token = peek();
        if (token.type == Token::Type::Operator && token.text.view() == "("_sv) {
            take();
            StringView const none[] = { {} };
            auto body = parse_list(Span<StringView const>(none, 0));
            if (!body || !peek_operator(")"_sv)) {
                fail();
                return nullptr;
            }
            take();
            command = make(Node::Kind::Subshell);
            command->children.append(move(body));
        } else if (token.type == Token::Type::Word && token.text.view() == "{"_sv) {
            take();
            StringView const terminators[] = { "}"_sv };
            auto body = parse_list(terminators);
            if (!body || !expect_word("}"_sv))
                return nullptr;
            command = make(Node::Kind::Group);
            command->children.append(move(body));
        } else if (token.type == Token::Type::Word && token.text.view() == "if"_sv) {
            command = parse_if();
        } else if (token.type == Token::Type::Word && (token.text.view() == "while"_sv || token.text.view() == "until"_sv)) {
            command = parse_loop();
        } else if (token.type == Token::Type::Word && token.text.view() == "for"_sv) {
            command = parse_for();
        } else if (token.type == Token::Type::Word && token.text.view() == "case"_sv) {
            command = parse_case();
        } else if (token.type == Token::Type::Word && token.text.view() == "function"_sv) {
            take();
            if (peek().type != Token::Type::Word || !is_name(peek().text.view())) {
                fail();
                return nullptr;
            }
            auto name = take().text;
            if (peek_operator("("_sv) && peek(1).type == Token::Type::Operator && peek(1).text.view() == ")"_sv) {
                take();
                take();
            }
            return parse_function_body(move(name), first);
        } else if (token.type == Token::Type::Word && is_name(token.text.view()) && !is_reserved(token.text.view()) && peek(1).type == Token::Type::Operator
            && peek(1).text.view() == "("_sv && peek(2).type == Token::Type::Operator && peek(2).text.view() == ")"_sv) {
            auto name = take().text;
            take();
            take();
            return parse_function_body(move(name), first);
        } else {
            command = parse_simple();
        }
        if (!command)
            return nullptr;
        if (command->kind != Node::Kind::Simple)
            parse_trailing_redirections(*command);
        record_text(*command, first);
        return command;
    }

    NodePointer parse_function_body(String name, size_t first)
    {
        skip_newlines();
        auto body = parse_command();
        if (!body) {
            fail();
            return nullptr;
        }
        auto function = make(Node::Kind::Function);
        function->name = move(name);
        function->children.append(move(body));
        record_text(*function, first);
        return function;
    }

    NodePointer parse_simple()
    {
        auto command = make(Node::Kind::Simple);
        for (;;) {
            if (peek().type == Token::Type::Operator) {
                if (!parse_redirection(command->redirections))
                    break;
                continue;
            }
            if (peek().type != Token::Type::Word)
                break;
            auto const& word = peek();
            if (command->words.is_empty() && is_assignment(word.text.view())) {
                command->assignments.append(take().text);
                continue;
            }
            command->words.append(take().text);
        }
        if (command->words.is_empty() && command->assignments.is_empty() && command->redirections.is_empty())
            return nullptr;
        return command;
    }

    NodePointer parse_if()
    {
        take(); // if
        auto node = make(Node::Kind::If);
        StringView const condition_end[] = { "then"_sv };
        StringView const body_end[] = { "elif"_sv, "else"_sv, "fi"_sv };
        for (;;) {
            auto condition = parse_list(condition_end);
            if (!condition || !expect_word("then"_sv))
                return nullptr;
            auto body = parse_list(body_end);
            if (!body)
                return nullptr;
            node->children.append(move(condition));
            node->children.append(move(body));
            skip_newlines();
            if (peek_word("elif"_sv)) {
                take();
                continue;
            }
            if (peek_word("else"_sv)) {
                take();
                StringView const else_end[] = { "fi"_sv };
                auto otherwise = parse_list(else_end);
                if (!otherwise)
                    return nullptr;
                node->children.append(move(otherwise));
            }
            if (!expect_word("fi"_sv))
                return nullptr;
            return node;
        }
    }

    NodePointer parse_loop()
    {
        bool is_while = take().text.view() == "while"_sv;
        auto node = make(is_while ? Node::Kind::While : Node::Kind::Until);
        StringView const condition_end[] = { "do"_sv };
        auto condition = parse_list(condition_end);
        if (!condition || !expect_word("do"_sv))
            return nullptr;
        StringView const body_end[] = { "done"_sv };
        auto body = parse_list(body_end);
        if (!body || !expect_word("done"_sv))
            return nullptr;
        node->children.append(move(condition));
        node->children.append(move(body));
        return node;
    }

    NodePointer parse_for()
    {
        take(); // for
        if (peek().type != Token::Type::Word || !is_name(peek().text.view())) {
            fail();
            return nullptr;
        }
        auto node = make(Node::Kind::For);
        node->name = take().text;
        skip_newlines();
        if (peek_word("in"_sv)) {
            take();
            node->has_items = true;
            while (peek().type == Token::Type::Word)
                node->items.append(take().text);
            if (peek_operator(";"_sv) || peek().type == Token::Type::Newline)
                take();
            else {
                fail();
                return nullptr;
            }
        } else if (peek_operator(";"_sv)) {
            take();
        }
        if (!expect_word("do"_sv))
            return nullptr;
        StringView const body_end[] = { "done"_sv };
        auto body = parse_list(body_end);
        if (!body || !expect_word("done"_sv))
            return nullptr;
        node->children.append(move(body));
        return node;
    }

    NodePointer parse_case()
    {
        take(); // case
        if (peek().type != Token::Type::Word) {
            fail();
            return nullptr;
        }
        auto node = make(Node::Kind::Case);
        node->name = take().text;
        if (!expect_word("in"_sv))
            return nullptr;
        for (;;) {
            skip_newlines();
            if (peek_word("esac"_sv)) {
                take();
                return node;
            }
            if (peek_operator("("_sv))
                take();
            Vector<String> patterns;
            for (;;) {
                if (peek().type != Token::Type::Word) {
                    fail();
                    return nullptr;
                }
                patterns.append(take().text);
                if (peek_operator("|"_sv)) {
                    take();
                    continue;
                }
                break;
            }
            if (!peek_operator(")"_sv)) {
                fail();
                return nullptr;
            }
            take();
            StringView const body_end[] = { "esac"_sv };
            auto body = parse_list(body_end);
            if (!body)
                return nullptr;
            node->patterns.append(move(patterns));
            node->children.append(move(body));
            skip_newlines();
            if (peek_operator(";;"_sv)) {
                take();
                continue;
            }
            if (!peek_word("esac"_sv)) {
                fail();
                return nullptr;
            }
        }
    }

    StringView m_source;
    Vector<Token> m_tokens;
    size_t m_index { 0 };
    StringBuilder m_error;
    bool m_incomplete { false };
};

}

ParseResult parse(StringView source)
{
    Lexer lexer(source);
    Vector<Token> tokens;
    if (!lexer.tokenize(tokens)) {
        ParseResult result;
        result.incomplete = lexer.is_incomplete();
        result.error = MUST(String::from_view(lexer.is_incomplete() ? "unexpected end of input"_sv : lexer.error()));
        return result;
    }
    Parser parser(source, move(tokens));
    return parser.run();
}

}
