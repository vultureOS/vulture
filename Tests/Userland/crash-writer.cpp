/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The storage crash test (Meta/storage-test.sh). Each boot it first checks
// that everything the last boot committed (made durable with fsync) is
// still there, intact, then writes on until the power is cut:
//
//   1. slot file k % 8 gets new contents for iteration k; fsync
//   2. renamed.tmp gets them too; fsync; renamed onto "renamed"
//   3. "committed" gets k: written aside, synced, renamed over, synced
//
// So after a crash with committed = c, each slot holds exactly the
// contents of the last iteration up to c that wrote it (except the slot
// iteration c + 1 was busy rewriting), and "renamed" holds whole contents
// of iteration c or c + 1, never a mixture. Every file says which
// iteration wrote it and carries a checksum.
//
// --once: write three iterations, then exit (the persistence test).

namespace {

constexpr int slots = 8;
constexpr unsigned magic = 0x5652'4954; // "TIRV"
char const* const directory = "/Users/crash";

struct Header {
    unsigned magic;
    unsigned iteration;
    unsigned length;
    unsigned checksum;
};

unsigned length_for(unsigned iteration)
{
    return 100 + (iteration * 7919u) % 150'000;
}

unsigned char byte_at(unsigned iteration, unsigned position)
{
    return static_cast<unsigned char>((iteration * 2654435761u + position * 40503u) >> 13);
}

unsigned checksum(unsigned char const* data, unsigned length)
{
    unsigned sum = 2166136261u;
    for (unsigned i = 0; i < length; ++i)
        sum = (sum ^ data[i]) * 16777619u;
    return sum;
}

// The contents iteration `iteration` writes; caller frees.
unsigned char* make_contents(unsigned iteration, unsigned& size)
{
    unsigned length = length_for(iteration);
    size = sizeof(Header) + length;
    auto* data = static_cast<unsigned char*>(malloc(size));
    if (!data)
        return nullptr;
    for (unsigned i = 0; i < length; ++i)
        data[sizeof(Header) + i] = byte_at(iteration, i);
    Header header { magic, iteration, length, checksum(data + sizeof(Header), length) };
    memcpy(data, &header, sizeof(header));
    return data;
}

void path_of(char (&path)[64], char const* name)
{
    snprintf(path, sizeof(path), "%s/%s", directory, name);
}

// Reads a file written by make_contents(); false (with a reason) if it is
// missing, incomplete or damaged.
bool read_contents(char const* path, unsigned& iteration, char const*& problem)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        problem = "missing";
        return false;
    }
    struct stat information {};
    fstat(fd, &information);
    auto size = static_cast<unsigned>(information.st_size);
    auto* data = static_cast<unsigned char*>(malloc(size ? size : 1));
    unsigned done = 0;
    while (data && done < size) {
        ssize_t got = read(fd, data + done, size - done);
        if (got <= 0)
            break;
        done += static_cast<unsigned>(got);
    }
    close(fd);
    bool ok = false;
    Header header {};
    if (!data || done != size) {
        problem = "unreadable";
    } else if (size < sizeof(Header)) {
        problem = "too short";
    } else {
        memcpy(&header, data, sizeof(header));
        if (header.magic != magic)
            problem = "no header";
        else if (header.length != size - sizeof(Header) || header.length != length_for(header.iteration))
            problem = "wrong length";
        else if (checksum(data + sizeof(Header), header.length) != header.checksum)
            problem = "bad checksum";
        else
            ok = true;
        for (unsigned i = 0; ok && i < header.length; ++i) {
            if (data[sizeof(Header) + i] != byte_at(header.iteration, i)) {
                problem = "wrong bytes";
                ok = false;
            }
        }
    }
    free(data);
    iteration = header.iteration;
    return ok;
}

bool write_file(char const* path, unsigned char const* data, unsigned size, unsigned iteration)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    // In pieces of varying size, as programs do.
    unsigned done = 0;
    while (done < size) {
        unsigned piece = 1 + (iteration * 31u + done) % 20'000;
        if (piece > size - done)
            piece = size - done;
        if (write(fd, data + done, piece) != static_cast<ssize_t>(piece)) {
            close(fd);
            return false;
        }
        done += piece;
    }
    bool ok = fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

int read_committed()
{
    char path[64];
    path_of(path, "committed");
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    char text[32] {};
    ssize_t length = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (length <= 0)
        return -1;
    return atoi(text);
}

// Checks what the last boot committed; returns how many problems.
int verify(int committed)
{
    int problems = 0, checked = 0;
    char path[64], name[32];
    for (int slot = 0; slot < slots; ++slot) {
        // The slot iteration committed + 1 was rewriting may hold anything.
        if (slot == (committed + 1) % slots || committed < slot)
            continue;
        unsigned expected = static_cast<unsigned>(committed - ((committed - slot) % slots));
        snprintf(name, sizeof(name), "slot-%d", slot);
        path_of(path, name);
        unsigned iteration = 0;
        char const* problem = nullptr;
        ++checked;
        if (!read_contents(path, iteration, problem)) {
            printf("crash-writer: FAIL: %s: %s\n", path, problem);
            ++problems;
        } else if (iteration != expected) {
            printf("crash-writer: FAIL: %s: has iteration %u, expected %u\n", path, iteration, expected);
            ++problems;
        }
    }
    if (committed >= 0) {
        path_of(path, "renamed");
        unsigned iteration = 0;
        char const* problem = nullptr;
        ++checked;
        if (!read_contents(path, iteration, problem)) {
            printf("crash-writer: FAIL: %s: %s\n", path, problem);
            ++problems;
        } else if (iteration != static_cast<unsigned>(committed) && iteration != static_cast<unsigned>(committed + 1)) {
            printf("crash-writer: FAIL: %s: has iteration %u, expected %d or %d\n", path, iteration, committed, committed + 1);
            ++problems;
        }
    }
    printf("crash-writer: verified committed iteration %d, %d files: %s\n", committed, checked, problems ? "FAIL" : "OK");
    return problems;
}

}

int main(int argc, char** argv)
{
    bool once = argc > 1 && strcmp(argv[1], "--once") == 0;
    if (mkdir(directory, 0755) < 0 && errno != EEXIST) {
        printf("crash-writer: FAIL: cannot make %s: %s\n", directory, strerror(errno));
        return 1;
    }
    int committed = read_committed();
    if (verify(committed) != 0)
        return 1;
    printf("crash-writer: writing\n");
    fflush(stdout);

    char path[64], name[32], temporary[64], renamed[64], committed_path[64], committed_temporary[64];
    path_of(temporary, "renamed.tmp");
    path_of(committed_temporary, "committed.tmp");
    path_of(renamed, "renamed");
    path_of(committed_path, "committed");
    for (unsigned iteration = static_cast<unsigned>(committed + 1);; ++iteration) {
        unsigned size = 0;
        auto* contents = make_contents(iteration, size);
        if (!contents) {
            printf("crash-writer: FAIL: out of memory\n");
            return 1;
        }
        snprintf(name, sizeof(name), "slot-%u", iteration % slots);
        path_of(path, name);
        bool ok = write_file(path, contents, size, iteration);
        ok = ok && write_file(temporary, contents, size, iteration + 1);
        ok = ok && rename(temporary, renamed) == 0;
        free(contents);

        // Some churn that is never synced on purpose: files come and go.
        snprintf(name, sizeof(name), "junk-%u", iteration % 5);
        path_of(path, name);
        int junk = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (junk >= 0) {
            char filler[3000];
            memset(filler, static_cast<int>(iteration), sizeof(filler));
            for (unsigned i = 0; i < iteration % 7; ++i)
                (void)write(junk, filler, sizeof(filler));
            close(junk);
        }
        snprintf(name, sizeof(name), "junk-%u", (iteration + 2) % 5);
        path_of(path, name);
        unlink(path);

        // Replaced whole, so a checkpoint never catches it half written.
        char text[32];
        int length = snprintf(text, sizeof(text), "%u\n", iteration);
        int fd = open(committed_temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ok = ok && fd >= 0 && write(fd, text, static_cast<size_t>(length)) == length && fsync(fd) == 0;
        if (fd >= 0)
            close(fd);
        ok = ok && rename(committed_temporary, committed_path) == 0;
        sync();
        if (!ok) {
            printf("crash-writer: FAIL: iteration %u: %s\n", iteration, strerror(errno));
            return 1;
        }
        if (iteration % 10 == 0) {
            printf("crash-writer: iteration %u\n", iteration);
            fflush(stdout);
        }
        if (once && iteration >= static_cast<unsigned>(committed + 3)) {
            printf("crash-writer: done with iteration %u\n", iteration);
            return 0;
        }
    }
}
