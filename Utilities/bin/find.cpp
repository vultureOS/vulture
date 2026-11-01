/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// find [path...] [expression]: -name -iname -path -type -size -newer
// -maxdepth -mindepth -empty -perm -user -print -print0 -exec ... \; -delete
// -prune, joined by -a (implicit), -o, !, ( ).
namespace {

enum class Kind {
    True,
    Name,
    IName,
    Path,
    Type,
    Size,
    Newer,
    Empty,
    Perm,
    Print,
    Print0,
    Exec,
    Delete,
    Prune,
    And,
    Or,
    Not,
};

struct Node {
    Kind kind;
    char const* text { nullptr };
    long long number { 0 };
    int compare { 0 }; // -1 less, 0 equal, 1 more
    char** exec_arguments { nullptr };
    int exec_count { 0 };
    Node* left { nullptr };
    Node* right { nullptr };
};

char** s_arguments;
int s_count;
int s_position;
bool s_has_action = false;
int s_max_depth = -1, s_min_depth = 0;
bool s_pruned = false;
int s_status = 0;

Node* make(Kind kind)
{
    auto* node = static_cast<Node*>(calloc(1, sizeof(Node)));
    node->kind = kind;
    return node;
}

Node* parse_or();

Node* parse_primary()
{
    if (s_position >= s_count)
        return make(Kind::True);
    char const* word = s_arguments[s_position++];
    auto argument = [&]() -> char const* {
        if (s_position >= s_count) {
            fprintf(stderr, "find: %s: requires additional arguments\n", word);
            exit(1);
        }
        return s_arguments[s_position++];
    };
    if (strcmp(word, "(") == 0) {
        Node* inner = parse_or();
        if (s_position < s_count && strcmp(s_arguments[s_position], ")") == 0)
            ++s_position;
        return inner;
    }
    if (strcmp(word, "!") == 0 || strcmp(word, "-not") == 0) {
        Node* node = make(Kind::Not);
        node->left = parse_primary();
        return node;
    }
    Node* node;
    if (strcmp(word, "-name") == 0) {
        node = make(Kind::Name);
        node->text = argument();
    } else if (strcmp(word, "-iname") == 0) {
        node = make(Kind::IName);
        node->text = argument();
    } else if (strcmp(word, "-path") == 0 || strcmp(word, "-wholename") == 0) {
        node = make(Kind::Path);
        node->text = argument();
    } else if (strcmp(word, "-type") == 0) {
        node = make(Kind::Type);
        node->text = argument();
    } else if (strcmp(word, "-size") == 0) {
        node = make(Kind::Size);
        char const* text = argument();
        node->compare = text[0] == '+' ? 1 : text[0] == '-' ? -1 : 0;
        char* end;
        node->number = strtoll(text + (node->compare ? 1 : 0), &end, 10);
        long long unit = *end == 'c' ? 1 : *end == 'k' ? 1024 : *end == 'M' ? 1024 * 1024 : *end == 'G' ? 1024LL * 1024 * 1024 : 512;
        node->number *= unit;
    } else if (strcmp(word, "-newer") == 0) {
        node = make(Kind::Newer);
        struct stat reference;
        if (stat(argument(), &reference) == 0)
            node->number = reference.st_mtime;
    } else if (strcmp(word, "-empty") == 0) {
        node = make(Kind::Empty);
    } else if (strcmp(word, "-perm") == 0) {
        node = make(Kind::Perm);
        char const* text = argument();
        node->compare = text[0] == '-' ? 1 : 0;
        node->number = strtol(text + node->compare, nullptr, 8);
    } else if (strcmp(word, "-maxdepth") == 0) {
        s_max_depth = atoi(argument());
        node = make(Kind::True);
    } else if (strcmp(word, "-mindepth") == 0) {
        s_min_depth = atoi(argument());
        node = make(Kind::True);
    } else if (strcmp(word, "-print") == 0) {
        node = make(Kind::Print);
        s_has_action = true;
    } else if (strcmp(word, "-print0") == 0) {
        node = make(Kind::Print0);
        s_has_action = true;
    } else if (strcmp(word, "-delete") == 0) {
        node = make(Kind::Delete);
        s_has_action = true;
    } else if (strcmp(word, "-prune") == 0) {
        node = make(Kind::Prune);
    } else if (strcmp(word, "-exec") == 0 || strcmp(word, "-ok") == 0) {
        node = make(Kind::Exec);
        node->exec_arguments = s_arguments + s_position;
        while (s_position < s_count && strcmp(s_arguments[s_position], ";") != 0 && strcmp(s_arguments[s_position], "+") != 0) {
            ++s_position;
            ++node->exec_count;
        }
        ++s_position;
        s_has_action = true;
    } else {
        fprintf(stderr, "find: %s: unknown primary or operator\n", word);
        exit(1);
    }
    return node;
}

Node* parse_and()
{
    Node* left = parse_primary();
    while (s_position < s_count && strcmp(s_arguments[s_position], "-o") != 0 && strcmp(s_arguments[s_position], "-or") != 0
        && strcmp(s_arguments[s_position], ")") != 0) {
        if (strcmp(s_arguments[s_position], "-a") == 0 || strcmp(s_arguments[s_position], "-and") == 0)
            ++s_position;
        Node* both = make(Kind::And);
        both->left = left;
        both->right = parse_primary();
        left = both;
    }
    return left;
}

Node* parse_or()
{
    Node* left = parse_and();
    while (s_position < s_count && (strcmp(s_arguments[s_position], "-o") == 0 || strcmp(s_arguments[s_position], "-or") == 0)) {
        ++s_position;
        Node* either = make(Kind::Or);
        either->left = left;
        either->right = parse_and();
        left = either;
    }
    return left;
}

bool evaluate(Node* node, char const* path, struct stat const& information)
{
    switch (node->kind) {
    case Kind::True:
        return true;
    case Kind::Name:
    case Kind::IName:
        return fnmatch(node->text, last_component(path), node->kind == Kind::IName ? FNM_CASEFOLD : 0) == 0;
    case Kind::Path:
        return fnmatch(node->text, path, 0) == 0;
    case Kind::Type: {
        char t = node->text[0];
        mode_t mode = information.st_mode;
        return (t == 'f' && S_ISREG(mode)) || (t == 'd' && S_ISDIR(mode)) || (t == 'l' && S_ISLNK(mode)) || (t == 'c' && S_ISCHR(mode))
            || (t == 'b' && S_ISBLK(mode)) || (t == 'p' && S_ISFIFO(mode));
    }
    case Kind::Size: {
        long long size = information.st_size;
        return node->compare > 0 ? size > node->number : node->compare < 0 ? size < node->number : size == node->number;
    }
    case Kind::Newer:
        return information.st_mtime > node->number;
    case Kind::Empty: {
        if (S_ISREG(information.st_mode))
            return information.st_size == 0;
        if (!S_ISDIR(information.st_mode))
            return false;
        DIR* directory = opendir(path);
        if (!directory)
            return false;
        bool empty = true;
        while (struct dirent* entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                empty = false;
                break;
            }
        }
        closedir(directory);
        return empty;
    }
    case Kind::Perm: {
        mode_t mode = information.st_mode & 07777;
        mode_t wanted = static_cast<mode_t>(node->number);
        return node->compare ? (mode & wanted) == wanted : mode == wanted;
    }
    case Kind::Print:
        printf("%s\n", path);
        return true;
    case Kind::Print0:
        fputs(path, stdout);
        putchar('\0');
        return true;
    case Kind::Delete:
        if ((S_ISDIR(information.st_mode) ? rmdir(path) : unlink(path)) < 0) {
            warn("find", path);
            s_status = 1;
            return false;
        }
        return true;
    case Kind::Prune:
        s_pruned = true;
        return true;
    case Kind::Exec: {
        fflush(stdout);
        auto** argv = static_cast<char**>(calloc(static_cast<size_t>(node->exec_count) + 1, sizeof(char*)));
        for (int i = 0; i < node->exec_count; ++i)
            argv[i] = strcmp(node->exec_arguments[i], "{}") == 0 ? const_cast<char*>(path) : node->exec_arguments[i];
        pid_t child = fork();
        if (child == 0) {
            execvp(argv[0], argv);
            fprintf(stderr, "find: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
        int status = 0;
        waitpid(child, &status, 0);
        free(argv);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    case Kind::And:
        return evaluate(node->left, path, information) && evaluate(node->right, path, information);
    case Kind::Or:
        return evaluate(node->left, path, information) || evaluate(node->right, path, information);
    case Kind::Not:
        return !evaluate(node->left, path, information);
    }
    return false;
}

void walk(char const* path, int depth, Node* expression)
{
    struct stat information;
    if (lstat(path, &information) < 0) {
        warn("find", path);
        s_status = 1;
        return;
    }
    s_pruned = false;
    if (depth >= s_min_depth) {
        bool matched = evaluate(expression, path, information);
        if (matched && !s_has_action)
            printf("%s\n", path);
    }
    if (!S_ISDIR(information.st_mode) || s_pruned || (s_max_depth >= 0 && depth >= s_max_depth))
        return;
    DIR* directory = opendir(path);
    if (!directory) {
        warn("find", path);
        s_status = 1;
        return;
    }
    while (struct dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char* child = path_join(path, entry->d_name);
        walk(child, depth + 1, expression);
        free(child);
    }
    closedir(directory);
}

}

int main(int argc, char** argv)
{
    int first_expression = 1;
    while (first_expression < argc && argv[first_expression][0] != '-' && strcmp(argv[first_expression], "(") != 0 && strcmp(argv[first_expression], "!") != 0)
        ++first_expression;
    s_arguments = argv + first_expression;
    s_count = argc - first_expression;
    Node* expression = s_count > 0 ? parse_or() : make(Kind::True);
    if (first_expression == 1) {
        walk(".", 0, expression);
    } else {
        for (int i = 1; i < first_expression; ++i)
            walk(argv[i], 0, expression);
    }
    fflush(stdout);
    return s_status;
}
