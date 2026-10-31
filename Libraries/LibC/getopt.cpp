/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <getopt.h>
#include <stdio.h>
#include <string.h>

extern "C" {

char* optarg = nullptr;
int optind = 1;
int opterr = 1;
int optopt = 0;
int optreset = 0;

static int s_position = 1; // Within a cluster such as -abc.

static int short_option(int argc, char* const argv[], char const* options)
{
    char const* argument = argv[optind];
    char option = argument[s_position];
    char const* spec = strchr(options, option);
    bool quiet = options[0] == ':';
    optopt = option;
    if (!spec || option == ':') {
        if (opterr && !quiet)
            fprintf(stderr, "%s: illegal option -- %c\n", argv[0], option);
        if (!argument[++s_position]) {
            ++optind;
            s_position = 1;
        }
        return '?';
    }
    if (spec[1] == ':') {
        if (argument[s_position + 1]) {
            optarg = const_cast<char*>(argument + s_position + 1);
            ++optind;
        } else if (spec[2] == ':') {
            optarg = nullptr; // Optional: only attached.
            ++optind;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
        } else {
            ++optind;
            s_position = 1;
            if (opterr && !quiet)
                fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], option);
            return quiet ? ':' : '?';
        }
        s_position = 1;
        return option;
    }
    if (!argument[++s_position]) {
        ++optind;
        s_position = 1;
    }
    return option;
}

int getopt(int argc, char* const argv[], char const* options)
{
    if (optreset || optind == 0) {
        optind = 1;
        s_position = 1;
        optreset = 0;
    }
    optarg = nullptr;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
        return -1;
    if (strcmp(argv[optind], "--") == 0) {
        ++optind;
        return -1;
    }
    return short_option(argc, argv, options);
}

int getopt_long(int argc, char* const argv[], char const* options, struct option const* long_options, int* index)
{
    if (optreset || optind == 0) {
        optind = 1;
        s_position = 1;
        optreset = 0;
    }
    optarg = nullptr;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
        return -1;
    if (strcmp(argv[optind], "--") == 0) {
        ++optind;
        return -1;
    }
    if (argv[optind][1] != '-' || s_position != 1)
        return short_option(argc, argv, options);

    char const* name = argv[optind] + 2;
    char const* equals = strchr(name, '=');
    size_t length = equals ? static_cast<size_t>(equals - name) : strlen(name);
    for (int i = 0; long_options[i].name; ++i) {
        auto const& option = long_options[i];
        if (strncmp(option.name, name, length) != 0 || option.name[length] != '\0')
            continue;
        ++optind;
        if (option.has_arg == required_argument) {
            if (equals) {
                optarg = const_cast<char*>(equals + 1);
            } else if (optind < argc) {
                optarg = argv[optind++];
            } else {
                if (opterr)
                    fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], option.name);
                return '?';
            }
        } else if (option.has_arg == optional_argument && equals) {
            optarg = const_cast<char*>(equals + 1);
        } else if (equals) {
            if (opterr)
                fprintf(stderr, "%s: option '--%s' takes no argument\n", argv[0], option.name);
            return '?';
        }
        if (index)
            *index = i;
        if (option.flag) {
            *option.flag = option.val;
            return 0;
        }
        return option.val;
    }
    if (opterr)
        fprintf(stderr, "%s: unrecognized option '--%.*s'\n", argv[0], static_cast<int>(length), name);
    ++optind;
    return '?';
}
}
