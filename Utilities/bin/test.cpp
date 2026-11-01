/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/stat.h>
#include <unistd.h>

// test expression / [ expression ]: as the shell's builtin (see vsh).
namespace {

char** s_arguments;
int s_position, s_end;
bool s_error = false;

bool disjunction();

bool unary(char op, char const* operand)
{
    if (op == 'z')
        return !*operand;
    if (op == 'n')
        return *operand;
    if (op == 't')
        return isatty(atoi(operand));
    struct stat information;
    if ((op == 'L' || op == 'h' ? lstat(operand, &information) : stat(operand, &information)) < 0)
        return false;
    switch (op) {
    case 'e':
        return true;
    case 'f':
        return S_ISREG(information.st_mode);
    case 'd':
        return S_ISDIR(information.st_mode);
    case 'L':
    case 'h':
        return S_ISLNK(information.st_mode);
    case 'c':
        return S_ISCHR(information.st_mode);
    case 'b':
        return S_ISBLK(information.st_mode);
    case 'p':
        return S_ISFIFO(information.st_mode);
    case 's':
        return information.st_size > 0;
    case 'r':
        return access(operand, R_OK) == 0;
    case 'w':
        return access(operand, W_OK) == 0;
    case 'x':
        return access(operand, X_OK) == 0;
    case 'u':
        return information.st_mode & 04000;
    case 'g':
        return information.st_mode & 02000;
    default:
        return false;
    }
}

bool is_binary(char const* op)
{
    static char const* const operators[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot" };
    for (auto const* candidate : operators) {
        if (strcmp(op, candidate) == 0)
            return true;
    }
    return false;
}

bool primary()
{
    if (s_position >= s_end) {
        s_error = true;
        return false;
    }
    char const* word = s_arguments[s_position];
    if (strcmp(word, "!") == 0) {
        ++s_position;
        return !primary();
    }
    if (strcmp(word, "(") == 0) {
        ++s_position;
        bool value = disjunction();
        if (s_position < s_end && strcmp(s_arguments[s_position], ")") == 0)
            ++s_position;
        else
            s_error = true;
        return value;
    }
    if (s_end - s_position >= 3 && is_binary(s_arguments[s_position + 1])) {
        char const* left = word;
        char const* op = s_arguments[s_position + 1];
        char const* right = s_arguments[s_position + 2];
        s_position += 3;
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
            return strcmp(left, right) == 0;
        if (strcmp(op, "!=") == 0)
            return strcmp(left, right) != 0;
        if (strcmp(op, "<") == 0)
            return strcmp(left, right) < 0;
        if (strcmp(op, ">") == 0)
            return strcmp(left, right) > 0;
        if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0) {
            struct stat a, b;
            if (stat(left, &a) < 0 || stat(right, &b) < 0)
                return false;
            return strcmp(op, "-nt") == 0 ? a.st_mtime > b.st_mtime : a.st_mtime < b.st_mtime;
        }
        long long x = strtoll(left, nullptr, 10), y = strtoll(right, nullptr, 10);
        if (strcmp(op, "-eq") == 0)
            return x == y;
        if (strcmp(op, "-ne") == 0)
            return x != y;
        if (strcmp(op, "-lt") == 0)
            return x < y;
        if (strcmp(op, "-le") == 0)
            return x <= y;
        if (strcmp(op, "-gt") == 0)
            return x > y;
        return x >= y;
    }
    if (word[0] == '-' && word[1] && !word[2] && s_end - s_position >= 2 && strchr("bcdefghLprsuwxznt", word[1])) {
        s_position += 2;
        return unary(word[1], s_arguments[s_position - 1]);
    }
    ++s_position;
    return *word;
}

bool conjunction()
{
    bool value = primary();
    while (s_position < s_end && strcmp(s_arguments[s_position], "-a") == 0) {
        ++s_position;
        bool right = primary();
        value = value && right;
    }
    return value;
}

bool disjunction()
{
    bool value = conjunction();
    while (s_position < s_end && strcmp(s_arguments[s_position], "-o") == 0) {
        ++s_position;
        bool right = conjunction();
        value = value || right;
    }
    return value;
}

}

int main(int argc, char** argv)
{
    s_arguments = argv;
    s_position = 1;
    s_end = argc;
    if (strcmp(last_component(argv[0]), "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ]\n");
            return 2;
        }
        --s_end;
    }
    if (s_position >= s_end)
        return 1;
    bool value = disjunction();
    if (s_error || s_position < s_end)
        return 2;
    return value ? 0 : 1;
}
