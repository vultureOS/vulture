/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

#include <strings.h>

__BEGIN_DECLS

void* memcpy(void* destination, void const* source, size_t);
void* memmove(void* destination, void const* source, size_t);
void* memset(void* destination, int value, size_t);
int memcmp(void const*, void const*, size_t);
void* memchr(void const*, int value, size_t);

size_t strlen(char const*);
size_t strnlen(char const*, size_t maximum);
int strcmp(char const*, char const*);
int strncmp(char const*, char const*, size_t);
char* strcpy(char* destination, char const* source);
char* strncpy(char* destination, char const* source, size_t);
size_t strlcpy(char* destination, char const* source, size_t size);
char* strcat(char* destination, char const* source);
char* strncat(char* destination, char const* source, size_t);
char* strchr(char const*, int character);
char* strrchr(char const*, int character);
char* strstr(char const* haystack, char const* needle);
size_t strspn(char const*, char const* accept);
size_t strcspn(char const*, char const* reject);
char* strpbrk(char const*, char const* accept);
char* strtok_r(char* string, char const* delimiters, char** state);
char* strtok(char* string, char const* delimiters);
char* strdup(char const*);
char* strndup(char const*, size_t);
char* strerror(int error);
int strerror_r(int error, char* buffer, size_t size);
char* strcasestr(char const* haystack, char const* needle);
void* memmem(void const* haystack, size_t haystack_length, void const* needle, size_t needle_length);
void* memrchr(void const*, int value, size_t);
size_t strlcat(char* destination, char const* source, size_t size);
char* stpcpy(char* destination, char const* source);
char* stpncpy(char* destination, char const* source, size_t);
char* strsep(char** string, char const* delimiters);
char* strchrnul(char const*, int character);
int strcoll(char const*, char const*);
size_t strxfrm(char* destination, char const* source, size_t size);
char const* strsignal(int signal);

__END_DECLS
