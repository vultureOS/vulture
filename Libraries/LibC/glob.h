/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef struct {
    size_t gl_pathc;
    char** gl_pathv;
    size_t gl_offs;
} glob_t;

#define GLOB_ERR 0x01
#define GLOB_MARK 0x02
#define GLOB_NOSORT 0x04
#define GLOB_DOOFFS 0x08
#define GLOB_NOCHECK 0x10
#define GLOB_APPEND 0x20
#define GLOB_NOESCAPE 0x40
#define GLOB_TILDE 0x800

#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3

// The paths matching a shell pattern ("*.txt", "/usr/*/bin", "~/a?").
int glob(char const* pattern, int flags, int (*error_function)(char const* path, int error), glob_t*);
void globfree(glob_t*);

__END_DECLS
