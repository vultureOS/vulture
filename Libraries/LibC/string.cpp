/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

// memcpy, memmove, memset and memcmp come from LibBase's freestanding set.

extern "C" {

void* memchr(void const* memory, int value, size_t size)
{
    auto const* bytes = static_cast<unsigned char const*>(memory);
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] == static_cast<unsigned char>(value))
            return const_cast<unsigned char*>(bytes + i);
    }
    return nullptr;
}

size_t strlen(char const* string)
{
    size_t length = 0;
    while (string[length])
        ++length;
    return length;
}

size_t strnlen(char const* string, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && string[length])
        ++length;
    return length;
}

int strcmp(char const* a, char const* b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

int strncmp(char const* a, char const* b, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i] || !a[i])
            return static_cast<unsigned char>(a[i]) - static_cast<unsigned char>(b[i]);
    }
    return 0;
}

char* strcpy(char* destination, char const* source)
{
    char* result = destination;
    while ((*destination++ = *source++)) {
    }
    return result;
}

char* strncpy(char* destination, char const* source, size_t count)
{
    size_t i = 0;
    for (; i < count && source[i]; ++i)
        destination[i] = source[i];
    for (; i < count; ++i)
        destination[i] = '\0';
    return destination;
}

size_t strlcpy(char* destination, char const* source, size_t size)
{
    size_t length = strlen(source);
    if (size > 0) {
        size_t copied = length < size - 1 ? length : size - 1;
        memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return length;
}

char* strcat(char* destination, char const* source)
{
    strcpy(destination + strlen(destination), source);
    return destination;
}

char* strncat(char* destination, char const* source, size_t count)
{
    char* end = destination + strlen(destination);
    size_t i = 0;
    for (; i < count && source[i]; ++i)
        end[i] = source[i];
    end[i] = '\0';
    return destination;
}

char* strchr(char const* string, int character)
{
    for (;; ++string) {
        if (*string == static_cast<char>(character))
            return const_cast<char*>(string);
        if (!*string)
            return nullptr;
    }
}

char* strrchr(char const* string, int character)
{
    char const* last = nullptr;
    for (;; ++string) {
        if (*string == static_cast<char>(character))
            last = string;
        if (!*string)
            return const_cast<char*>(last);
    }
}

char* strstr(char const* haystack, char const* needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length == 0)
        return const_cast<char*>(haystack);
    for (; *haystack; ++haystack) {
        if (strncmp(haystack, needle, needle_length) == 0)
            return const_cast<char*>(haystack);
    }
    return nullptr;
}

size_t strspn(char const* string, char const* accept)
{
    size_t length = 0;
    while (string[length] && strchr(accept, string[length]))
        ++length;
    return length;
}

size_t strcspn(char const* string, char const* reject)
{
    size_t length = 0;
    while (string[length] && !strchr(reject, string[length]))
        ++length;
    return length;
}

char* strpbrk(char const* string, char const* accept)
{
    string += strcspn(string, accept);
    return *string ? const_cast<char*>(string) : nullptr;
}

char* strtok_r(char* string, char const* delimiters, char** state)
{
    if (!string)
        string = *state;
    string += strspn(string, delimiters);
    if (!*string) {
        *state = string;
        return nullptr;
    }
    char* end = string + strcspn(string, delimiters);
    if (*end)
        *end++ = '\0';
    *state = end;
    return string;
}

char* strtok(char* string, char const* delimiters)
{
    static char* s_state;
    return strtok_r(string, delimiters, &s_state);
}

char* strdup(char const* string)
{
    return strndup(string, strlen(string));
}

char* strndup(char const* string, size_t maximum)
{
    size_t length = strnlen(string, maximum);
    auto* copy = static_cast<char*>(malloc(length + 1));
    if (!copy)
        return nullptr;
    memcpy(copy, string, length);
    copy[length] = '\0';
    return copy;
}

char* strerror(int error)
{
    switch (error) {
    case 0:
        return const_cast<char*>("Success");
    case EPERM:
        return const_cast<char*>("Operation not permitted");
    case ENOENT:
        return const_cast<char*>("No such file or directory");
    case ESRCH:
        return const_cast<char*>("No such process");
    case EINTR:
        return const_cast<char*>("Interrupted system call");
    case EIO:
        return const_cast<char*>("Input/output error");
    case E2BIG:
        return const_cast<char*>("Argument list too long");
    case ENOEXEC:
        return const_cast<char*>("Exec format error");
    case EBADF:
        return const_cast<char*>("Bad file descriptor");
    case ECHILD:
        return const_cast<char*>("No child processes");
    case EAGAIN:
        return const_cast<char*>("Resource temporarily unavailable");
    case ENOMEM:
        return const_cast<char*>("Out of memory");
    case EACCES:
        return const_cast<char*>("Permission denied");
    case EFAULT:
        return const_cast<char*>("Bad address");
    case EEXIST:
        return const_cast<char*>("File exists");
    case ENOTDIR:
        return const_cast<char*>("Not a directory");
    case EISDIR:
        return const_cast<char*>("Is a directory");
    case EINVAL:
        return const_cast<char*>("Invalid argument");
    case EMFILE:
        return const_cast<char*>("Too many open files");
    case ENOTTY:
        return const_cast<char*>("Not a terminal");
    case ENOSPC:
        return const_cast<char*>("No space left on device");
    case ESPIPE:
        return const_cast<char*>("Illegal seek");
    case EROFS:
        return const_cast<char*>("Read-only file system");
    case EPIPE:
        return const_cast<char*>("Broken pipe");
    case ERANGE:
        return const_cast<char*>("Result out of range");
    case ENAMETOOLONG:
        return const_cast<char*>("File name too long");
    case ENOSYS:
        return const_cast<char*>("Function not implemented");
    default:
        return const_cast<char*>("Unknown error");
    }
}

char const* strsignal(int signal)
{
    switch (signal) {
    case SIGHUP:
        return "Hangup";
    case SIGINT:
        return "Interrupt";
    case SIGQUIT:
        return "Quit";
    case SIGILL:
        return "Illegal instruction";
    case SIGTRAP:
        return "Trace/breakpoint trap";
    case SIGABRT:
        return "Aborted";
    case SIGBUS:
        return "Bus error";
    case SIGFPE:
        return "Floating point exception";
    case SIGKILL:
        return "Killed";
    case SIGUSR1:
        return "User signal 1";
    case SIGSEGV:
        return "Segmentation fault";
    case SIGUSR2:
        return "User signal 2";
    case SIGPIPE:
        return "Broken pipe";
    case SIGALRM:
        return "Alarm clock";
    case SIGTERM:
        return "Terminated";
    case SIGCHLD:
        return "Child exited";
    default:
        return "Unknown signal";
    }
}
}
