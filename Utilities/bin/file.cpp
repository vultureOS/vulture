/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// file [-bhL] [--mime-type] file...: what a file is, by what's in it rather than what it's called.

namespace {

bool g_brief = false, g_mime = false, g_follow = false;

bool starts_with(unsigned char const* data, size_t length, char const* text, size_t text_length)
{
    return length >= text_length && memcmp(data, text, text_length) == 0;
}

// What a run of text is, as far as it can be told.
char const* describe_text(unsigned char const* data, size_t length, bool& is_text)
{
    bool has_high = false, has_long_line = false, has_crlf = false, has_cr = false;
    size_t line = 0;
    is_text = true;
    for (size_t i = 0; i < length; ++i) {
        unsigned char const c = data[i];
        if (c == '\n') {
            if (i > 0 && data[i - 1] == '\r')
                has_crlf = true;
            line = 0;
            continue;
        }
        if (++line > 300)
            has_long_line = true;
        if (c == '\r')
            has_cr = true;
        if (c >= 0x80) {
            has_high = true;
            size_t extra = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : 0;
            if (extra == 0) {
                is_text = false;
                return "data";
            }
            for (size_t j = 1; j <= extra; ++j) {
                // (The buffer can end in the middle of a character.)
                if (i + j < length && (data[i + j] & 0xc0) != 0x80) {
                    is_text = false;
                    return "data";
                }
            }
            i += extra;
            continue;
        }
        if (c < 7 || (c > 13 && c < 27) || (c > 27 && c < 32) || c == 127) {
            is_text = false;
            return "data";
        }
    }
    static char description[128];
    snprintf(description, sizeof(description), "%s text%s%s", has_high ? "Unicode text, UTF-8" : "ASCII",
        has_crlf ? ", with CRLF line terminators" : (has_cr ? ", with CR line terminators" : ""),
        has_long_line ? ", with very long lines" : "");
    // "Unicode text, UTF-8 text" is how file(1) says it, and it's what a Mac says.
    return description;
}

struct Answer {
    char const* description;
    char const* mime;
};

Answer classify(unsigned char const* data, size_t length, char const* buffer_for_script)
{
    (void)buffer_for_script;
    static char built[256];
    if (length == 0)
        return { "empty", "inode/x-empty" };
    if (starts_with(data, length, "\x89PNG\r\n\x1a\n", 8)) {
        if (length >= 26) {
            unsigned const width = static_cast<unsigned>(data[16]) << 24 | static_cast<unsigned>(data[17]) << 16 | static_cast<unsigned>(data[18]) << 8 | data[19];
            unsigned const height = static_cast<unsigned>(data[20]) << 24 | static_cast<unsigned>(data[21]) << 16 | static_cast<unsigned>(data[22]) << 8 | data[23];
            snprintf(built, sizeof(built), "PNG image data, %u x %u, %u-bit", width, height, data[24]);
            return { built, "image/png" };
        }
        return { "PNG image data", "image/png" };
    }
    if (starts_with(data, length, "\xff\xd8\xff", 3))
        return { "JPEG image data", "image/jpeg" };
    if (starts_with(data, length, "GIF87a", 6) || starts_with(data, length, "GIF89a", 6))
        return { "GIF image data", "image/gif" };
    if (starts_with(data, length, "%PDF-", 5)) {
        snprintf(built, sizeof(built), "PDF document, version %.3s", length >= 8 ? reinterpret_cast<char const*>(data) + 5 : "?");
        return { built, "application/pdf" };
    }
    if (starts_with(data, length, "\x1f\x8b", 2))
        return { "gzip compressed data", "application/gzip" };
    if (starts_with(data, length, "PK\x03\x04", 4) || starts_with(data, length, "PK\x05\x06", 4))
        return { "Zip archive data", "application/zip" };
    if (starts_with(data, length, "BZh", 3))
        return { "bzip2 compressed data", "application/x-bzip2" };
    if (starts_with(data, length, "\xfd" "7zXZ", 5))
        return { "XZ compressed data", "application/x-xz" };
    if (length >= 512 && memcmp(data + 257, "ustar", 5) == 0)
        return { "POSIX tar archive", "application/x-tar" };
    if (starts_with(data, length, "RIFF", 4) && length >= 12 && memcmp(data + 8, "WAVE", 4) == 0)
        return { "RIFF (little-endian) data, WAVE audio", "audio/x-wav" };
    if (starts_with(data, length, "\x7f" "ELF", 4) && length >= 20) {
        char const* kind = data[16] == 3 ? "pie executable" : data[16] == 2 ? "executable" : data[16] == 1 ? "relocatable" : data[16] == 4 ? "core file" : "file";
        unsigned const machine = data[18] | static_cast<unsigned>(data[19]) << 8;
        char const* cpu = machine == 183 ? "ARM aarch64" : machine == 62 ? "x86-64" : "unknown";
        snprintf(built, sizeof(built), "ELF %s %s %s, %s", data[4] == 2 ? "64-bit" : "32-bit", data[5] == 1 ? "LSB" : "MSB", kind, cpu);
        return { built, "application/x-executable" };
    }
    if (starts_with(data, length, "bplist00", 8))
        return { "Apple binary property list", "application/octet-stream" };
    bool is_text = false;
    char const* text = describe_text(data, length, is_text);
    if (!is_text)
        return { "data", "application/octet-stream" };
    if (starts_with(data, length, "#!", 2)) {
        char interpreter[64];
        size_t i = 2;
        while (i < length && data[i] == ' ')
            ++i;
        size_t used = 0;
        while (i < length && data[i] != '\n' && data[i] != ' ' && used + 1 < sizeof(interpreter))
            interpreter[used++] = static_cast<char>(data[i++]);
        interpreter[used] = 0;
        // Pretty as file(1) has it: "Bourne-Again shell", "POSIX shell". And what sort of text, after that.
        char const* shell = last_component(interpreter);
        if (strcmp(shell, "sh") == 0)
            shell = "POSIX shell";
        else if (strcmp(shell, "bash") == 0)
            shell = "Bourne-Again shell";
        snprintf(built, sizeof(built), "%s script text executable, %s", shell, text);
        return { built, "text/x-shellscript" };
    }
    if (starts_with(data, length, "<?xml", 5)) {
        snprintf(built, sizeof(built), "XML 1.0 document text, %s", text);
        return { built, "text/xml" };
    }
    if (starts_with(data, length, "<!DOCTYPE html", 14) || starts_with(data, length, "<html", 5)) {
        snprintf(built, sizeof(built), "HTML document text, %s", text);
        return { built, "text/html" };
    }
    if (starts_with(data, length, "{", 1) && length > 1 && (data[1] == '\n' || data[1] == '"' || data[1] == ' '))
        return { "JSON data", "application/json" };
    // C, by its includes, as that's what it would be looking for.
    if (starts_with(data, length, "#include", 8) || starts_with(data, length, "/*", 2)) {
        snprintf(built, sizeof(built), "c program text, %s", text);
        return { built, "text/x-c" };
    }
    return { text, "text/plain" };
}

int describe(char const* path)
{
    if (!g_brief)
        printf("%s: ", path);
    struct stat status;
    if ((g_follow ? stat(path, &status) : lstat(path, &status)) != 0) {
        printf("cannot open `%s' (%s)\n", path, strerror(errno));
        return 0;
    }
    if (S_ISLNK(status.st_mode)) {
        char target[1024];
        ssize_t const length = readlink(path, target, sizeof(target) - 1);
        target[length > 0 ? length : 0] = 0;
        if (g_mime)
            printf("inode/symlink\n");
        else
            printf("symbolic link to %s\n", target);
        return 0;
    }
    if (S_ISDIR(status.st_mode)) {
        puts(g_mime ? "inode/directory" : "directory");
        return 0;
    }
    if (S_ISCHR(status.st_mode)) {
        puts(g_mime ? "inode/chardevice" : "character special");
        return 0;
    }
    if (S_ISBLK(status.st_mode)) {
        puts(g_mime ? "inode/blockdevice" : "block special");
        return 0;
    }
    if (S_ISFIFO(status.st_mode)) {
        puts(g_mime ? "inode/fifo" : "fifo (named pipe)");
        return 0;
    }
    int const fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("regular file, no read permission\n");
        return 0;
    }
    static unsigned char buffer[65536];
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    close(fd);
    auto const answer = classify(buffer, got > 0 ? static_cast<size_t>(got) : 0, nullptr);
    puts(g_mime ? answer.mime : answer.description);
    return 0;
}

}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--mime-type") == 0 || strcmp(argv[first], "-I") == 0) {
            g_mime = true;
            continue;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'b')
                g_brief = true;
            else if (*option == 'L')
                g_follow = true;
            else if (*option != 'h') {
                fprintf(stderr, "usage: file [-bhL] [--mime-type] file ...\n");
                return 1;
            }
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: file [-bhL] [--mime-type] file ...\n");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc; ++i)
        status |= describe(argv[i]);
    return status;
}
