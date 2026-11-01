/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// The shell language, as POSIX sh has it: pipelines, && and ||, lists,
// background jobs, redirections and here-documents, if/while/until/for/
// case, subshells, brace groups and functions.

namespace Shell {

// Words are kept as written, quotes included; expansion interprets them.
struct Redirection {
    enum class Kind {
        Input,           // <
        Output,          // >
        Append,          // >>
        ReadWrite,       // <>
        DuplicateInput,  // <&
        DuplicateOutput, // >&
        HereDocument,    // <<, <<-
        HereString,      // <<<
        OutputAndError,  // &>
    };
    int fd { 0 };
    Kind kind { Kind::Input };
    String target;
    // Here-documents: the body, and whether the delimiter was quoted (then
    // the body is taken literally).
    String body;
    bool literal_body { false };
};

struct Node;
using NodePointer = OwnPtr<Node>;

struct Node {
    enum class Kind {
        Simple,     // assignments, words, redirections
        Pipeline,   // children: the commands
        Not,        // children[0]
        And,        // children[0] && children[1]
        Or,         // children[0] || children[1]
        Sequence,   // children, one after another
        Background, // children[0], not waited for
        If,         // children: condition, body, [condition, body]..., [else]
        While,      // children: condition, body
        Until,      // children: condition, body
        For,        // name, items (or "$@"), children[0] the body
        Case,       // name (the subject), patterns[i] for children[i]
        Subshell,   // children[0], in a child process
        Group,      // children[0], in this process
        Function,   // name, children[0] the body
    };

    Kind kind { Kind::Simple };
    Vector<String> assignments;
    Vector<String> words;
    Vector<Redirection> redirections;
    Vector<NodePointer> children;
    String name;
    Vector<String> items;
    bool has_items { false };
    Vector<Vector<String>> patterns;
    // How the command was written (for job listings).
    String text;
};

struct ParseResult {
    NodePointer program;
    // Unfinished input (an open quote, a missing "fi"...): more lines may
    // complete it.
    bool incomplete { false };
    String error;
};

ParseResult parse(StringView source);

}
