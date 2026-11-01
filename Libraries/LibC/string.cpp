/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
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

// macOS's words for each error, as its strerror() gives them.
char* strerror(int error)
{
    static struct {
        int number;
        char const* text;
    } const messages[] = {
        { 0, "Undefined error: 0" },
        { EPERM, "Operation not permitted" },
        { ENOENT, "No such file or directory" },
        { ESRCH, "No such process" },
        { EINTR, "Interrupted system call" },
        { EIO, "Input/output error" },
        { ENXIO, "Device not configured" },
        { E2BIG, "Argument list too long" },
        { ENOEXEC, "Exec format error" },
        { EBADF, "Bad file descriptor" },
        { ECHILD, "No child processes" },
        { EAGAIN, "Resource temporarily unavailable" },
        { ENOMEM, "Cannot allocate memory" },
        { EACCES, "Permission denied" },
        { EFAULT, "Bad address" },
        { EBUSY, "Resource busy" },
        { EEXIST, "File exists" },
        { EXDEV, "Cross-device link" },
        { ENODEV, "Operation not supported by device" },
        { ENOTDIR, "Not a directory" },
        { EISDIR, "Is a directory" },
        { EINVAL, "Invalid argument" },
        { ENFILE, "Too many open files in system" },
        { EMFILE, "Too many open files" },
        { ENOTTY, "Inappropriate ioctl for device" },
        { ETXTBSY, "Text file busy" },
        { EFBIG, "File too large" },
        { ENOSPC, "No space left on device" },
        { ESPIPE, "Illegal seek" },
        { EROFS, "Read-only file system" },
        { EMLINK, "Too many links" },
        { EPIPE, "Broken pipe" },
        { EDOM, "Numerical argument out of domain" },
        { ERANGE, "Result too large" },
        { EDEADLK, "Resource deadlock avoided" },
        { ENAMETOOLONG, "File name too long" },
        { ENOLCK, "No locks available" },
        { ENOSYS, "Function not implemented" },
        { ENOTEMPTY, "Directory not empty" },
        { ELOOP, "Too many levels of symbolic links" },
        { ENOMSG, "No message of desired type" },
        { EIDRM, "Identifier removed" },
        { ENOLINK, "Link has been severed" },
        { EPROTO, "Protocol error" },
        { EBADMSG, "Bad message" },
        { EOVERFLOW, "Value too large to be stored in data type" },
        { EILSEQ, "Illegal byte sequence" },
        { ENOTSOCK, "Socket operation on non-socket" },
        { EDESTADDRREQ, "Destination address required" },
        { EMSGSIZE, "Message too long" },
        { EPROTOTYPE, "Protocol wrong type for socket" },
        { ENOPROTOOPT, "Protocol not available" },
        { EPROTONOSUPPORT, "Protocol not supported" },
        { ENOTSUP, "Operation not supported" },
        { EAFNOSUPPORT, "Address family not supported by protocol family" },
        { EADDRINUSE, "Address already in use" },
        { EADDRNOTAVAIL, "Can't assign requested address" },
        { ENETDOWN, "Network is down" },
        { ENETUNREACH, "Network is unreachable" },
        { ECONNABORTED, "Software caused connection abort" },
        { ECONNRESET, "Connection reset by peer" },
        { ENOBUFS, "No buffer space available" },
        { EISCONN, "Socket is already connected" },
        { ENOTCONN, "Socket is not connected" },
        { ETIMEDOUT, "Operation timed out" },
        { ECONNREFUSED, "Connection refused" },
        { EHOSTUNREACH, "No route to host" },
        { EALREADY, "Operation already in progress" },
        { EINPROGRESS, "Operation now in progress" },
        { ECANCELED, "Operation canceled" },
    };
    for (auto const& message : messages) {
        if (message.number == error)
            return const_cast<char*>(message.text);
    }
    static char unknown[32];
    snprintf(unknown, sizeof(unknown), "Unknown error: %d", error);
    return unknown;
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
