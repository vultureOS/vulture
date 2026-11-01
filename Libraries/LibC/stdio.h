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
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FILENAME_MAX 1024

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
int getc_unlocked(FILE*);
int getchar_unlocked(void);
int putc_unlocked(int, FILE*);
int putchar_unlocked(int);
void flockfile(FILE*);
int ftrylockfile(FILE*);
void funlockfile(FILE*);
int getc(FILE*);
int getchar(void);
char* fgets(char* buffer, int size, FILE*);
int fflush(FILE*);
int ferror(FILE*);
int feof(FILE*);
int fileno(FILE*);
FILE* fopen(char const* path, char const* mode);
FILE* fdopen(int fd, char const* mode);
FILE* popen(char const* command, char const* mode);
int pclose(FILE*);
FILE* tmpfile(void);
int setvbuf(FILE*, char* buffer, int mode, size_t size);
void setbuf(FILE*, char* buffer);
void setlinebuf(FILE*);
int ungetc(int, FILE*);
void clearerr(FILE*);
int fseek(FILE*, long offset, int origin);
int fseeko(FILE*, off_t offset, int origin);
long ftell(FILE*);
off_t ftello(FILE*);
void rewind(FILE*);
ssize_t getline(char** line, size_t* capacity, FILE*);
ssize_t getdelim(char** line, size_t* capacity, int delimiter, FILE*);
int asprintf(char** result, char const* format, ...) __printf_like(2, 3);
int vasprintf(char** result, char const* format, va_list);
int scanf(char const* format, ...);
int fscanf(FILE*, char const* format, ...);
int sscanf(char const* string, char const* format, ...);
int vscanf(char const* format, va_list);
int vfscanf(FILE*, char const* format, va_list);
int vsscanf(char const* string, char const* format, va_list);
int fclose(FILE*);
void perror(char const* prefix);
int rename(char const* old_path, char const* new_path);
int remove(char const* path);

__END_DECLS
