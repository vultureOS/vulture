/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define EOF (-1)
#define BUFSIZ 1024

typedef struct FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int printf(char const* format, ...) __printf_like(1, 2);
int fprintf(FILE*, char const* format, ...) __printf_like(2, 3);
int sprintf(char* buffer, char const* format, ...) __printf_like(2, 3);
int snprintf(char* buffer, size_t size, char const* format, ...) __printf_like(3, 4);
int vprintf(char const* format, va_list);
int vfprintf(FILE*, char const* format, va_list);
int vsprintf(char* buffer, char const* format, va_list);
int vsnprintf(char* buffer, size_t size, char const* format, va_list);
int dprintf(int fd, char const* format, ...) __printf_like(2, 3);

int putchar(int);
int puts(char const*);
int fputc(int, FILE*);
int putc(int, FILE*);
int fputs(char const*, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fgetc(FILE*);
int getc(FILE*);
int getchar(void);
char* fgets(char* buffer, int size, FILE*);
int fflush(FILE*);
int ferror(FILE*);
int feof(FILE*);
int fileno(FILE*);
FILE* fopen(char const* path, char const* mode);
int fclose(FILE*);
void perror(char const* prefix);

__END_DECLS
