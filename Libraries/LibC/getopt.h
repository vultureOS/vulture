/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;
extern int optreset;

int getopt(int argc, char* const argv[], char const* options);

#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option {
    char const* name;
    int has_arg;
    int* flag;
    int val;
};

int getopt_long(int argc, char* const argv[], char const* options, struct option const* long_options, int* index);

__END_DECLS
