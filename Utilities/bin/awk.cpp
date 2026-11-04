/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <math.h>
#include <regex.h>
#include <time.h>
#include <unistd.h>

// awk [-F fs] [-v var=value] ['program' | -f file] [file ...]: the POSIX language: patterns and actions, fields, associative
// arrays, functions, regular expressions, printf, getline, and pipes.

extern char** environ;

namespace {

[[noreturn]] void die(char const* format, char const* detail = "")
{
    fprintf(stderr, "awk: ");
    fprintf(stderr, format, detail);
    fprintf(stderr, "\n");
    exit(2);
}

// ---- Values.

// A value is a number, a string, or, for what came from input, a string that looks like a number and counts as both.
struct Value {
    enum class Kind : u8 {
        Uninitialized,
        Number,
        String,
        NumericString,
    };
    Kind kind { Kind::Uninitialized };
    double number { 0 };
    String text;

    static Value from_number(double value)
    {
        Value result;
        result.kind = Kind::Number;
        result.number = value;
        return result;
    }
    static Value from_string(StringView value)
    {
        Value result;
        result.kind = Kind::String;
        result.text = MUST(String::from_view(value));
        return result;
    }
    // What's read from input.
    static Value from_input(StringView value);
};

bool looks_numeric(StringView text, double& number)
{
    size_t start = 0;
    while (start < text.length() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n'))
        ++start;
    size_t end = text.length();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n'))
        --end;
    if (start == end)
        return false;
    auto const trimmed = MUST(String::from_view(text.substring_view(start, end - start)));
    // Numbers in decimal only: strtod's hexadecimal, "inf" and "nan" aren't numeric to awk.
    char const first = trimmed.characters()[0];
    char const* digits = trimmed.characters();
    if (first == '+' || first == '-')
        ++digits;
    if (!(*digits >= '0' && *digits <= '9') && *digits != '.')
        return false;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        return false;
    char* stop = nullptr;
    number = strtod(trimmed.characters(), &stop);
    return stop && *stop == 0;
}

Value Value::from_input(StringView value)
{
    Value result;
    result.text = MUST(String::from_view(value));
    double number = 0;
    if (looks_numeric(value, number)) {
        result.kind = Kind::NumericString;
        result.number = number;
    } else {
        result.kind = Kind::String;
    }
    return result;
}

// (Set when the interpreter starts.)
String g_convfmt;
String g_ofmt;

String number_to_string(double number, StringView format)
{
    char buffer[128];
    // A whole number is just that, however large.
    if (number == floor(number) && fabs(number) < 1e16) {
        snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
    } else {
        auto const owned = MUST(String::from_view(format));
        snprintf(buffer, sizeof(buffer), owned.characters(), number);
    }
    return MUST(String::from_view(StringView(buffer)));
}

String to_string(Value const& value)
{
    switch (value.kind) {
    case Value::Kind::Uninitialized:
        return {};
    case Value::Kind::Number:
        return number_to_string(value.number, g_convfmt.view());
    default:
        return MUST(String::from_view(value.text.view()));
    }
}

double to_number(Value const& value)
{
    switch (value.kind) {
    case Value::Kind::Uninitialized:
        return 0;
    case Value::Kind::Number:
    case Value::Kind::NumericString:
        return value.number;
    case Value::Kind::String:
        return strtod(value.text.characters(), nullptr);
    }
    return 0;
}

bool to_bool(Value const& value)
{
    switch (value.kind) {
    case Value::Kind::Uninitialized:
        return false;
    case Value::Kind::Number:
    case Value::Kind::NumericString:
        return value.number != 0;
    case Value::Kind::String:
        return !value.text.view().is_empty();
    }
    return false;
}

// ---- The language.

enum class Token : u8 {
    End,
    Newline,
    Number,
    String,
    Regex,
    Name,
    FunctionName, // A name directly followed by '(': a call of something defined.
    Builtin,
    // Keywords.
    Begin,
    EndKeyword,
    Function,
    If,
    Else,
    While,
    For,
    Do,
    Break,
    Continue,
    Next,
    Exit,
    Return,
    Delete,
    In,
    Getline,
    Print,
    Printf,
    // Punctuation.
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Semicolon,
    Comma,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Caret,
    Not,
    Greater,
    Less,
    Pipe,
    Question,
    Colon,
    Tilde,
    NoMatch,
    Dollar,
    Assign,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
    ModAssign,
    PowAssign,
    Equal,
    NotEqual,
    LessEqual,
    GreaterEqual,
    And,
    Or,
    Increment,
    Decrement,
    Append,
};

enum class Builtin : u8 {
    Length,
    Substr,
    Index,
    Split,
    Sub,
    Gsub,
    Match,
    Sprintf,
    Sin,
    Cos,
    Atan2,
    Exp,
    Log,
    Sqrt,
    Int,
    Rand,
    Srand,
    Tolower,
    Toupper,
    System,
    Close,
    Fflush,
};

class Lexer {
public:
    explicit Lexer(StringView source)
        : m_source(source)
    {
    }

    Token kind { Token::End };
    String text;
    double number { 0 };
    Builtin builtin { Builtin::Length };
    int line { 1 };

    void next()
    {
        m_last = kind;
        scan();
    }

    // Whether a '/' here starts a regular expression, which is for the parser to say: only it knows what's expected.
    void read_regex()
    {
        StringBuilder pattern;
        while (true) {
            if (m_position >= m_source.length() || m_source[m_position] == '\n')
                die("a regular expression isn't finished, line %s", line_text());
            char const c = m_source[m_position++];
            if (c == '/')
                break;
            if (c == '\\' && m_position < m_source.length()) {
                if (m_source[m_position] == '/') {
                    pattern.append('/');
                    ++m_position;
                    continue;
                }
                pattern.append(c);
                pattern.append(m_source[m_position++]);
                continue;
            }
            pattern.append(c);
        }
        text = MUST(pattern.to_string());
        kind = Token::Regex;
    }

    char const* line_text()
    {
        snprintf(m_line_text, sizeof(m_line_text), "%d", line);
        return m_line_text;
    }

private:
    void scan()
    {
        // Blanks, comments, and a backslash that joins lines.
        for (;;) {
            if (m_position >= m_source.length()) {
                kind = Token::End;
                return;
            }
            char const c = m_source[m_position];
            if (c == ' ' || c == '\t' || c == '\r') {
                ++m_position;
            } else if (c == '\\' && m_position + 1 < m_source.length() && m_source[m_position + 1] == '\n') {
                m_position += 2;
                ++line;
            } else if (c == '#') {
                while (m_position < m_source.length() && m_source[m_position] != '\n')
                    ++m_position;
            } else {
                break;
            }
        }
        char const c = m_source[m_position];
        if (c == '\n') {
            ++m_position;
            ++line;
            kind = Token::Newline;
            return;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && m_position + 1 < m_source.length() && m_source[m_position + 1] >= '0' && m_source[m_position + 1] <= '9')) {
            auto const owned = MUST(String::from_view(m_source.substring_view(m_position)));
            char* stop = nullptr;
            number = strtod(owned.characters(), &stop);
            m_position += static_cast<size_t>(stop - owned.characters());
            kind = Token::Number;
            return;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            size_t const start = m_position;
            while (m_position < m_source.length()) {
                char const d = m_source[m_position];
                if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9') || d == '_'))
                    break;
                ++m_position;
            }
            auto const word = m_source.substring_view(start, m_position - start);
            struct Keyword {
                StringView name;
                Token token;
            };
            static Keyword const keywords[] = {
                { "BEGIN"_sv, Token::Begin }, { "END"_sv, Token::EndKeyword }, { "function"_sv, Token::Function }, { "func"_sv, Token::Function },
                { "if"_sv, Token::If }, { "else"_sv, Token::Else }, { "while"_sv, Token::While }, { "for"_sv, Token::For }, { "do"_sv, Token::Do },
                { "break"_sv, Token::Break }, { "continue"_sv, Token::Continue }, { "next"_sv, Token::Next }, { "exit"_sv, Token::Exit },
                { "return"_sv, Token::Return }, { "delete"_sv, Token::Delete }, { "in"_sv, Token::In }, { "getline"_sv, Token::Getline },
                { "print"_sv, Token::Print }, { "printf"_sv, Token::Printf },
            };
            for (auto const& keyword : keywords) {
                if (keyword.name == word) {
                    kind = keyword.token;
                    return;
                }
            }
            struct BuiltinName {
                StringView name;
                Builtin builtin;
            };
            static BuiltinName const builtins[] = {
                { "length"_sv, Builtin::Length }, { "substr"_sv, Builtin::Substr }, { "index"_sv, Builtin::Index }, { "split"_sv, Builtin::Split },
                { "sub"_sv, Builtin::Sub }, { "gsub"_sv, Builtin::Gsub }, { "match"_sv, Builtin::Match }, { "sprintf"_sv, Builtin::Sprintf },
                { "sin"_sv, Builtin::Sin }, { "cos"_sv, Builtin::Cos }, { "atan2"_sv, Builtin::Atan2 }, { "exp"_sv, Builtin::Exp }, { "log"_sv, Builtin::Log },
                { "sqrt"_sv, Builtin::Sqrt }, { "int"_sv, Builtin::Int }, { "rand"_sv, Builtin::Rand }, { "srand"_sv, Builtin::Srand },
                { "tolower"_sv, Builtin::Tolower }, { "toupper"_sv, Builtin::Toupper }, { "system"_sv, Builtin::System }, { "close"_sv, Builtin::Close },
                { "fflush"_sv, Builtin::Fflush },
            };
            for (auto const& entry : builtins) {
                if (entry.name == word) {
                    builtin = entry.builtin;
                    kind = Token::Builtin;
                    return;
                }
            }
            text = MUST(String::from_view(word));
            kind = m_position < m_source.length() && m_source[m_position] == '(' ? Token::FunctionName : Token::Name;
            return;
        }
        if (c == '"') {
            ++m_position;
            StringBuilder value;
            for (;;) {
                if (m_position >= m_source.length() || m_source[m_position] == '\n')
                    die("a string isn't finished, line %s", line_text());
                char const d = m_source[m_position++];
                if (d == '"')
                    break;
                if (d != '\\' || m_position >= m_source.length()) {
                    value.append(d);
                    continue;
                }
                char const e = m_source[m_position++];
                switch (e) {
                case 'n':
                    value.append('\n');
                    break;
                case 't':
                    value.append('\t');
                    break;
                case 'r':
                    value.append('\r');
                    break;
                case 'a':
                    value.append('\a');
                    break;
                case 'b':
                    value.append('\b');
                    break;
                case 'f':
                    value.append('\f');
                    break;
                case 'v':
                    value.append('\v');
                    break;
                case '\\':
                    value.append('\\');
                    break;
                case '"':
                    value.append('"');
                    break;
                case '/':
                    value.append('/');
                    break;
                case '\n':
                    ++line;
                    break;
                default:
                    if (e >= '0' && e <= '7') {
                        int code = e - '0';
                        for (int digit = 0; digit < 2 && m_position < m_source.length() && m_source[m_position] >= '0' && m_source[m_position] <= '7'; ++digit)
                            code = code * 8 + (m_source[m_position++] - '0');
                        value.append(static_cast<char>(code));
                    } else {
                        // Kept, for what's a regular expression in a string.
                        value.append('\\');
                        value.append(e);
                    }
                    break;
                }
            }
            text = MUST(value.to_string());
            kind = Token::String;
            return;
        }
        ++m_position;
        char const d = m_position < m_source.length() ? m_source[m_position] : 0;
        auto two = [&](Token token) {
            ++m_position;
            kind = token;
        };
        switch (c) {
        case '{':
            kind = Token::LeftBrace;
            return;
        case '}':
            kind = Token::RightBrace;
            return;
        case '(':
            kind = Token::LeftParen;
            return;
        case ')':
            kind = Token::RightParen;
            return;
        case '[':
            kind = Token::LeftBracket;
            return;
        case ']':
            kind = Token::RightBracket;
            return;
        case ';':
            kind = Token::Semicolon;
            return;
        case ',':
            kind = Token::Comma;
            return;
        case '?':
            kind = Token::Question;
            return;
        case ':':
            kind = Token::Colon;
            return;
        case '~':
            kind = Token::Tilde;
            return;
        case '$':
            kind = Token::Dollar;
            return;
        case '+':
            if (d == '+')
                return two(Token::Increment);
            if (d == '=')
                return two(Token::AddAssign);
            kind = Token::Plus;
            return;
        case '-':
            if (d == '-')
                return two(Token::Decrement);
            if (d == '=')
                return two(Token::SubAssign);
            kind = Token::Minus;
            return;
        case '*':
            if (d == '*') {
                ++m_position;
                if (m_position < m_source.length() && m_source[m_position] == '=')
                    return two(Token::PowAssign);
                kind = Token::Caret;
                return;
            }
            if (d == '=')
                return two(Token::MulAssign);
            kind = Token::Star;
            return;
        case '/':
            // A '/=' could be the start of a regular expression instead: the parser takes that back if so.
            if (d == '=')
                return two(Token::DivAssign);
            kind = Token::Slash;
            return;
        case '%':
            if (d == '=')
                return two(Token::ModAssign);
            kind = Token::Percent;
            return;
        case '^':
            if (d == '=')
                return two(Token::PowAssign);
            kind = Token::Caret;
            return;
        case '!':
            if (d == '=')
                return two(Token::NotEqual);
            if (d == '~')
                return two(Token::NoMatch);
            kind = Token::Not;
            return;
        case '>':
            if (d == '=')
                return two(Token::GreaterEqual);
            if (d == '>')
                return two(Token::Append);
            kind = Token::Greater;
            return;
        case '<':
            if (d == '=')
                return two(Token::LessEqual);
            kind = Token::Less;
            return;
        case '=':
            if (d == '=')
                return two(Token::Equal);
            kind = Token::Assign;
            return;
        case '&':
            if (d == '&')
                return two(Token::And);
            break;
        case '|':
            if (d == '|')
                return two(Token::Or);
            kind = Token::Pipe;
            return;
        default:
            break;
        }
        char what[2] = { c, 0 };
        die("this makes no sense here: %s", what);
    }

    StringView m_source;
    size_t m_position { 0 };
    Token m_last { Token::End };
    char m_line_text[16];
};

// ---- What's parsed into.

struct Expression;
using ExpressionPointer = OwnPtr<Expression>;

struct Expression {
    enum class Kind : u8 {
        Number,
        String,
        Regex, // On its own: matches $0.
        Variable,
        Local,
        Field,
        Index, // array[subscripts]
        In, // (subscripts) in array
        Unary,
        Binary,
        Concat,
        And,
        Or,
        Match, // ~ or !~
        Conditional,
        Assign,
        PreIncrement,
        PostIncrement,
        Group,
        Call,
        BuiltinCall,
        Getline,
    };
    Kind kind;
    Token op { Token::End };
    double number { 0 };
    String text;
    int slot { 0 };
    bool is_local_array { false };
    Builtin builtin { Builtin::Length };
    bool negate { false };
    int increment { 1 };
    ExpressionPointer left;
    ExpressionPointer right;
    ExpressionPointer third;
    Vector<ExpressionPointer> arguments;
    // getline's: 0 plain, 1 < file, 2 cmd |
    int getline_kind { 0 };
    regex_t* regex { nullptr };

    explicit Expression(Kind kind_)
        : kind(kind_)
    {
    }
};

struct Statement;
using StatementPointer = OwnPtr<Statement>;

struct Statement {
    enum class Kind : u8 {
        Expression,
        Print,
        Printf,
        If,
        While,
        DoWhile,
        For,
        ForIn,
        Block,
        Next,
        Exit,
        Return,
        Break,
        Continue,
        Delete,
        DeleteAll,
    };
    Kind kind;
    ExpressionPointer expression;
    ExpressionPointer step;
    ExpressionPointer initial;
    StatementPointer body;
    StatementPointer otherwise;
    Vector<StatementPointer> statements;
    Vector<ExpressionPointer> arguments;
    // print's output: 0 stdout, or Token::Greater, Append or Pipe to `expression`.
    Token redirect { Token::End };
    // for (name in array)
    ExpressionPointer variable;
    ExpressionPointer array;

    explicit Statement(Kind kind_)
        : kind(kind_)
    {
    }
};

struct Rule {
    ExpressionPointer pattern;
    ExpressionPointer range_end;
    bool in_range { false };
    StatementPointer action;
};

struct FunctionDefinition {
    String name;
    Vector<String> parameters;
    StatementPointer body;
};

// Where things are at run time.
struct Cell {
    Value value;
    HashMap<String, Cell>* array { nullptr };
};

class Program {
public:
    Vector<OwnPtr<Statement>> begin_actions;
    Vector<OwnPtr<Statement>> end_actions;
    Vector<OwnPtr<Rule>> rules;
    Vector<OwnPtr<FunctionDefinition>> functions;
    Vector<String> global_names;

    int global_slot(StringView name)
    {
        for (size_t i = 0; i < global_names.size(); ++i) {
            if (global_names[i].view() == name)
                return static_cast<int>(i);
        }
        global_names.append(MUST(String::from_view(name)));
        return static_cast<int>(global_names.size() - 1);
    }

    int function_index(StringView name) const
    {
        for (size_t i = 0; i < functions.size(); ++i) {
            if (functions[i]->name.view() == name)
                return static_cast<int>(i);
        }
        return -1;
    }
};

// ---- The parser: recursive descent, one level for each precedence.

class Parser {
public:
    Parser(StringView source, Program& program)
        : m_lexer(source)
        , m_program(program)
    {
        m_lexer.next();
    }

    void parse_program()
    {
        skip_terminators();
        while (m_lexer.kind != Token::End) {
            parse_item();
            skip_terminators();
        }
    }

private:
    void expect(Token token, char const* what)
    {
        if (m_lexer.kind != token)
            die("%s was expected here", what);
        m_lexer.next();
    }

    void skip_newlines()
    {
        while (m_lexer.kind == Token::Newline)
            m_lexer.next();
    }

    void skip_terminators()
    {
        while (m_lexer.kind == Token::Newline || m_lexer.kind == Token::Semicolon)
            m_lexer.next();
    }

    void parse_item()
    {
        if (m_lexer.kind == Token::Function) {
            parse_function();
            return;
        }
        if (m_lexer.kind == Token::Begin) {
            m_lexer.next();
            skip_newlines();
            m_program.begin_actions.append(parse_block());
            return;
        }
        if (m_lexer.kind == Token::EndKeyword) {
            m_lexer.next();
            skip_newlines();
            m_program.end_actions.append(parse_block());
            return;
        }
        auto rule = adopt_own(*new Rule);
        if (m_lexer.kind != Token::LeftBrace) {
            rule->pattern = parse_expression();
            if (m_lexer.kind == Token::Comma) {
                m_lexer.next();
                skip_newlines();
                rule->range_end = parse_expression();
            }
        }
        if (m_lexer.kind == Token::LeftBrace)
            rule->action = parse_block();
        m_program.rules.append(move(rule));
    }

    void parse_function()
    {
        m_lexer.next();
        if (m_lexer.kind != Token::Name && m_lexer.kind != Token::FunctionName)
            die("a function needs a name");
        auto definition = adopt_own(*new FunctionDefinition);
        definition->name = MUST(String::from_view(m_lexer.text.view()));
        m_lexer.next();
        expect(Token::LeftParen, "(");
        while (m_lexer.kind != Token::RightParen) {
            if (m_lexer.kind != Token::Name)
                die("a function's parameters are names");
            definition->parameters.append(MUST(String::from_view(m_lexer.text.view())));
            m_lexer.next();
            if (m_lexer.kind == Token::Comma) {
                m_lexer.next();
                skip_newlines();
            }
        }
        m_lexer.next();
        skip_newlines();
        m_locals = &definition->parameters;
        definition->body = parse_block();
        m_locals = nullptr;
        m_program.functions.append(move(definition));
    }

    StatementPointer parse_block()
    {
        expect(Token::LeftBrace, "{");
        auto block = adopt_own(*new Statement(Statement::Kind::Block));
        skip_terminators();
        while (m_lexer.kind != Token::RightBrace) {
            if (m_lexer.kind == Token::End)
                die("a { isn't closed");
            block->statements.append(parse_statement());
            skip_terminators();
        }
        m_lexer.next();
        return block;
    }

    void end_of_simple_statement()
    {
        if (m_lexer.kind == Token::Semicolon || m_lexer.kind == Token::Newline) {
            m_lexer.next();
            return;
        }
        if (m_lexer.kind != Token::RightBrace && m_lexer.kind != Token::End)
            die("something else was expected at the end of a statement, line %s", m_lexer.line_text());
    }

    StatementPointer parse_statement_or_empty()
    {
        skip_newlines();
        if (m_lexer.kind == Token::Semicolon) {
            m_lexer.next();
            return adopt_own(*new Statement(Statement::Kind::Block));
        }
        return parse_statement();
    }

    StatementPointer parse_statement()
    {
        switch (m_lexer.kind) {
        case Token::LeftBrace:
            return parse_block();
        case Token::If: {
            m_lexer.next();
            expect(Token::LeftParen, "(");
            auto statement = adopt_own(*new Statement(Statement::Kind::If));
            statement->expression = parse_expression();
            expect(Token::RightParen, ")");
            statement->body = parse_statement_or_empty();
            // An "else" may come after newlines and a semicolon.
            skip_terminators();
            if (m_lexer.kind == Token::Else) {
                m_lexer.next();
                statement->otherwise = parse_statement_or_empty();
            }
            return statement;
        }
        case Token::While: {
            m_lexer.next();
            expect(Token::LeftParen, "(");
            auto statement = adopt_own(*new Statement(Statement::Kind::While));
            statement->expression = parse_expression();
            expect(Token::RightParen, ")");
            statement->body = parse_statement_or_empty();
            return statement;
        }
        case Token::Do: {
            m_lexer.next();
            auto statement = adopt_own(*new Statement(Statement::Kind::DoWhile));
            statement->body = parse_statement_or_empty();
            skip_terminators();
            if (m_lexer.kind != Token::While)
                die("do needs a while");
            m_lexer.next();
            expect(Token::LeftParen, "(");
            statement->expression = parse_expression();
            expect(Token::RightParen, ")");
            end_of_simple_statement();
            return statement;
        }
        case Token::For:
            return parse_for();
        default:
            break;
        }
        auto statement = parse_simple_statement();
        end_of_simple_statement();
        return statement;
    }

    StatementPointer parse_for()
    {
        m_lexer.next();
        expect(Token::LeftParen, "(");
        ExpressionPointer initial;
        if (m_lexer.kind != Token::Semicolon) {
            initial = parse_expression_no_in_check();
            // for (name in array)
            if (m_lexer.kind == Token::RightParen && initial->kind == Expression::Kind::In && initial->arguments.size() == 1
                && (initial->arguments[0]->kind == Expression::Kind::Variable || initial->arguments[0]->kind == Expression::Kind::Local)) {
                m_lexer.next();
                auto statement = adopt_own(*new Statement(Statement::Kind::ForIn));
                statement->variable = move(initial->arguments[0]);
                statement->array = move(initial->left);
                statement->body = parse_statement_or_empty();
                return statement;
            }
        }
        expect(Token::Semicolon, ";");
        skip_newlines();
        auto statement = adopt_own(*new Statement(Statement::Kind::For));
        statement->initial = move(initial);
        if (m_lexer.kind != Token::Semicolon)
            statement->expression = parse_expression();
        expect(Token::Semicolon, ";");
        skip_newlines();
        if (m_lexer.kind != Token::RightParen)
            statement->step = parse_expression();
        expect(Token::RightParen, ")");
        statement->body = parse_statement_or_empty();
        return statement;
    }

    StatementPointer parse_simple_statement()
    {
        switch (m_lexer.kind) {
        case Token::Print:
        case Token::Printf:
            return parse_print();
        case Token::Next:
            m_lexer.next();
            return adopt_own(*new Statement(Statement::Kind::Next));
        case Token::Break:
            m_lexer.next();
            return adopt_own(*new Statement(Statement::Kind::Break));
        case Token::Continue:
            m_lexer.next();
            return adopt_own(*new Statement(Statement::Kind::Continue));
        case Token::Exit:
        case Token::Return: {
            bool const is_exit = m_lexer.kind == Token::Exit;
            m_lexer.next();
            auto statement = adopt_own(*new Statement(is_exit ? Statement::Kind::Exit : Statement::Kind::Return));
            if (m_lexer.kind != Token::Semicolon && m_lexer.kind != Token::Newline && m_lexer.kind != Token::RightBrace && m_lexer.kind != Token::End)
                statement->expression = parse_expression();
            return statement;
        }
        case Token::Delete: {
            m_lexer.next();
            if (m_lexer.kind != Token::Name && m_lexer.kind != Token::FunctionName)
                die("delete needs an array");
            auto name = MUST(String::from_view(m_lexer.text.view()));
            m_lexer.next();
            auto array = variable_named(name.view());
            if (m_lexer.kind == Token::LeftBracket) {
                m_lexer.next();
                auto statement = adopt_own(*new Statement(Statement::Kind::Delete));
                statement->array = move(array);
                parse_subscripts(statement->arguments);
                return statement;
            }
            auto statement = adopt_own(*new Statement(Statement::Kind::DeleteAll));
            statement->array = move(array);
            return statement;
        }
        default: {
            auto statement = adopt_own(*new Statement(Statement::Kind::Expression));
            statement->expression = parse_expression();
            return statement;
        }
        }
    }

    StatementPointer parse_print()
    {
        bool const is_printf = m_lexer.kind == Token::Printf;
        m_lexer.next();
        auto statement = adopt_own(*new Statement(is_printf ? Statement::Kind::Printf : Statement::Kind::Print));
        // ">" means output here, not "greater than": unless it's inside parentheses.
        ++m_no_greater;
        bool const parenthesized = m_lexer.kind == Token::LeftParen;
        if (m_lexer.kind != Token::Semicolon && m_lexer.kind != Token::Newline && m_lexer.kind != Token::RightBrace && m_lexer.kind != Token::Greater
            && m_lexer.kind != Token::Append && m_lexer.kind != Token::Pipe && m_lexer.kind != Token::End) {
            auto first = parse_expression();
            // print (a, b) > file: a group, seen as (a, b) only with something after it, handled by parse_primary.
            if (parenthesized && first->kind == Expression::Kind::Group) {
                for (auto& argument : first->arguments)
                    statement->arguments.append(move(argument));
            } else {
                statement->arguments.append(move(first));
            }
            while (m_lexer.kind == Token::Comma) {
                m_lexer.next();
                skip_newlines();
                statement->arguments.append(parse_expression());
            }
        }
        --m_no_greater;
        if (m_lexer.kind == Token::Greater || m_lexer.kind == Token::Append || m_lexer.kind == Token::Pipe) {
            statement->redirect = m_lexer.kind;
            m_lexer.next();
            ++m_no_greater;
            ++m_no_in;
            statement->expression = parse_concat(true);
            --m_no_in;
            --m_no_greater;
        }
        return statement;
    }

    void parse_subscripts(Vector<ExpressionPointer>& into)
    {
        ++m_grouped;
        int const saved_greater = m_no_greater;
        m_no_greater = 0;
        into.append(parse_expression());
        while (m_lexer.kind == Token::Comma) {
            m_lexer.next();
            skip_newlines();
            into.append(parse_expression());
        }
        m_no_greater = saved_greater;
        --m_grouped;
        expect(Token::RightBracket, "]");
    }

    ExpressionPointer variable_named(StringView name)
    {
        if (m_locals) {
            for (size_t i = 0; i < m_locals->size(); ++i) {
                if ((*m_locals)[i].view() == name) {
                    auto local = adopt_own(*new Expression(Expression::Kind::Local));
                    local->slot = static_cast<int>(i);
                    return local;
                }
            }
        }
        auto variable = adopt_own(*new Expression(Expression::Kind::Variable));
        variable->slot = m_program.global_slot(name);
        return variable;
    }

    ExpressionPointer parse_expression_no_in_check()
    {
        return parse_expression();
    }

    ExpressionPointer parse_expression()
    {
        return parse_ternary();
    }

    static bool is_lvalue(Expression const& expression)
    {
        return expression.kind == Expression::Kind::Variable || expression.kind == Expression::Kind::Local || expression.kind == Expression::Kind::Field || expression.kind == Expression::Kind::Index;
    }

    ExpressionPointer parse_ternary()
    {
        auto condition = parse_or();
        if (m_lexer.kind == Token::Question) {
            m_lexer.next();
            skip_newlines();
            auto conditional = adopt_own(*new Expression(Expression::Kind::Conditional));
            conditional->left = move(condition);
            conditional->right = parse_ternary();
            skip_newlines();
            expect(Token::Colon, ":");
            skip_newlines();
            conditional->third = parse_ternary();
            return conditional;
        }
        if (is_lvalue(*condition)) {
            switch (m_lexer.kind) {
            case Token::Assign:
            case Token::AddAssign:
            case Token::SubAssign:
            case Token::MulAssign:
            case Token::DivAssign:
            case Token::ModAssign:
            case Token::PowAssign: {
                auto assign = adopt_own(*new Expression(Expression::Kind::Assign));
                assign->op = m_lexer.kind;
                m_lexer.next();
                skip_newlines();
                assign->left = move(condition);
                assign->right = parse_ternary();
                return assign;
            }
            default:
                break;
            }
        }
        return condition;
    }

    ExpressionPointer parse_or()
    {
        auto left = parse_and();
        while (m_lexer.kind == Token::Or) {
            m_lexer.next();
            skip_newlines();
            auto node = adopt_own(*new Expression(Expression::Kind::Or));
            node->left = move(left);
            node->right = parse_and();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_and()
    {
        auto left = parse_in();
        while (m_lexer.kind == Token::And) {
            m_lexer.next();
            skip_newlines();
            auto node = adopt_own(*new Expression(Expression::Kind::And));
            node->left = move(left);
            node->right = parse_in();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_in()
    {
        auto left = parse_match();
        while (m_lexer.kind == Token::In && !m_no_in) {
            m_lexer.next();
            if (m_lexer.kind != Token::Name && m_lexer.kind != Token::FunctionName)
                die("in needs an array");
            auto node = adopt_own(*new Expression(Expression::Kind::In));
            if (left->kind == Expression::Kind::Group) {
                for (auto& subscript : left->arguments)
                    node->arguments.append(move(subscript));
            } else {
                node->arguments.append(move(left));
            }
            node->left = variable_named(m_lexer.text.view());
            m_lexer.next();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_match()
    {
        auto left = parse_comparison();
        while (m_lexer.kind == Token::Tilde || m_lexer.kind == Token::NoMatch) {
            bool const negate = m_lexer.kind == Token::NoMatch;
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Match));
            node->negate = negate;
            node->left = move(left);
            node->right = parse_comparison();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_comparison()
    {
        auto left = parse_concat(false);
        for (;;) {
            Token const token = m_lexer.kind;
            bool const is_comparison = token == Token::Less || token == Token::LessEqual || token == Token::Equal || token == Token::NotEqual || token == Token::GreaterEqual || (token == Token::Greater && !m_no_greater);
            if (is_comparison) {
                m_lexer.next();
                auto node = adopt_own(*new Expression(Expression::Kind::Binary));
                node->op = token;
                node->left = move(left);
                node->right = parse_concat(false);
                left = move(node);
                continue;
            }
            // cmd | getline
            if (token == Token::Pipe && !m_no_greater) {
                m_lexer.next();
                if (m_lexer.kind != Token::Getline)
                    die("a | here should be followed by getline");
                m_lexer.next();
                auto node = adopt_own(*new Expression(Expression::Kind::Getline));
                node->getline_kind = 2;
                node->left = move(left);
                if (starts_lvalue())
                    node->right = parse_primary_postfix();
                left = move(node);
                continue;
            }
            return left;
        }
    }

    bool starts_lvalue() const
    {
        return m_lexer.kind == Token::Name || m_lexer.kind == Token::Dollar;
    }

    bool starts_concatenated_value() const
    {
        switch (m_lexer.kind) {
        case Token::Number:
        case Token::String:
        case Token::Name:
        case Token::FunctionName:
        case Token::Builtin:
        case Token::Dollar:
        case Token::Not:
        case Token::LeftParen:
        case Token::Minus:
        case Token::Plus:
        case Token::Increment:
        case Token::Decrement:
            return true;
        default:
            return false;
        }
    }

    ExpressionPointer parse_concat(bool only_one_level)
    {
        (void)only_one_level;
        auto left = parse_additive();
        while (starts_concatenated_value()) {
            // "a -1" is subtraction, handled below: only what's left over gets here.
            if (m_lexer.kind == Token::In)
                break;
            auto node = adopt_own(*new Expression(Expression::Kind::Concat));
            node->left = move(left);
            node->right = parse_additive();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_additive()
    {
        auto left = parse_multiplicative();
        while (m_lexer.kind == Token::Plus || m_lexer.kind == Token::Minus) {
            Token const token = m_lexer.kind;
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Binary));
            node->op = token;
            node->left = move(left);
            node->right = parse_multiplicative();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_multiplicative()
    {
        auto left = parse_unary();
        while (m_lexer.kind == Token::Star || m_lexer.kind == Token::Slash || m_lexer.kind == Token::Percent) {
            Token const token = m_lexer.kind;
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Binary));
            node->op = token;
            node->left = move(left);
            node->right = parse_unary();
            left = move(node);
        }
        return left;
    }

    ExpressionPointer parse_unary()
    {
        if (m_lexer.kind == Token::Not || m_lexer.kind == Token::Minus || m_lexer.kind == Token::Plus) {
            Token const token = m_lexer.kind;
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Unary));
            node->op = token;
            node->left = parse_unary();
            return node;
        }
        return parse_power();
    }

    ExpressionPointer parse_power()
    {
        auto base = parse_postfix();
        if (m_lexer.kind == Token::Caret) {
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Binary));
            node->op = Token::Caret;
            node->left = move(base);
            // Right to left, and it binds tighter than a minus on its right.
            node->right = parse_unary();
            return node;
        }
        return base;
    }

    ExpressionPointer parse_postfix()
    {
        auto value = parse_primary_postfix();
        if (is_lvalue(*value) && (m_lexer.kind == Token::Increment || m_lexer.kind == Token::Decrement)) {
            auto node = adopt_own(*new Expression(Expression::Kind::PostIncrement));
            node->increment = m_lexer.kind == Token::Increment ? 1 : -1;
            m_lexer.next();
            node->left = move(value);
            return node;
        }
        return value;
    }

    ExpressionPointer parse_primary_postfix()
    {
        switch (m_lexer.kind) {
        case Token::Number: {
            auto node = adopt_own(*new Expression(Expression::Kind::Number));
            node->number = m_lexer.number;
            m_lexer.next();
            return node;
        }
        case Token::String: {
            auto node = adopt_own(*new Expression(Expression::Kind::String));
            node->text = MUST(String::from_view(m_lexer.text.view()));
            m_lexer.next();
            return node;
        }
        case Token::DivAssign:
        case Token::Slash: {
            // Division can't start a value, so this is a regular expression. (Starting "=", if that's how it scanned.)
            bool const with_equals = m_lexer.kind == Token::DivAssign;
            m_lexer.read_regex();
            auto node = adopt_own(*new Expression(Expression::Kind::Regex));
            if (with_equals) {
                StringBuilder pattern;
                pattern.append('=');
                pattern.append(m_lexer.text.view());
                node->text = MUST(pattern.to_string());
            } else {
                node->text = MUST(String::from_view(m_lexer.text.view()));
            }
            m_lexer.next();
            return node;
        }
        case Token::Dollar: {
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Field));
            if (m_lexer.kind == Token::Increment || m_lexer.kind == Token::Decrement || m_lexer.kind == Token::Minus)
                node->left = parse_unary_for_field();
            else
                node->left = parse_primary_postfix();
            return node;
        }
        case Token::Increment:
        case Token::Decrement: {
            int const increment = m_lexer.kind == Token::Increment ? 1 : -1;
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::PreIncrement));
            node->increment = increment;
            node->left = parse_primary_postfix();
            if (!is_lvalue(*node->left))
                die("++ and -- need something to change");
            return node;
        }
        case Token::LeftParen: {
            m_lexer.next();
            int const saved_greater = m_no_greater;
            int const saved_in = m_no_in;
            m_no_greater = 0;
            m_no_in = 0;
            auto first = parse_expression();
            auto group = adopt_own(*new Expression(Expression::Kind::Group));
            group->arguments.append(move(first));
            while (m_lexer.kind == Token::Comma) {
                m_lexer.next();
                skip_newlines();
                group->arguments.append(parse_expression());
            }
            m_no_greater = saved_greater;
            m_no_in = saved_in;
            expect(Token::RightParen, ")");
            return group;
        }
        case Token::Getline: {
            m_lexer.next();
            auto node = adopt_own(*new Expression(Expression::Kind::Getline));
            if (starts_lvalue())
                node->right = parse_primary_postfix();
            if (m_lexer.kind == Token::Less) {
                m_lexer.next();
                node->getline_kind = 1;
                node->left = parse_primary_postfix();
            }
            return node;
        }
        case Token::Builtin: {
            auto node = adopt_own(*new Expression(Expression::Kind::BuiltinCall));
            node->builtin = m_lexer.builtin;
            m_lexer.next();
            if (m_lexer.kind == Token::LeftParen) {
                m_lexer.next();
                parse_call_arguments(node->arguments);
            } else if (node->builtin != Builtin::Length) {
                die("this function needs its parentheses");
            }
            return node;
        }
        case Token::FunctionName: {
            auto node = adopt_own(*new Expression(Expression::Kind::Call));
            node->text = MUST(String::from_view(m_lexer.text.view()));
            m_lexer.next();
            m_lexer.next();
            parse_call_arguments(node->arguments);
            return node;
        }
        case Token::Name: {
            auto name = MUST(String::from_view(m_lexer.text.view()));
            m_lexer.next();
            auto variable = variable_named(name.view());
            if (m_lexer.kind == Token::LeftBracket) {
                m_lexer.next();
                auto node = adopt_own(*new Expression(Expression::Kind::Index));
                node->left = move(variable);
                parse_subscripts(node->arguments);
                return node;
            }
            return variable;
        }
        case Token::Not:
        case Token::Minus:
        case Token::Plus:
            return parse_unary();
        default:
            die("something makes no sense here, line %s", m_lexer.line_text());
        }
    }

    ExpressionPointer parse_unary_for_field()
    {
        return parse_unary();
    }

    void parse_call_arguments(Vector<ExpressionPointer>& into)
    {
        int const saved_greater = m_no_greater;
        int const saved_in = m_no_in;
        m_no_greater = 0;
        m_no_in = 0;
        skip_newlines();
        while (m_lexer.kind != Token::RightParen) {
            into.append(parse_expression());
            skip_newlines();
            if (m_lexer.kind == Token::Comma) {
                m_lexer.next();
                skip_newlines();
            } else if (m_lexer.kind != Token::RightParen) {
                die("a function call's arguments aren't right, line %s", m_lexer.line_text());
            }
        }
        m_no_greater = saved_greater;
        m_no_in = saved_in;
        m_lexer.next();
    }

    Lexer m_lexer;
    Program& m_program;
    Vector<String>* m_locals { nullptr };
    int m_no_greater { 0 };
    int m_no_in { 0 };
    int m_grouped { 0 };
};

// ---- Running it.

using Array = HashMap<String, Cell>;

struct LocalCell {
    Value value;
    Array* array { nullptr };
    // If it's been passed along, this is who owns it.
    LocalCell* alias { nullptr };
    Cell* global_alias { nullptr };
    bool owns_array { false };
};

enum class Flow : u8 {
    Normal,
    Break,
    Continue,
    Next,
    Exit,
    Return,
};

class Interpreter {
public:
    explicit Interpreter(Program& program)
        : m_program(program)
    {
        MUST(m_globals.try_resize(program.global_names.size()));
        m_slot_nf = program.global_slot("NF"_sv);
        m_slot_nr = program.global_slot("NR"_sv);
        m_slot_fnr = program.global_slot("FNR"_sv);
        m_slot_fs = program.global_slot("FS"_sv);
        m_slot_ofs = program.global_slot("OFS"_sv);
        m_slot_ors = program.global_slot("ORS"_sv);
        m_slot_rs = program.global_slot("RS"_sv);
        m_slot_filename = program.global_slot("FILENAME"_sv);
        m_slot_subsep = program.global_slot("SUBSEP"_sv);
        m_slot_rstart = program.global_slot("RSTART"_sv);
        m_slot_rlength = program.global_slot("RLENGTH"_sv);
        m_slot_ofmt = program.global_slot("OFMT"_sv);
        m_slot_convfmt = program.global_slot("CONVFMT"_sv);
        m_slot_environ = program.global_slot("ENVIRON"_sv);
        m_slot_argc = program.global_slot("ARGC"_sv);
        m_slot_argv = program.global_slot("ARGV"_sv);
        MUST(m_globals.try_resize(program.global_names.size()));
        set_global(m_slot_fs, Value::from_string(" "_sv));
        set_global(m_slot_ofs, Value::from_string(" "_sv));
        set_global(m_slot_ors, Value::from_string("\n"_sv));
        set_global(m_slot_rs, Value::from_string("\n"_sv));
        set_global(m_slot_subsep, Value::from_string("\034"_sv));
        set_global(m_slot_ofmt, Value::from_string("%.6g"_sv));
        set_global(m_slot_convfmt, Value::from_string("%.6g"_sv));
        auto& environment = array_of_global(m_slot_environ);
        for (char** entry = environ; entry && *entry; ++entry) {
            StringView const text { *entry };
            auto const equals = text.find('=');
            if (!equals.has_value())
                continue;
            Cell cell;
            cell.value = Value::from_input(text.substring_view(equals.value() + 1));
            MUST(environment.try_set(MUST(String::from_view(text.substring_view(0, equals.value()))), move(cell)));
        }
    }

    void set_arguments(int argc, char** argv)
    {
        auto& arguments = array_of_global(m_slot_argv);
        for (int i = 0; i < argc; ++i) {
            Cell cell;
            cell.value = Value::from_input(StringView(argv[i]));
            MUST(arguments.try_set(number_to_string(i, "%.6g"_sv), move(cell)));
        }
        set_global(m_slot_argc, Value::from_number(argc));
    }

    void assign_from_command_line(StringView assignment)
    {
        auto const equals = assignment.find('=');
        auto const name = assignment.substring_view(0, equals.value());
        int const slot = m_program.global_slot(name);
        MUST(m_globals.try_resize(m_program.global_names.size()));
        auto const processed = process_escapes(assignment.substring_view(equals.value() + 1));
        set_global(slot, Value::from_input(processed.view()));
    }

    int run()
    {
        for (auto& action : m_program.begin_actions) {
            if (execute(*action) == Flow::Exit)
                return finish();
        }
        if (!m_program.rules.is_empty() || !m_program.end_actions.is_empty()) {
            m_input_index = 1;
            while (read_main_record()) {
                bool stop = false;
                for (auto& rule : m_program.rules) {
                    if (!rule_matches(*rule))
                        continue;
                    if (!rule->action) {
                        print_record();
                        continue;
                    }
                    Flow const flow = execute(*rule->action);
                    if (flow == Flow::Next)
                        break;
                    if (flow == Flow::Exit) {
                        stop = true;
                        break;
                    }
                }
                if (stop)
                    return finish();
            }
        }
        return finish();
    }

private:
    int finish()
    {
        if (!m_exiting_from_end) {
            m_exiting_from_end = true;
            for (auto& action : m_program.end_actions) {
                if (execute(*action) == Flow::Exit)
                    break;
            }
        }
        fflush(stdout);
        for (auto& stream : m_streams) {
            if (stream.is_pipe)
                pclose(stream.file);
            else
                fclose(stream.file);
        }
        return m_exit_status;
    }

    bool rule_matches(Rule& rule)
    {
        if (!rule.pattern)
            return true;
        if (!rule.range_end)
            return to_bool(evaluate(*rule.pattern));
        if (rule.in_range) {
            if (to_bool(evaluate(*rule.range_end)))
                rule.in_range = false;
            return true;
        }
        if (to_bool(evaluate(*rule.pattern))) {
            rule.in_range = !to_bool(evaluate(*rule.range_end));
            return true;
        }
        return false;
    }

    // ---- Input.

    FILE* m_main_input { nullptr };
    long m_input_index { 0 };
    bool m_used_any_file { false };

    bool open_next_main_input()
    {
        for (;;) {
            long const count = static_cast<long>(to_number(global(m_slot_argc)));
            if (m_input_index >= count) {
                if (m_used_any_file)
                    return false;
                m_used_any_file = true;
                m_main_input = stdin;
                return true;
            }
            auto& arguments = array_of_global(m_slot_argv);
            auto const key = number_to_string(static_cast<double>(m_input_index++), "%.6g"_sv);
            auto const* cell = arguments.get(key);
            if (!cell)
                continue;
            auto const argument = to_string(cell->value);
            if (argument.view().is_empty())
                continue;
            // var=value on the command line, at this point in the files.
            auto const equals = argument.view().find('=');
            if (equals.has_value() && is_name(argument.view().substring_view(0, equals.value()))) {
                assign_from_command_line(argument.view());
                continue;
            }
            m_used_any_file = true;
            set_global(m_slot_filename, Value::from_string(argument.view()));
            set_global(m_slot_fnr, Value::from_number(0));
            if (argument.view() == "-"_sv) {
                m_main_input = stdin;
                return true;
            }
            m_main_input = fopen(argument.characters(), "r");
            if (!m_main_input) {
                fprintf(stderr, "awk: can't open file %s\n", argument.characters());
                m_exit_status = 2;
                continue;
            }
            return true;
        }
    }

    static bool is_name(StringView text)
    {
        if (text.is_empty() || (text[0] >= '0' && text[0] <= '9'))
            return false;
        for (char const c : text) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        return true;
    }

    bool read_record_from(FILE* file, String& out)
    {
        auto const separator = to_string(global(m_slot_rs));
        StringBuilder record;
        int c = fgetc(file);
        if (c == EOF)
            return false;
        // RS="": paragraphs.
        if (separator.view().is_empty()) {
            while (c == '\n')
                c = fgetc(file);
            if (c == EOF)
                return false;
            while (c != EOF) {
                if (c == '\n') {
                    int const after = fgetc(file);
                    if (after == '\n' || after == EOF) {
                        int more = after;
                        while (more == '\n')
                            more = fgetc(file);
                        if (more != EOF)
                            ungetc(more, file);
                        break;
                    }
                    record.append('\n');
                    c = after;
                    continue;
                }
                record.append(static_cast<char>(c));
                c = fgetc(file);
            }
        } else {
            char const end = separator.view()[0];
            while (c != EOF && static_cast<char>(c) != end) {
                record.append(static_cast<char>(c));
                c = fgetc(file);
            }
        }
        out = MUST(record.to_string());
        return true;
    }

    bool read_main_record()
    {
        for (;;) {
            if (!m_main_input && !open_next_main_input())
                return false;
            String record;
            if (read_record_from(m_main_input, record)) {
                set_global(m_slot_nr, Value::from_number(to_number(global(m_slot_nr)) + 1));
                set_global(m_slot_fnr, Value::from_number(to_number(global(m_slot_fnr)) + 1));
                set_record(record.view());
                return true;
            }
            if (m_main_input != stdin)
                fclose(m_main_input);
            m_main_input = nullptr;
            if (m_input_index >= static_cast<long>(to_number(global(m_slot_argc))) && m_used_any_file)
                return false;
        }
    }

    // ---- Fields.

    String m_record;
    Vector<String> m_fields;
    bool m_fields_valid { false };
    bool m_record_valid { true };

    void set_record(StringView record)
    {
        m_record = MUST(String::from_view(record));
        m_record_valid = true;
        m_fields_valid = false;
    }

    void split_fields()
    {
        if (m_fields_valid)
            return;
        m_fields_valid = true;
        m_fields.clear();
        auto const separator = to_string(global(m_slot_fs));
        split_text(m_record.view(), separator.view(), [&](StringView field) {
            m_fields.append(MUST(String::from_view(field)));
        });
        set_global_raw(m_slot_nf, Value::from_number(static_cast<double>(m_fields.size())));
    }

    void rebuild_record()
    {
        if (m_record_valid)
            return;
        auto const separator = to_string(global(m_slot_ofs));
        StringBuilder record;
        for (size_t i = 0; i < m_fields.size(); ++i) {
            if (i)
                record.append(separator.view());
            record.append(m_fields[i].view());
        }
        m_record = MUST(record.to_string());
        m_record_valid = true;
    }

    template<typename Callback>
    void split_text(StringView text, StringView separator, Callback callback)
    {
        // A single space: runs of blanks, with those at the ends ignored.
        if (separator == " "_sv) {
            size_t i = 0;
            while (i < text.length()) {
                while (i < text.length() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
                    ++i;
                size_t const start = i;
                while (i < text.length() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n')
                    ++i;
                if (i > start)
                    callback(text.substring_view(start, i - start));
            }
            return;
        }
        if (text.is_empty())
            return;
        // The record separator being empty makes a newline a separator too.
        bool const paragraphs = to_string(global(m_slot_rs)).view().is_empty();
        if (separator.length() == 1 && separator[0] != '\\' && !paragraphs) {
            size_t start = 0;
            for (size_t i = 0; i < text.length(); ++i) {
                if (text[i] == separator[0]) {
                    callback(text.substring_view(start, i - start));
                    start = i + 1;
                }
            }
            callback(text.substring_view(start));
            return;
        }
        // Otherwise, a regular expression.
        auto* regex = compile(separator);
        auto const owned = MUST(String::from_view(text));
        char const* at = owned.characters();
        for (;;) {
            regmatch_t match[1];
            if (regexec(regex, at, 1, match, at == owned.characters() ? 0 : REG_NOTBOL) != 0 || match[0].rm_eo == match[0].rm_so) {
                // (A paragraph's newlines too.)
                char const* newline = paragraphs ? strchr(at, '\n') : nullptr;
                if (!newline) {
                    callback(StringView(at));
                    return;
                }
                callback(StringView(at, static_cast<size_t>(newline - at)));
                at = newline + 1;
                continue;
            }
            if (paragraphs) {
                char const* newline = strchr(at, '\n');
                if (newline && newline < at + match[0].rm_so) {
                    callback(StringView(at, static_cast<size_t>(newline - at)));
                    at = newline + 1;
                    continue;
                }
            }
            callback(StringView(at, static_cast<size_t>(match[0].rm_so)));
            at += match[0].rm_eo;
        }
    }

    Value field(long index)
    {
        if (index < 0)
            die("trying to access a field below zero");
        if (index == 0) {
            rebuild_record();
            return Value::from_input(m_record.view());
        }
        split_fields();
        if (static_cast<size_t>(index) > m_fields.size())
            return {};
        return Value::from_input(m_fields[static_cast<size_t>(index) - 1].view());
    }

    void set_field(long index, String text)
    {
        if (index < 0)
            die("trying to access a field below zero");
        if (index == 0) {
            set_record(text.view());
            return;
        }
        split_fields();
        while (m_fields.size() < static_cast<size_t>(index))
            m_fields.append(String {});
        m_fields[static_cast<size_t>(index) - 1] = move(text);
        set_global_raw(m_slot_nf, Value::from_number(static_cast<double>(m_fields.size())));
        m_record_valid = false;
    }

    void set_number_of_fields(long count)
    {
        split_fields();
        while (m_fields.size() < static_cast<size_t>(count))
            m_fields.append(String {});
        while (m_fields.size() > static_cast<size_t>(count))
            (void)m_fields.take_last();
        m_record_valid = false;
    }

    // ---- Variables.

    Vector<Cell> m_globals;
    Vector<LocalCell> m_stack;
    size_t m_frame { 0 };

    Value global(int slot)
    {
        if (slot == m_slot_nf)
            split_fields();
        return m_globals[static_cast<size_t>(slot)].value;
    }

    void set_global_raw(int slot, Value value)
    {
        m_globals[static_cast<size_t>(slot)].value = move(value);
    }

    void set_global(int slot, Value value)
    {
        if (slot == m_slot_nf)
            set_number_of_fields(static_cast<long>(to_number(value)));
        if (slot == m_slot_convfmt)
            g_convfmt = to_string(value);
        if (slot == m_slot_ofmt)
            g_ofmt = to_string(value);
        set_global_raw(slot, move(value));
    }

    Array& array_of_global(int slot)
    {
        auto& cell = m_globals[static_cast<size_t>(slot)];
        if (!cell.array)
            cell.array = new Array;
        return *cell.array;
    }

    LocalCell& resolve_local(size_t index)
    {
        LocalCell* cell = &m_stack[index];
        while (cell->alias)
            cell = cell->alias;
        return *cell;
    }

    Array& array_of(Expression const& expression)
    {
        if (expression.kind == Expression::Kind::Variable)
            return array_of_global(expression.slot);
        auto& cell = resolve_local(m_frame + static_cast<size_t>(expression.slot));
        if (cell.global_alias) {
            if (!cell.global_alias->array)
                cell.global_alias->array = new Array;
            return *cell.global_alias->array;
        }
        if (!cell.array) {
            cell.array = new Array;
            cell.owns_array = true;
        }
        return *cell.array;
    }

    String subscript_key(Vector<ExpressionPointer> const& subscripts)
    {
        if (subscripts.size() == 1)
            return to_string(evaluate(*subscripts[0]));
        auto const separator = to_string(global(m_slot_subsep));
        StringBuilder key;
        for (size_t i = 0; i < subscripts.size(); ++i) {
            if (i)
                key.append(separator.view());
            key.append(to_string(evaluate(*subscripts[i])).view());
        }
        return MUST(key.to_string());
    }

    void assign(Expression const& target, Value value)
    {
        switch (target.kind) {
        case Expression::Kind::Variable:
            set_global(target.slot, move(value));
            return;
        case Expression::Kind::Local: {
            auto& cell = resolve_local(m_frame + static_cast<size_t>(target.slot));
            if (cell.global_alias)
                cell.global_alias->value = move(value);
            else
                cell.value = move(value);
            return;
        }
        case Expression::Kind::Field:
            set_field(static_cast<long>(to_number(evaluate(*target.left))), to_string(value));
            return;
        case Expression::Kind::Index: {
            auto& array = array_of(*target.left);
            auto key = subscript_key(target.arguments);
            Cell cell;
            cell.value = move(value);
            MUST(array.try_set(move(key), move(cell)));
            return;
        }
        default:
            die("that can't be assigned to");
        }
    }

    Value read_lvalue(Expression const& target)
    {
        return evaluate(target);
    }

    // ---- Regular expressions, kept once made.

    HashMap<String, regex_t*> m_regexes;

    regex_t* compile(StringView pattern)
    {
        auto key = MUST(String::from_view(pattern));
        if (auto const* found = m_regexes.get(key))
            return *found;
        auto* regex = new regex_t;
        int const status = regcomp(regex, key.characters(), REG_EXTENDED);
        if (status != 0) {
            char message[256];
            regerror(status, regex, message, sizeof(message));
            fprintf(stderr, "awk: bad regular expression \"%s\": %s\n", key.characters(), message);
            exit(2);
        }
        MUST(m_regexes.try_set(move(key), regex));
        return regex;
    }

    regex_t* regex_of(Expression const& expression)
    {
        if (expression.kind == Expression::Kind::Regex)
            return compile(expression.text.view());
        return compile(to_string(evaluate(expression)).view());
    }

    // ---- Expressions.

    int compare(Value const& left, Value const& right)
    {
        bool const left_numeric = left.kind == Value::Kind::Number || left.kind == Value::Kind::NumericString || left.kind == Value::Kind::Uninitialized;
        bool const right_numeric = right.kind == Value::Kind::Number || right.kind == Value::Kind::NumericString || right.kind == Value::Kind::Uninitialized;
        if (left_numeric && right_numeric) {
            double const a = to_number(left);
            double const b = to_number(right);
            return a < b ? -1 : a > b ? 1 : 0;
        }
        auto const a = to_string(left);
        auto const b = to_string(right);
        return strcmp(a.characters(), b.characters());
    }

    Value evaluate(Expression const& expression)
    {
        switch (expression.kind) {
        case Expression::Kind::Number:
            return Value::from_number(expression.number);
        case Expression::Kind::String:
            return Value::from_string(expression.text.view());
        case Expression::Kind::Regex: {
            rebuild_record();
            return Value::from_number(regexec(compile(expression.text.view()), m_record.characters(), 0, nullptr, 0) == 0 ? 1 : 0);
        }
        case Expression::Kind::Variable:
            return global(expression.slot);
        case Expression::Kind::Local: {
            auto& cell = resolve_local(m_frame + static_cast<size_t>(expression.slot));
            return cell.global_alias ? cell.global_alias->value : cell.value;
        }
        case Expression::Kind::Field:
            return field(static_cast<long>(to_number(evaluate(*expression.left))));
        case Expression::Kind::Index: {
            auto& array = array_of(*expression.left);
            auto key = subscript_key(expression.arguments);
            if (auto const* found = array.get(key))
                return found->value;
            // Asking for it makes it, as awk does.
            MUST(array.try_set(move(key), Cell {}));
            return {};
        }
        case Expression::Kind::In: {
            auto& array = array_of(*expression.left);
            auto const key = subscript_key(expression.arguments);
            return Value::from_number(array.get(key) ? 1 : 0);
        }
        case Expression::Kind::Group:
            if (expression.arguments.size() != 1)
                die("a list in parentheses is only for \"in\", or print");
            return evaluate(*expression.arguments[0]);
        case Expression::Kind::Unary: {
            auto const value = evaluate(*expression.left);
            if (expression.op == Token::Not)
                return Value::from_number(to_bool(value) ? 0 : 1);
            double const number = to_number(value);
            return Value::from_number(expression.op == Token::Minus ? -number : number);
        }
        case Expression::Kind::Binary:
            return evaluate_binary(expression);
        case Expression::Kind::Concat: {
            auto const left = to_string(evaluate(*expression.left));
            auto const right = to_string(evaluate(*expression.right));
            StringBuilder both;
            both.append(left.view());
            both.append(right.view());
            return Value::from_string(both.view());
        }
        case Expression::Kind::And:
            return Value::from_number(to_bool(evaluate(*expression.left)) && to_bool(evaluate(*expression.right)) ? 1 : 0);
        case Expression::Kind::Or:
            return Value::from_number(to_bool(evaluate(*expression.left)) || to_bool(evaluate(*expression.right)) ? 1 : 0);
        case Expression::Kind::Match: {
            auto const text = to_string(evaluate(*expression.left));
            bool const matched = regexec(regex_of(*expression.right), text.characters(), 0, nullptr, 0) == 0;
            return Value::from_number(matched != expression.negate ? 1 : 0);
        }
        case Expression::Kind::Conditional:
            return to_bool(evaluate(*expression.left)) ? evaluate(*expression.right) : evaluate(*expression.third);
        case Expression::Kind::Assign: {
            auto value = evaluate(*expression.right);
            if (expression.op != Token::Assign) {
                double const left = to_number(read_lvalue(*expression.left));
                double const right = to_number(value);
                double result = 0;
                switch (expression.op) {
                case Token::AddAssign:
                    result = left + right;
                    break;
                case Token::SubAssign:
                    result = left - right;
                    break;
                case Token::MulAssign:
                    result = left * right;
                    break;
                case Token::DivAssign:
                    if (right == 0)
                        die("division by zero in /=");
                    result = left / right;
                    break;
                case Token::ModAssign:
                    if (right == 0)
                        die("division by zero in %%=");
                    result = fmod(left, right);
                    break;
                case Token::PowAssign:
                    result = pow(left, right);
                    break;
                default:
                    break;
                }
                value = Value::from_number(result);
            }
            assign(*expression.left, value);
            return value;
        }
        case Expression::Kind::PreIncrement:
        case Expression::Kind::PostIncrement: {
            double const before = to_number(read_lvalue(*expression.left));
            double const after = before + expression.increment;
            assign(*expression.left, Value::from_number(after));
            return Value::from_number(expression.kind == Expression::Kind::PreIncrement ? after : before);
        }
        case Expression::Kind::Call:
            return call_function(expression);
        case Expression::Kind::BuiltinCall:
            return call_builtin(expression);
        case Expression::Kind::Getline:
            return evaluate_getline(expression);
        }
        return {};
    }

    Value evaluate_binary(Expression const& expression)
    {
        auto const left = evaluate(*expression.left);
        auto const right = evaluate(*expression.right);
        switch (expression.op) {
        case Token::Plus:
            return Value::from_number(to_number(left) + to_number(right));
        case Token::Minus:
            return Value::from_number(to_number(left) - to_number(right));
        case Token::Star:
            return Value::from_number(to_number(left) * to_number(right));
        case Token::Slash:
            if (to_number(right) == 0)
                die("division by zero");
            return Value::from_number(to_number(left) / to_number(right));
        case Token::Percent:
            if (to_number(right) == 0)
                die("division by zero in %%");
            return Value::from_number(fmod(to_number(left), to_number(right)));
        case Token::Caret:
            return Value::from_number(pow(to_number(left), to_number(right)));
        case Token::Less:
            return Value::from_number(compare(left, right) < 0 ? 1 : 0);
        case Token::LessEqual:
            return Value::from_number(compare(left, right) <= 0 ? 1 : 0);
        case Token::Greater:
            return Value::from_number(compare(left, right) > 0 ? 1 : 0);
        case Token::GreaterEqual:
            return Value::from_number(compare(left, right) >= 0 ? 1 : 0);
        case Token::Equal:
            return Value::from_number(compare(left, right) == 0 ? 1 : 0);
        case Token::NotEqual:
            return Value::from_number(compare(left, right) != 0 ? 1 : 0);
        default:
            return {};
        }
    }

    Value evaluate_getline(Expression const& expression)
    {
        String record;
        FILE* file = nullptr;
        if (expression.getline_kind == 0) {
            for (;;) {
                if (!m_main_input && !open_next_main_input())
                    return Value::from_number(0);
                if (read_record_from(m_main_input, record))
                    break;
                if (m_main_input != stdin)
                    fclose(m_main_input);
                m_main_input = nullptr;
                if (m_input_index >= static_cast<long>(to_number(global(m_slot_argc))) && m_used_any_file)
                    return Value::from_number(0);
            }
            set_global(m_slot_nr, Value::from_number(to_number(global(m_slot_nr)) + 1));
            set_global(m_slot_fnr, Value::from_number(to_number(global(m_slot_fnr)) + 1));
        } else {
            auto const name = to_string(evaluate(*expression.left));
            file = stream(name.view(), expression.getline_kind == 2 ? StreamKind::PipeIn : StreamKind::FileIn);
            if (!file)
                return Value::from_number(-1);
            if (!read_record_from(file, record))
                return Value::from_number(0);
            if (expression.getline_kind == 2)
                set_global(m_slot_nr, Value::from_number(to_number(global(m_slot_nr)) + 1));
        }
        if (expression.right)
            assign(*expression.right, Value::from_input(record.view()));
        else
            set_record(record.view());
        return Value::from_number(1);
    }

    Value call_function(Expression const& expression)
    {
        int const index = m_program.function_index(expression.text.view());
        if (index < 0)
            die("calling a function that isn't defined: %s", expression.text.characters());
        auto& definition = *m_program.functions[static_cast<size_t>(index)];
        if (expression.arguments.size() > definition.parameters.size())
            die("too many arguments to %s", definition.name.characters());
        if (++m_depth > 2000)
            die("functions have called each other too deeply");
        // What's passed along: arrays by reference, everything else by value.
        Vector<LocalCell> frame;
        MUST(frame.try_resize(definition.parameters.size()));
        size_t const new_frame = m_stack.size();
        for (size_t i = 0; i < expression.arguments.size(); ++i) {
            auto const& argument = *expression.arguments[i];
            if (argument.kind == Expression::Kind::Variable) {
                auto& global_cell = m_globals[static_cast<size_t>(argument.slot)];
                // It may turn out to be an array, used as one by the function.
                if (global_cell.array || global_cell.value.kind == Value::Kind::Uninitialized) {
                    frame[i].global_alias = &global_cell;
                    continue;
                }
            } else if (argument.kind == Expression::Kind::Local) {
                auto& caller = resolve_local(m_frame + static_cast<size_t>(argument.slot));
                if (caller.global_alias && (caller.global_alias->array || caller.global_alias->value.kind == Value::Kind::Uninitialized)) {
                    frame[i].global_alias = caller.global_alias;
                    continue;
                }
                if (caller.array || caller.value.kind == Value::Kind::Uninitialized) {
                    frame[i].array = caller.array ? caller.array : (caller.array = new Array);
                    caller.owns_array = true;
                    continue;
                }
            }
            frame[i].value = evaluate(argument);
        }
        size_t const saved_frame = m_frame;
        for (auto& cell : frame)
            m_stack.append(move(cell));
        m_frame = new_frame;
        Value saved_return = move(m_return_value);
        m_return_value = {};
        Flow const flow = execute(*definition.body);
        Value result = move(m_return_value);
        m_return_value = move(saved_return);
        while (m_stack.size() > new_frame)
            (void)m_stack.take_last();
        m_frame = saved_frame;
        --m_depth;
        if (flow == Flow::Exit)
            m_pending_exit = true;
        return result;
    }

    // printf's formatting: each conversion gets what it wants.
    String format(StringView text, Vector<Value> const& values, size_t first)
    {
        StringBuilder out;
        size_t next = first;
        for (size_t i = 0; i < text.length(); ++i) {
            if (text[i] != '%') {
                out.append(text[i]);
                continue;
            }
            if (i + 1 < text.length() && text[i + 1] == '%') {
                out.append('%');
                ++i;
                continue;
            }
            size_t j = i + 1;
            char specification[64];
            size_t used = 0;
            specification[used++] = '%';
            auto add = [&](char c) {
                if (used + 1 < sizeof(specification))
                    specification[used++] = c;
            };
            while (j < text.length() && (text[j] == '-' || text[j] == '+' || text[j] == ' ' || text[j] == '#' || text[j] == '0'))
                add(text[j++]);
            auto star_or_digits = [&] {
                if (j < text.length() && text[j] == '*') {
                    long const value = next < values.size() ? static_cast<long>(to_number(values[next++])) : 0;
                    char digits[32];
                    snprintf(digits, sizeof(digits), "%ld", value);
                    for (char const* d = digits; *d; ++d)
                        add(*d);
                    ++j;
                } else {
                    while (j < text.length() && text[j] >= '0' && text[j] <= '9')
                        add(text[j++]);
                }
            };
            star_or_digits();
            if (j < text.length() && text[j] == '.') {
                add(text[j++]);
                star_or_digits();
            }
            while (j < text.length() && (text[j] == 'l' || text[j] == 'h' || text[j] == 'L'))
                ++j;
            if (j >= text.length()) {
                out.append(text.substring_view(i));
                break;
            }
            char const conversion = text[j];
            char buffer[4096];
            Value const value = next < values.size() ? values[next] : Value {};
            switch (conversion) {
            case 'd':
            case 'i': {
                ++next;
                add('l');
                add('l');
                add('d');
                specification[used] = 0;
                snprintf(buffer, sizeof(buffer), specification, static_cast<long long>(to_number(value)));
                out.append(StringView(buffer));
                break;
            }
            case 'o':
            case 'x':
            case 'X':
            case 'u': {
                ++next;
                add('l');
                add('l');
                add(conversion);
                specification[used] = 0;
                snprintf(buffer, sizeof(buffer), specification, static_cast<unsigned long long>(to_number(value)));
                out.append(StringView(buffer));
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G': {
                ++next;
                add(conversion);
                specification[used] = 0;
                snprintf(buffer, sizeof(buffer), specification, to_number(value));
                out.append(StringView(buffer));
                break;
            }
            case 'c': {
                ++next;
                add('c');
                specification[used] = 0;
                int character = 0;
                if (value.kind == Value::Kind::Number || value.kind == Value::Kind::NumericString) {
                    character = static_cast<int>(to_number(value));
                } else {
                    auto const string = to_string(value);
                    character = string.view().is_empty() ? 0 : static_cast<unsigned char>(string.view()[0]);
                }
                snprintf(buffer, sizeof(buffer), specification, character);
                out.append(StringView(buffer));
                break;
            }
            case 's': {
                ++next;
                add('s');
                specification[used] = 0;
                auto const string = to_string(value);
                snprintf(buffer, sizeof(buffer), specification, string.characters());
                if (string.view().length() >= sizeof(buffer) - 1)
                    out.append(string.view());
                else
                    out.append(StringView(buffer));
                break;
            }
            default:
                out.append(text.substring_view(i, j - i + 1));
                break;
            }
            i = j;
        }
        return MUST(out.to_string());
    }

    Value call_builtin(Expression const& expression);

    // Substitution, for sub and gsub: in "replacement", & is what matched and \\& is an &.
    Value substitute(Expression const& expression, bool global_substitution)
    {
        if (expression.arguments.size() < 2 || expression.arguments.size() > 3)
            die("sub and gsub take two or three arguments");
        auto* regex = regex_of(*expression.arguments[0]);
        auto const replacement = to_string(evaluate(*expression.arguments[1]));
        Expression const* target = expression.arguments.size() == 3 ? expression.arguments[2].ptr() : nullptr;
        if (target && target->kind == Expression::Kind::Group && target->arguments.size() == 1)
            target = target->arguments[0].ptr();
        String const original = target ? to_string(evaluate(*target)) : to_string(field(0));
        StringBuilder out;
        char const* at = original.characters();
        char const* const start = original.characters();
        size_t const length = strlen(start);
        long count = 0;
        while (true) {
            regmatch_t match[1];
            int const flags = at == start ? 0 : REG_NOTBOL;
            if (static_cast<size_t>(at - start) > length || regexec(regex, at, 1, match, flags) != 0) {
                break;
            }
            ++count;
            out.append(StringView(at, static_cast<size_t>(match[0].rm_so)));
            for (size_t i = 0; i < replacement.view().length(); ++i) {
                char const c = replacement.view()[i];
                if (c == '\\' && i + 1 < replacement.view().length() && (replacement.view()[i + 1] == '&' || replacement.view()[i + 1] == '\\')) {
                    out.append(replacement.view()[++i]);
                } else if (c == '&') {
                    out.append(StringView(at + match[0].rm_so, static_cast<size_t>(match[0].rm_eo - match[0].rm_so)));
                } else {
                    out.append(c);
                }
            }
            bool const empty_match = match[0].rm_eo == match[0].rm_so;
            at += match[0].rm_eo;
            if (empty_match) {
                if (*at == 0) {
                    ++at;
                    break;
                }
                out.append(*at);
                ++at;
            }
            if (!global_substitution || *at == 0)
                break;
        }
        if (static_cast<size_t>(at - start) <= length)
            out.append(StringView(at));
        if (count > 0) {
            auto const result = Value::from_string(out.view());
            if (target)
                assign(*target, result);
            else
                set_field(0, to_string(result));
        }
        return Value::from_number(static_cast<double>(count));
    }

    // ---- Statements.

    Flow execute(Statement const& statement)
    {
        if (m_pending_exit)
            return Flow::Exit;
        switch (statement.kind) {
        case Statement::Kind::Expression:
            (void)evaluate(*statement.expression);
            return m_pending_exit ? Flow::Exit : Flow::Normal;
        case Statement::Kind::Block:
            for (auto const& inner : statement.statements) {
                Flow const flow = execute(*inner);
                if (flow != Flow::Normal)
                    return flow;
            }
            return Flow::Normal;
        case Statement::Kind::If:
            if (to_bool(evaluate(*statement.expression)))
                return execute(*statement.body);
            if (statement.otherwise)
                return execute(*statement.otherwise);
            return Flow::Normal;
        case Statement::Kind::While:
            while (to_bool(evaluate(*statement.expression))) {
                Flow const flow = execute(*statement.body);
                if (flow == Flow::Break)
                    break;
                if (flow != Flow::Normal && flow != Flow::Continue)
                    return flow;
            }
            return Flow::Normal;
        case Statement::Kind::DoWhile:
            do {
                Flow const flow = execute(*statement.body);
                if (flow == Flow::Break)
                    break;
                if (flow != Flow::Normal && flow != Flow::Continue)
                    return flow;
            } while (to_bool(evaluate(*statement.expression)));
            return Flow::Normal;
        case Statement::Kind::For:
            if (statement.initial)
                (void)evaluate(*statement.initial);
            while (!statement.expression || to_bool(evaluate(*statement.expression))) {
                Flow const flow = execute(*statement.body);
                if (flow == Flow::Break)
                    break;
                if (flow != Flow::Normal && flow != Flow::Continue)
                    return flow;
                if (statement.step)
                    (void)evaluate(*statement.step);
            }
            return Flow::Normal;
        case Statement::Kind::ForIn: {
            auto& array = array_of(*statement.array);
            Vector<String> keys;
            array.for_each([&](String const& key, Cell const&) {
                keys.append(MUST(String::from_view(key.view())));
            });
            for (auto const& key : keys) {
                if (!array.get(key))
                    continue;
                assign(*statement.variable, Value::from_input(key.view()));
                Flow const flow = execute(*statement.body);
                if (flow == Flow::Break)
                    break;
                if (flow != Flow::Normal && flow != Flow::Continue)
                    return flow;
            }
            return Flow::Normal;
        }
        case Statement::Kind::Next:
            return Flow::Next;
        case Statement::Kind::Break:
            return Flow::Break;
        case Statement::Kind::Continue:
            return Flow::Continue;
        case Statement::Kind::Exit:
            if (statement.expression)
                m_exit_status = static_cast<int>(to_number(evaluate(*statement.expression)));
            return Flow::Exit;
        case Statement::Kind::Return:
            if (statement.expression)
                m_return_value = evaluate(*statement.expression);
            return Flow::Return;
        case Statement::Kind::Delete: {
            auto& array = array_of(*statement.array);
            array.remove(subscript_key(statement.arguments));
            return Flow::Normal;
        }
        case Statement::Kind::DeleteAll:
            array_of(*statement.array).clear();
            return Flow::Normal;
        case Statement::Kind::Print:
            execute_print(statement);
            return Flow::Normal;
        case Statement::Kind::Printf:
            execute_printf(statement);
            return Flow::Normal;
        }
        return Flow::Normal;
    }

    void print_record()
    {
        rebuild_record();
        fputs(m_record.characters(), stdout);
        fputs(to_string(global(m_slot_ors)).characters(), stdout);
    }

    FILE* output_for(Statement const& statement)
    {
        if (statement.redirect == Token::End)
            return stdout;
        auto const name = to_string(evaluate(*statement.expression));
        StreamKind const kind = statement.redirect == Token::Pipe ? StreamKind::PipeOut : statement.redirect == Token::Append ? StreamKind::FileAppend : StreamKind::FileOut;
        FILE* file = stream(name.view(), kind);
        if (!file)
            die("can't redirect to %s", name.characters());
        return file;
    }

    String output_text(Value const& value)
    {
        if (value.kind == Value::Kind::Number)
            return number_to_string(value.number, g_ofmt.view());
        return to_string(value);
    }

    void execute_print(Statement const& statement)
    {
        // What goes out is worked out first: an argument may call something that prints.
        StringBuilder text;
        if (statement.arguments.is_empty()) {
            rebuild_record();
            text.append(m_record.view());
        } else {
            auto const separator = to_string(global(m_slot_ofs));
            for (size_t i = 0; i < statement.arguments.size(); ++i) {
                if (i)
                    text.append(separator.view());
                text.append(output_text(evaluate(*statement.arguments[i])).view());
            }
        }
        text.append(to_string(global(m_slot_ors)).view());
        FILE* out = output_for(statement);
        fwrite(text.view().characters_without_null_termination(), 1, text.view().length(), out);
        if (out != stdout)
            fflush(out);
    }

    void execute_printf(Statement const& statement)
    {
        if (statement.arguments.is_empty())
            die("printf needs a format");
        Vector<Value> values;
        for (auto const& argument : statement.arguments)
            values.append(evaluate(*argument));
        auto const text = format(to_string(values[0]).view(), values, 1);
        FILE* out = output_for(statement);
        fwrite(text.characters(), 1, text.view().length(), out);
        if (out != stdout)
            fflush(out);
    }

    // ---- Files and pipes.

    enum class StreamKind : u8 {
        FileIn,
        FileOut,
        FileAppend,
        PipeIn,
        PipeOut,
    };
    struct Stream {
        String name;
        FILE* file;
        bool is_pipe;
    };
    Vector<Stream> m_streams;

    FILE* stream(StringView name, StreamKind kind)
    {
        for (auto const& stream : m_streams) {
            if (stream.name.view() == name)
                return stream.file;
        }
        auto owned = MUST(String::from_view(name));
        FILE* file = nullptr;
        bool is_pipe = false;
        switch (kind) {
        case StreamKind::FileIn:
            file = name == "-"_sv ? stdin : fopen(owned.characters(), "r");
            break;
        case StreamKind::FileOut:
            file = name == "/dev/stdout"_sv ? stdout : name == "/dev/stderr"_sv ? stderr : fopen(owned.characters(), "w");
            break;
        case StreamKind::FileAppend:
            file = fopen(owned.characters(), "a");
            break;
        case StreamKind::PipeIn:
            fflush(stdout);
            file = popen(owned.characters(), "r");
            is_pipe = true;
            break;
        case StreamKind::PipeOut:
            fflush(stdout);
            file = popen(owned.characters(), "w");
            is_pipe = true;
            break;
        }
        if (!file)
            return nullptr;
        if (file != stdout && file != stderr && file != stdin)
            m_streams.append(Stream { move(owned), file, is_pipe });
        return file;
    }

    int close_stream(StringView name)
    {
        for (size_t i = 0; i < m_streams.size(); ++i) {
            if (m_streams[i].name.view() != name)
                continue;
            int const result = m_streams[i].is_pipe ? pclose(m_streams[i].file) : fclose(m_streams[i].file);
            m_streams.remove(i);
            return result;
        }
        return -1;
    }

    static String process_escapes(StringView text)
    {
        StringBuilder out;
        for (size_t i = 0; i < text.length(); ++i) {
            if (text[i] != '\\' || i + 1 >= text.length()) {
                out.append(text[i]);
                continue;
            }
            char const c = text[++i];
            switch (c) {
            case 'n':
                out.append('\n');
                break;
            case 't':
                out.append('\t');
                break;
            case '\\':
                out.append('\\');
                break;
            case '"':
                out.append('"');
                break;
            default:
                out.append('\\');
                out.append(c);
                break;
            }
        }
        return MUST(out.to_string());
    }

    Program& m_program;
    int m_slot_nf, m_slot_nr, m_slot_fnr, m_slot_fs, m_slot_ofs, m_slot_ors, m_slot_rs, m_slot_filename, m_slot_subsep, m_slot_rstart, m_slot_rlength, m_slot_ofmt, m_slot_convfmt, m_slot_environ, m_slot_argc, m_slot_argv;
    Value m_return_value;
    int m_exit_status { 0 };
    bool m_exiting_from_end { false };
    bool m_pending_exit { false };
    int m_depth { 0 };
    double m_seed { 0 };
    u64 m_random_state { 1 };
};

Value Interpreter::call_builtin(Expression const& expression)
{
    auto argument = [&](size_t index) { return evaluate(*expression.arguments[index]); };
    size_t const count = expression.arguments.size();
    auto need = [&](size_t low, size_t high) {
        if (count < low || count > high)
            die("a built-in function has the wrong number of arguments");
    };
    switch (expression.builtin) {
    case Builtin::Length:
        if (count == 0) {
            rebuild_record();
            return Value::from_number(static_cast<double>(m_record.view().length()));
        }
        need(1, 1);
        // Of an array, if that's what it is.
        if (expression.arguments[0]->kind == Expression::Kind::Variable) {
            auto const& cell = m_globals[static_cast<size_t>(expression.arguments[0]->slot)];
            if (cell.array)
                return Value::from_number(static_cast<double>(cell.array->size()));
        }
        return Value::from_number(static_cast<double>(to_string(argument(0)).view().length()));
    case Builtin::Substr: {
        need(2, 3);
        auto const text = to_string(argument(0));
        double const length = static_cast<double>(text.view().length());
        double const round_start = round(to_number(argument(1)));
        double start = round_start;
        double stop = length + 1;
        if (count == 3) {
            double const requested = round(to_number(argument(2)));
            stop = min(stop, round_start + requested);
        }
        start = max(start, 1.0);
        if (stop <= start)
            return Value::from_string({});
        return Value::from_string(text.view().substring_view(static_cast<size_t>(start) - 1, static_cast<size_t>(stop - start)));
    }
    case Builtin::Index: {
        need(2, 2);
        auto const text = to_string(argument(0));
        auto const target = to_string(argument(1));
        auto const found = text.view().find(target.view());
        return Value::from_number(found.has_value() ? static_cast<double>(found.value() + 1) : 0);
    }
    case Builtin::Split: {
        need(2, 3);
        auto const text = to_string(argument(0));
        auto& array = array_of(*expression.arguments[1]);
        array.clear();
        String separator;
        if (count == 3) {
            auto const& third = *expression.arguments[2];
            separator = third.kind == Expression::Kind::Regex ? MUST(String::from_view(third.text.view())) : to_string(evaluate(third));
        } else {
            separator = to_string(global(m_slot_fs));
        }
        long pieces = 0;
        split_text(text.view(), separator.view(), [&](StringView piece) {
            ++pieces;
            Cell cell;
            cell.value = Value::from_input(piece);
            MUST(array.try_set(number_to_string(static_cast<double>(pieces), "%.6g"_sv), move(cell)));
        });
        return Value::from_number(static_cast<double>(pieces));
    }
    case Builtin::Sub:
        return substitute(expression, false);
    case Builtin::Gsub:
        return substitute(expression, true);
    case Builtin::Match: {
        need(2, 2);
        auto const text = to_string(argument(0));
        regmatch_t match[1];
        if (regexec(regex_of(*expression.arguments[1]), text.characters(), 1, match, 0) != 0) {
            set_global(m_slot_rstart, Value::from_number(0));
            set_global(m_slot_rlength, Value::from_number(-1));
            return Value::from_number(0);
        }
        set_global(m_slot_rstart, Value::from_number(static_cast<double>(match[0].rm_so + 1)));
        set_global(m_slot_rlength, Value::from_number(static_cast<double>(match[0].rm_eo - match[0].rm_so)));
        return Value::from_number(static_cast<double>(match[0].rm_so + 1));
    }
    case Builtin::Sprintf: {
        if (count == 0)
            die("sprintf needs a format");
        Vector<Value> values;
        for (size_t i = 0; i < count; ++i)
            values.append(argument(i));
        auto const text = format(to_string(values[0]).view(), values, 1);
        return Value::from_string(text.view());
    }
    case Builtin::Sin:
        need(1, 1);
        return Value::from_number(sin(to_number(argument(0))));
    case Builtin::Cos:
        need(1, 1);
        return Value::from_number(cos(to_number(argument(0))));
    case Builtin::Atan2:
        need(2, 2);
        return Value::from_number(atan2(to_number(argument(0)), to_number(argument(1))));
    case Builtin::Exp:
        need(1, 1);
        return Value::from_number(exp(to_number(argument(0))));
    case Builtin::Log:
        need(1, 1);
        return Value::from_number(log(to_number(argument(0))));
    case Builtin::Sqrt:
        need(1, 1);
        return Value::from_number(sqrt(to_number(argument(0))));
    case Builtin::Int:
        need(1, 1);
        return Value::from_number(trunc(to_number(argument(0))));
    case Builtin::Rand:
        // Small and repeatable, as awk's is meant to be until srand is called.
        m_random_state = m_random_state * 6364136223846793005ull + 1442695040888963407ull;
        return Value::from_number(static_cast<double>(m_random_state >> 11) / 9007199254740992.0);
    case Builtin::Srand: {
        need(0, 1);
        double const previous = m_seed;
        m_seed = count ? to_number(argument(0)) : static_cast<double>(time(nullptr));
        m_random_state = static_cast<u64>(m_seed) * 2 + 1;
        return Value::from_number(previous);
    }
    case Builtin::Tolower:
    case Builtin::Toupper: {
        need(1, 1);
        auto const text = to_string(argument(0));
        StringBuilder out;
        for (char c : text.view()) {
            if (expression.builtin == Builtin::Tolower && c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + 32);
            else if (expression.builtin == Builtin::Toupper && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 32);
            out.append(c);
        }
        return Value::from_string(out.view());
    }
    case Builtin::System: {
        need(1, 1);
        fflush(stdout);
        auto const command = to_string(argument(0));
        int const status = system(command.characters());
        return Value::from_number((status >> 8) & 0xff);
    }
    case Builtin::Close:
        need(1, 2);
        return Value::from_number(close_stream(to_string(argument(0)).view()));
    case Builtin::Fflush:
        fflush(stdout);
        for (auto const& stream : m_streams)
            fflush(stream.file);
        return Value::from_number(0);
    }
    return {};
}

}

int main(int argc, char** argv)
{
    String field_separator;
    bool has_field_separator = false;
    Vector<String> sources;
    Vector<char const*> assignments;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        char const option = argv[first][1];
        char const* value = argv[first][2] ? argv[first] + 2 : nullptr;
        if (option == 'F' || option == 'v' || option == 'f') {
            if (!value && first + 1 < argc)
                value = argv[++first];
            if (!value) {
                fprintf(stderr, "usage: awk [-F fs] [-v var=value] [prog | -f progfile] [file ...]\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: awk [-F fs] [-v var=value] [prog | -f progfile] [file ...]\n");
            return 2;
        }
        if (option == 'F') {
            // -Ft is a tab: it's an old convention, and it's kept.
            field_separator = MUST(String::from_view(strcmp(value, "t") == 0 ? "\t"_sv : StringView(value)));
            has_field_separator = true;
        } else if (option == 'v') {
            if (!strchr(value, '=')) {
                fprintf(stderr, "awk: invalid -v argument: %s\n", value);
                return 2;
            }
            assignments.append(value);
        } else {
            FILE* file = strcmp(value, "-") == 0 ? stdin : fopen(value, "r");
            if (!file) {
                fprintf(stderr, "awk: can't open file %s\n", value);
                return 2;
            }
            StringBuilder source;
            int c;
            while ((c = fgetc(file)) != EOF)
                source.append(static_cast<char>(c));
            if (file != stdin)
                fclose(file);
            sources.append(MUST(source.to_string()));
        }
    }
    if (sources.is_empty()) {
        if (first >= argc) {
            fprintf(stderr, "usage: awk [-F fs] [-v var=value] [prog | -f progfile] [file ...]\n");
            return 2;
        }
        sources.append(MUST(String::from_view(StringView(argv[first++]))));
    }
    StringBuilder all_source;
    for (auto const& source : sources) {
        all_source.append(source.view());
        all_source.append('\n');
    }
    auto const whole_source = MUST(all_source.to_string());
    Program program;
    Parser parser(whole_source.view(), program);
    parser.parse_program();
    Interpreter interpreter(program);
    if (has_field_separator) {
        StringBuilder assignment;
        assignment.append("FS="_sv);
        assignment.append(field_separator.view());
        interpreter.assign_from_command_line(assignment.view());
    }
    for (auto const* assignment : assignments)
        interpreter.assign_from_command_line(StringView(assignment));
    // ARGV[0] is "awk", then the file names and assignments.
    argv[first - 1] = const_cast<char*>("awk");
    interpreter.set_arguments(argc - first + 1, argv + first - 1);
    return interpreter.run();
}
