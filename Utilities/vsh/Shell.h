/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Syntax.h"
#include <LibBase/HashMap.h>
#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

namespace Shell {

struct Variable {
    String value;
    bool exported { false };
};

struct Job {
    int id { 0 };
    pid_t group { 0 };
    Vector<pid_t> processes;
    Vector<bool> finished;
    int status { 0 }; // Of the last process, as waitpid() gives it.
    String text;
    enum class State {
        Running,
        Stopped,
        Done,
    };
    State state { State::Running };
    bool background { false };
    // The terminal's modes when it stopped, given back on fg.
    struct termios modes {};
    bool has_modes { false };
};

class Shell;
using Builtin = int (*)(Shell&, Vector<String>& arguments);

class Shell {
public:
    Shell();

    // Interactive use on a terminal: job control, the line editor, and
    // signals left to the foreground job.
    void enable_interactive_mode();
    [[nodiscard]] bool is_interactive() const { return m_interactive; }

    int run_source(StringView source);
    int run_file(char const* path, Vector<String> arguments);
    int run_interactive();
    [[noreturn]] void exit_shell(int status);

    // Variables.
    [[nodiscard]] Optional<StringView> variable(StringView name) const;
    void set_variable(StringView name, StringView value);
    void export_variable(StringView name);
    void unset_variable(StringView name);
    [[nodiscard]] Vector<String> environment() const;
    void set_positional(Vector<String> arguments) { m_positional = move(arguments); }
    void set_script_name(StringView name) { m_script_name = MUST(String::from_view(name)); }
    [[nodiscard]] int last_status() const { return m_last_status; }
    // $$
    [[nodiscard]] pid_t pid() const { return m_pid; }

    // For builtins.
    HashMap<String, Variable>& variables() { return m_variables; }
    HashMap<String, String>& aliases() { return m_aliases; }
    Vector<String>& positional() { return m_positional; }
    Vector<OwnPtr<Job>>& jobs() { return m_jobs; }
    Vector<String>& history() { return m_history; }
    Job* find_job(StringView specification);
    int continue_job(Job&, bool foreground);
    void report_jobs(bool everything);
    void print_job(Job const&, bool with_pid, FILE* = stderr);
    // zsh's warning before leaving suspended jobs behind: true (and a
    // warning) the first time in a row.
    bool warn_about_suspended_jobs();
    [[nodiscard]] Optional<Builtin> builtin(StringView name) const;
    [[nodiscard]] bool has_function(StringView name) const { return m_functions.contains(name); }
    // A command's path, as PATH finds it.
    [[nodiscard]] Optional<String> find_in_path(StringView name) const;
    int evaluate_test(Vector<String> const& arguments, size_t first, size_t end);
    // Values of $((...)); false (and a message) on a syntax error.
    bool evaluate_arithmetic(StringView expression, i64& result);
    void set_trap(int signal, StringView command);
    StringView trap_command(int signal) const;

    enum class Flow {
        Normal,
        Break,
        Continue,
        Return,
    };
    void set_flow(Flow flow, int count)
    {
        m_flow = flow;
        m_flow_count = count;
    }
    [[nodiscard]] int loop_depth() const { return m_loop_depth; }
    [[nodiscard]] bool in_function() const { return m_function_depth > 0; }

    bool errexit { false };
    bool xtrace { false };
    bool nounset { false };
    bool noglob { false };

private:
    int execute(Node const&);
    int execute_list(Node const&);
    int execute_pipeline(Node const&, bool background);
    int execute_simple(Node const&, bool in_child);
    int execute_in_child(Node const&); // Never returns in a forked child.
    int execute_function(Node const& body, Vector<String> arguments);
    int run_external(Vector<String> const& arguments, Vector<String> const& assignments);
    int wait_for_job(Job&);
    void reap_background_jobs();
    void take_terminal(pid_t group);
    void forget_job(Job&);

    struct SavedDescriptor {
        int fd;
        int saved;
    };
    bool apply_redirections(Vector<Redirection> const&, Vector<SavedDescriptor>* saved);
    void restore_descriptors(Vector<SavedDescriptor>&);

public:
    // Expansion: words to fields (splitting and globbing), or to one
    // string (assignments, redirection targets, case subjects).
    bool expand_words(Vector<String> const& words, Vector<String>& fields);
    bool expand_single(StringView raw, String& result);
    bool expand_pattern(StringView raw, String& result);
    String command_substitution(StringView source);

private:
    HashMap<String, Variable> m_variables;
    HashMap<String, String> m_aliases;
    HashMap<String, Node const*> m_functions;
    Vector<NodePointer> m_programs; // Every parsed program (functions point into them).
    Vector<String> m_positional;
    String m_script_name;
    Vector<OwnPtr<Job>> m_jobs;
    Vector<String> m_history;
    String m_traps[65];

    int m_last_status { 0 };
    pid_t m_last_background { 0 };
    bool m_interactive { false };
    bool m_job_control { false };
    pid_t m_shell_group { 0 };
    // $$: the shell's process, also in subshells.
    pid_t m_pid { 0 };
    bool m_warned_about_jobs { false };
    bool m_warned_on_previous_line { false };
    int m_terminal { -1 };
    struct termios m_terminal_modes {};
    bool m_in_child { false };

    Flow m_flow { Flow::Normal };
    int m_flow_count { 0 };
    int m_loop_depth { 0 };
    int m_function_depth { 0 };
    int m_alias_depth { 0 };
};

int run_builtin(Shell&, Builtin, Vector<String>& arguments);

}
