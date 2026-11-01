/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

// The pattern is parsed into a tree; matching walks it with backtracking,
// trying alternatives in order and repetitions greedily, which gives the
// leftmost match (and the longest one for the patterns tools commonly use).

namespace {

enum class Kind {
    Literal,     // character
    Any,         // .
    Set,         // [...]
    Start,       // ^
    End,         // $
    Group,       // ( ... ), capturing `group`
    Alternation, // a | b, children
    Concatenation,
    Repeat,      // child{minimum,maximum}
    BackReference,
};

struct Node {
    Kind kind;
    char character { 0 };
    bool set[256] {};
    int group { 0 };
    int minimum { 0 };
    int maximum { -1 }; // -1: no limit
    Node** children { nullptr };
    int child_count { 0 };
    int child_capacity { 0 };
};

struct Compiled {
    Node* root { nullptr };
    int groups { 0 };
    int flags { 0 };
};

Node* make(Kind kind)
{
    auto* node = static_cast<Node*>(calloc(1, sizeof(Node)));
    if (node) {
        node->kind = kind;
        node->maximum = -1;
    }
    return node;
}

bool add_child(Node* parent, Node* child)
{
    if (parent->child_count == parent->child_capacity) {
        int capacity = parent->child_capacity ? parent->child_capacity * 2 : 4;
        auto** grown = static_cast<Node**>(realloc(parent->children, static_cast<size_t>(capacity) * sizeof(Node*)));
        if (!grown)
            return false;
        parent->children = grown;
        parent->child_capacity = capacity;
    }
    parent->children[parent->child_count++] = child;
    return true;
}

void destroy(Node* node)
{
    if (!node)
        return;
    for (int i = 0; i < node->child_count; ++i)
        destroy(node->children[i]);
    free(node->children);
    free(node);
}

class Parser {
public:
    Parser(char const* pattern, int flags)
        : m_pattern(pattern)
        , m_extended(flags & REG_EXTENDED)
        , m_icase(flags & REG_ICASE)
    {
    }

    int error { 0 };
    int groups { 0 };

    Node* parse()
    {
        Node* node = alternation(0);
        if (!error && *m_pattern) {
            error = m_extended ? REG_EPAREN : REG_EPAREN;
        }
        return node;
    }

private:
    bool at_alternation() const
    {
        return m_extended ? *m_pattern == '|' : (m_pattern[0] == '\\' && m_pattern[1] == '|');
    }

    bool at_group_close() const
    {
        return m_extended ? *m_pattern == ')' : (m_pattern[0] == '\\' && m_pattern[1] == ')');
    }

    Node* alternation(int depth)
    {
        Node* first = concatenation(depth);
        if (error || !at_alternation())
            return first;
        Node* alternatives = make(Kind::Alternation);
        add_child(alternatives, first);
        while (at_alternation()) {
            m_pattern += m_extended ? 1 : 2;
            add_child(alternatives, concatenation(depth));
            if (error)
                break;
        }
        return alternatives;
    }

    Node* concatenation(int depth)
    {
        Node* sequence = make(Kind::Concatenation);
        bool at_start = true;
        while (*m_pattern && !at_alternation() && !(depth > 0 && at_group_close()) && !error) {
            Node* atom = parse_atom(at_start);
            at_start = false;
            if (!atom)
                break;
            atom = repetitions(atom);
            add_child(sequence, atom);
        }
        return sequence;
    }

    Node* repetitions(Node* atom)
    {
        for (;;) {
            int minimum, maximum;
            char c = *m_pattern;
            if (c == '*') {
                minimum = 0;
                maximum = -1;
                ++m_pattern;
            } else if (m_extended && c == '+') {
                minimum = 1;
                maximum = -1;
                ++m_pattern;
            } else if (m_extended && c == '?') {
                minimum = 0;
                maximum = 1;
                ++m_pattern;
            } else if (!m_extended && c == '\\' && (m_pattern[1] == '+' || m_pattern[1] == '?')) {
                minimum = m_pattern[1] == '+' ? 1 : 0;
                maximum = m_pattern[1] == '+' ? -1 : 1;
                m_pattern += 2;
            } else if ((m_extended && c == '{') || (!m_extended && c == '\\' && m_pattern[1] == '{')) {
                m_pattern += m_extended ? 1 : 2;
                if (!isdigit(static_cast<unsigned char>(*m_pattern))) {
                    error = REG_BADBR;
                    return atom;
                }
                minimum = static_cast<int>(strtol(m_pattern, const_cast<char**>(&m_pattern), 10));
                maximum = minimum;
                if (*m_pattern == ',') {
                    ++m_pattern;
                    maximum = isdigit(static_cast<unsigned char>(*m_pattern)) ? static_cast<int>(strtol(m_pattern, const_cast<char**>(&m_pattern), 10)) : -1;
                }
                if (m_extended ? *m_pattern != '}' : (m_pattern[0] != '\\' || m_pattern[1] != '}')) {
                    error = REG_EBRACE;
                    return atom;
                }
                m_pattern += m_extended ? 1 : 2;
                if (maximum >= 0 && maximum < minimum) {
                    error = REG_BADBR;
                    return atom;
                }
            } else {
                return atom;
            }
            Node* repeat = make(Kind::Repeat);
            repeat->minimum = minimum;
            repeat->maximum = maximum;
            add_child(repeat, atom);
            atom = repeat;
        }
    }

    Node* literal(char c)
    {
        Node* node;
        if (m_icase && isalpha(static_cast<unsigned char>(c))) {
            node = make(Kind::Set);
            node->set[static_cast<unsigned char>(tolower(c))] = true;
            node->set[static_cast<unsigned char>(toupper(c))] = true;
        } else {
            node = make(Kind::Literal);
            node->character = c;
        }
        return node;
    }

    Node* parse_set()
    {
        // Just past '['.
        Node* node = make(Kind::Set);
        bool negate = false;
        if (*m_pattern == '^') {
            negate = true;
            ++m_pattern;
        }
        bool first = true;
        while (*m_pattern && (first || *m_pattern != ']')) {
            first = false;
            if (m_pattern[0] == '[' && m_pattern[1] == ':') {
                char const* end = strstr(m_pattern + 2, ":]");
                if (!end) {
                    error = REG_EBRACK;
                    return node;
                }
                size_t length = static_cast<size_t>(end - (m_pattern + 2));
                char name[16] = {};
                if (length >= sizeof(name)) {
                    error = REG_ECTYPE;
                    return node;
                }
                memcpy(name, m_pattern + 2, length);
                int (*test)(int) = nullptr;
                if (strcmp(name, "alpha") == 0)
                    test = isalpha;
                else if (strcmp(name, "digit") == 0)
                    test = isdigit;
                else if (strcmp(name, "alnum") == 0)
                    test = isalnum;
                else if (strcmp(name, "space") == 0)
                    test = isspace;
                else if (strcmp(name, "upper") == 0)
                    test = isupper;
                else if (strcmp(name, "lower") == 0)
                    test = islower;
                else if (strcmp(name, "punct") == 0)
                    test = ispunct;
                else if (strcmp(name, "print") == 0)
                    test = isprint;
                else if (strcmp(name, "graph") == 0)
                    test = isgraph;
                else if (strcmp(name, "cntrl") == 0)
                    test = iscntrl;
                else if (strcmp(name, "xdigit") == 0)
                    test = isxdigit;
                else if (strcmp(name, "blank") == 0)
                    test = [](int c) { return static_cast<int>(c == ' ' || c == '\t'); };
                if (!test) {
                    error = REG_ECTYPE;
                    return node;
                }
                for (int c = 0; c < 256; ++c) {
                    if (test(c))
                        node->set[c] = true;
                }
                m_pattern = end + 2;
                continue;
            }
            unsigned char low = static_cast<unsigned char>(*m_pattern++);
            unsigned char high = low;
            if (m_pattern[0] == '-' && m_pattern[1] && m_pattern[1] != ']') {
                high = static_cast<unsigned char>(m_pattern[1]);
                m_pattern += 2;
                if (high < low) {
                    error = REG_ERANGE;
                    return node;
                }
            }
            for (int c = low; c <= high; ++c) {
                node->set[c] = true;
                if (m_icase && isalpha(c)) {
                    node->set[tolower(c)] = true;
                    node->set[toupper(c)] = true;
                }
            }
        }
        if (*m_pattern != ']') {
            error = REG_EBRACK;
            return node;
        }
        ++m_pattern;
        if (negate) {
            for (int c = 0; c < 256; ++c)
                node->set[c] = !node->set[c];
            node->set['\n'] = false;
        }
        return node;
    }

    Node* parse_atom(bool at_start)
    {
        char c = *m_pattern;
        if (c == '^' && (m_extended || at_start)) {
            ++m_pattern;
            return make(Kind::Start);
        }
        if (c == '$' && (m_extended || m_pattern[1] == '\0' || (m_pattern[1] == '\\' && (m_pattern[2] == ')' || m_pattern[2] == '|')))) {
            ++m_pattern;
            return make(Kind::End);
        }
        if (c == '.') {
            ++m_pattern;
            return make(Kind::Any);
        }
        if (c == '[') {
            ++m_pattern;
            return parse_set();
        }
        bool opens_group = m_extended ? c == '(' : (c == '\\' && m_pattern[1] == '(');
        if (opens_group) {
            m_pattern += m_extended ? 1 : 2;
            Node* group = make(Kind::Group);
            group->group = ++groups;
            add_child(group, alternation(1));
            if (!at_group_close()) {
                error = REG_EPAREN;
                return group;
            }
            m_pattern += m_extended ? 1 : 2;
            return group;
        }
        if (c == '*' && at_start) {
            ++m_pattern;
            return literal('*');
        }
        if (m_extended && (c == '*' || c == '+' || c == '?' || c == '{')) {
            error = REG_BADRPT;
            return nullptr;
        }
        if (c == '\\') {
            char next = m_pattern[1];
            if (!next) {
                error = REG_EESCAPE;
                return nullptr;
            }
            m_pattern += 2;
            if (next >= '1' && next <= '9') {
                Node* reference = make(Kind::BackReference);
                reference->group = next - '0';
                if (reference->group > groups)
                    error = REG_ESUBREG;
                return reference;
            }
            if (next == 'n')
                return literal('\n');
            if (next == 't')
                return literal('\t');
            if (next == 'w' || next == 'W' || next == 's' || next == 'S' || next == 'd' || next == 'D') {
                Node* set = make(Kind::Set);
                for (int ch = 0; ch < 256; ++ch) {
                    bool member = next == 'w' || next == 'W' ? (isalnum(ch) || ch == '_') : (next == 's' || next == 'S' ? isspace(ch) : isdigit(ch));
                    set->set[ch] = (next >= 'a') ? member : !member;
                }
                return set;
            }
            return literal(next);
        }
        ++m_pattern;
        return literal(c);
    }

    char const* m_pattern;
    bool m_extended;
    bool m_icase;
};

struct Matcher {
    char const* subject;
    char const* end;
    int flags;
    int compile_flags;
    regoff_t starts[10];
    regoff_t ends[10];
    int steps;
};

// Matches `node` at `position`, then the continuation `rest` (nodes still
// to match after it, a stack). Returns the end of the overall match.
struct Continuation {
    Node** nodes;
    int count;
    Continuation const* outer;
    // For repeats: how many more times may the repeat node recur.
    Node* repeat;
    int done;
};

char const* match_continuation(Matcher&, char const* position, Continuation const* rest);

char const* match_node(Matcher& matcher, Node* node, char const* position, Continuation const* rest)
{
    if (++matcher.steps > 2'000'000)
        return nullptr;
    switch (node->kind) {
    case Kind::Literal:
        if (position < matcher.end && *position == node->character)
            return match_continuation(matcher, position + 1, rest);
        return nullptr;
    case Kind::Any:
        if (position < matcher.end && !((matcher.compile_flags & REG_NEWLINE) && *position == '\n'))
            return match_continuation(matcher, position + 1, rest);
        return nullptr;
    case Kind::Set:
        if (position < matcher.end && node->set[static_cast<unsigned char>(*position)])
            return match_continuation(matcher, position + 1, rest);
        return nullptr;
    case Kind::Start: {
        bool at_start = position == matcher.subject && !(matcher.flags & REG_NOTBOL);
        bool after_newline = (matcher.compile_flags & REG_NEWLINE) && position > matcher.subject && position[-1] == '\n';
        return at_start || after_newline ? match_continuation(matcher, position, rest) : nullptr;
    }
    case Kind::End: {
        bool at_end = position == matcher.end && !(matcher.flags & REG_NOTEOL);
        bool before_newline = (matcher.compile_flags & REG_NEWLINE) && position < matcher.end && *position == '\n';
        return at_end || before_newline ? match_continuation(matcher, position, rest) : nullptr;
    }
    case Kind::BackReference: {
        regoff_t start = matcher.starts[node->group], stop = matcher.ends[node->group];
        if (start < 0 || stop < start)
            return nullptr;
        size_t length = static_cast<size_t>(stop - start);
        if (static_cast<size_t>(matcher.end - position) < length || memcmp(position, matcher.subject + start, length) != 0)
            return nullptr;
        return match_continuation(matcher, position + length, rest);
    }
    case Kind::Concatenation: {
        Continuation next { node->children, node->child_count, rest, nullptr, 0 };
        return match_continuation(matcher, position, &next);
    }
    case Kind::Alternation:
        for (int i = 0; i < node->child_count; ++i) {
            if (char const* result = match_node(matcher, node->children[i], position, rest))
                return result;
        }
        return nullptr;
    case Kind::Group: {
        regoff_t saved_start = matcher.starts[node->group], saved_end = matcher.ends[node->group];
        matcher.starts[node->group] = position - matcher.subject;
        // A marker node closes the group when the child has matched.
        static Node close_marker { Kind::Group, 0, {}, -1, 0, -1, nullptr, 0, 0 };
        Node* marker_array[1] = { &close_marker };
        Continuation closing { marker_array, 1, rest, nullptr, node->group };
        char const* result = match_node(matcher, node->children[0], position, &closing);
        if (!result) {
            matcher.starts[node->group] = saved_start;
            matcher.ends[node->group] = saved_end;
        }
        return result;
    }
    case Kind::Repeat: {
        Continuation again { nullptr, 0, rest, node, 0 };
        return match_continuation(matcher, position, &again);
    }
    }
    return nullptr;
}

// A repeat continuation: try one more (greedy) before moving on.
char const* match_repeat(Matcher& matcher, Node* repeat, int done, char const* position, Continuation const* rest, char const* last_position)
{
    bool may_more = repeat->maximum < 0 || done < repeat->maximum;
    if (may_more && !(done > repeat->minimum && position == last_position)) {
        // After one more child match, come back here with done + 1.
        Continuation more { nullptr, 0, rest, repeat, done + 1 };
        // Remember where this iteration started, to stop empty loops.
        more.nodes = reinterpret_cast<Node**>(const_cast<char*>(position));
        more.count = -1;
        if (char const* result = match_node(matcher, repeat->children[0], position, &more))
            return result;
    }
    if (done >= repeat->minimum)
        return match_continuation(matcher, position, rest);
    return nullptr;
}

char const* match_continuation(Matcher& matcher, char const* position, Continuation const* rest)
{
    if (!rest)
        return position;
    if (rest->repeat) {
        char const* last = rest->count == -1 ? reinterpret_cast<char const*>(rest->nodes) : nullptr;
        return match_repeat(matcher, rest->repeat, rest->done, position, rest->outer, last);
    }
    if (rest->count == 1 && rest->nodes[0]->kind == Kind::Group && rest->nodes[0]->group == -1) {
        // Closing a group.
        regoff_t saved = matcher.ends[rest->done];
        matcher.ends[rest->done] = position - matcher.subject;
        char const* result = match_continuation(matcher, position, rest->outer);
        if (!result)
            matcher.ends[rest->done] = saved;
        return result;
    }
    if (rest->count == 0)
        return match_continuation(matcher, position, rest->outer);
    Continuation remaining { rest->nodes + 1, rest->count - 1, rest->outer, nullptr, 0 };
    return match_node(matcher, rest->nodes[0], position, &remaining);
}

}

extern "C" {

int regcomp(regex_t* regex, char const* pattern, int flags)
{
    Parser parser(pattern, flags);
    Node* root = parser.parse();
    if (parser.error) {
        destroy(root);
        return parser.error;
    }
    auto* compiled = static_cast<Compiled*>(calloc(1, sizeof(Compiled)));
    if (!compiled) {
        destroy(root);
        return REG_ESPACE;
    }
    compiled->root = root;
    compiled->groups = parser.groups;
    compiled->flags = flags;
    regex->re_compiled = compiled;
    regex->re_nsub = static_cast<size_t>(parser.groups);
    return 0;
}

int regexec(regex_t const* regex, char const* string, size_t match_count, regmatch_t matches[], int flags)
{
    auto const* compiled = static_cast<Compiled const*>(regex->re_compiled);
    Matcher matcher;
    matcher.subject = string;
    matcher.end = string + strlen(string);
    matcher.flags = flags;
    matcher.compile_flags = compiled->flags;
    for (char const* start = string; start <= matcher.end; ++start) {
        for (int i = 0; i < 10; ++i)
            matcher.starts[i] = matcher.ends[i] = -1;
        matcher.steps = 0;
        if (char const* end = match_node(matcher, compiled->root, start, nullptr)) {
            if (!(compiled->flags & REG_NOSUB) && matches) {
                for (size_t i = 0; i < match_count; ++i) {
                    if (i == 0) {
                        matches[0].rm_so = start - string;
                        matches[0].rm_eo = end - string;
                    } else if (i < 10 && matcher.starts[i] >= 0 && matcher.ends[i] >= 0) {
                        matches[i].rm_so = matcher.starts[i];
                        matches[i].rm_eo = matcher.ends[i];
                    } else {
                        matches[i].rm_so = matches[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }
        // A pattern anchored at the start can only match there.
        if (compiled->root->kind == Kind::Concatenation && compiled->root->child_count > 0 && compiled->root->children[0]->kind == Kind::Start && !(compiled->flags & REG_NEWLINE))
            break;
    }
    return REG_NOMATCH;
}

size_t regerror(int error, regex_t const*, char* buffer, size_t size)
{
    static char const* const messages[] = { "success", "no match", "invalid regular expression", "invalid collating element",
        "invalid character class", "trailing backslash", "invalid back reference", "unmatched [", "unmatched ( or )", "unmatched {",
        "invalid repetition count", "invalid range", "out of memory", "repetition operator without operand" };
    char const* message = error >= 0 && error < static_cast<int>(sizeof(messages) / sizeof(messages[0])) ? messages[error] : "unknown error";
    size_t length = strlen(message) + 1;
    if (size > 0)
        strlcpy(buffer, message, size);
    return length;
}

void regfree(regex_t* regex)
{
    auto* compiled = static_cast<Compiled*>(regex->re_compiled);
    if (!compiled)
        return;
    destroy(compiled->root);
    free(compiled);
    regex->re_compiled = nullptr;
}

}
