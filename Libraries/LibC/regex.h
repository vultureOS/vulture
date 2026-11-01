/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// POSIX regular expressions, basic (BRE) and extended (ERE): . [ ] [^ ]
// [[:class:]] ^ $ * + ? {m,n} | ( ) and back-references \1-\9, matched
// leftmost-longest is approximated by leftmost with greedy backtracking.

typedef long regoff_t;

typedef struct {
    size_t re_nsub;
    void* re_compiled;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 0x01
#define REG_ICASE 0x02
#define REG_NOSUB 0x04
#define REG_NEWLINE 0x08

#define REG_NOTBOL 0x01
#define REG_NOTEOL 0x02

#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

int regcomp(regex_t*, char const* pattern, int flags);
int regexec(regex_t const*, char const* string, size_t match_count, regmatch_t matches[], int flags);
size_t regerror(int error, regex_t const*, char* buffer, size_t size);
void regfree(regex_t*);

__END_DECLS
