/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void* malloc(size_t);
void* calloc(size_t count, size_t size);
void* realloc(void*, size_t);
void free(void*);

__noreturn void exit(int status);
__noreturn void _Exit(int status);
__noreturn void abort(void);
int atexit(void (*function)(void));

int atoi(char const*);
long atol(char const*);
long long atoll(char const*);
long strtol(char const*, char** end, int base);
unsigned long strtoul(char const*, char** end, int base);
long long strtoll(char const*, char** end, int base);
unsigned long long strtoull(char const*, char** end, int base);

char* getenv(char const* name);
int setenv(char const* name, char const* value, int overwrite);
int unsetenv(char const* name);
int putenv(char* entry);
int clearenv(void);
char* realpath(char const* path, char* resolved);
int mkstemp(char* template_string);
int mkstemps(char* template_string, int suffix_length);
char* mkdtemp(char* template_string);
int system(char const* command);
double strtod(char const*, char** end);
float strtof(char const*, char** end);
double atof(char const*);
void* bsearch(void const* key, void const* base, size_t count, size_t size, int (*compare)(void const*, void const*));
char const* getprogname(void);
void setprogname(char const* name);

typedef struct {
    int quot;
    int rem;
} div_t;
typedef struct {
    long quot;
    long rem;
} ldiv_t;
typedef struct {
    long long quot;
    long long rem;
} lldiv_t;
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);

int abs(int);
long labs(long);
long long llabs(long long);

// Pseudo-terminals.
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char* ptsname(int fd);
int ptsname_r(int fd, char* buffer, size_t size);

void qsort(void* base, size_t count, size_t size, int (*compare)(void const*, void const*));
int rand(void);
void srand(unsigned seed);

__END_DECLS
