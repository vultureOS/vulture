/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

extern "C" {

int strcasecmp(char const* a, char const* b)
{
    for (;; ++a, ++b) {
        int difference = tolower(static_cast<unsigned char>(*a)) - tolower(static_cast<unsigned char>(*b));
        if (difference || !*a)
            return difference;
    }
}

int strncasecmp(char const* a, char const* b, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        int difference = tolower(static_cast<unsigned char>(a[i])) - tolower(static_cast<unsigned char>(b[i]));
        if (difference || !a[i])
            return difference;
    }
    return 0;
}

int ffs(int value)
{
    return value ? __builtin_ctz(static_cast<unsigned>(value)) + 1 : 0;
}

char* strcasestr(char const* haystack, char const* needle)
{
    size_t length = strlen(needle);
    for (; *haystack; ++haystack) {
        if (strncasecmp(haystack, needle, length) == 0)
            return const_cast<char*>(haystack);
    }
    return length == 0 ? const_cast<char*>(haystack) : nullptr;
}

void* memmem(void const* haystack, size_t haystack_length, void const* needle, size_t needle_length)
{
    if (needle_length == 0)
        return const_cast<void*>(haystack);
    auto const* bytes = static_cast<unsigned char const*>(haystack);
    for (size_t i = 0; i + needle_length <= haystack_length; ++i) {
        if (memcmp(bytes + i, needle, needle_length) == 0)
            return const_cast<unsigned char*>(bytes + i);
    }
    return nullptr;
}

void* memrchr(void const* memory, int value, size_t count)
{
    auto const* bytes = static_cast<unsigned char const*>(memory);
    while (count-- > 0) {
        if (bytes[count] == static_cast<unsigned char>(value))
            return const_cast<unsigned char*>(bytes + count);
    }
    return nullptr;
}

size_t strlcat(char* destination, char const* source, size_t size)
{
    size_t existing = strnlen(destination, size);
    if (existing == size)
        return size + strlen(source);
    return existing + strlcpy(destination + existing, source, size - existing);
}

char* stpcpy(char* destination, char const* source)
{
    while ((*destination = *source++))
        ++destination;
    return destination;
}

char* stpncpy(char* destination, char const* source, size_t count)
{
    size_t i = 0;
    for (; i < count && source[i]; ++i)
        destination[i] = source[i];
    char* end = destination + i;
    for (; i < count; ++i)
        destination[i] = '\0';
    return end;
}

char* strsep(char** string, char const* delimiters)
{
    char* start = *string;
    if (!start)
        return nullptr;
    char* end = start + strcspn(start, delimiters);
    if (*end) {
        *end = '\0';
        *string = end + 1;
    } else {
        *string = nullptr;
    }
    return start;
}

char* strchrnul(char const* string, int character)
{
    while (*string && *string != static_cast<char>(character))
        ++string;
    return const_cast<char*>(string);
}

int strerror_r(int error, char* buffer, size_t size)
{
    char const* message = strerror(error);
    if (strlcpy(buffer, message, size) >= size)
        return ERANGE;
    return 0;
}

int strcoll(char const* a, char const* b)
{
    return strcmp(a, b);
}

size_t strxfrm(char* destination, char const* source, size_t size)
{
    return strlcpy(destination, source, size);
}
}
