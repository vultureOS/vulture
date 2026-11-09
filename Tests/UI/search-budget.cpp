/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <SearchEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// search-budget [COUNT]: Search, over that many files. Each is written, and has a word of its own in it; when the index has
// caught up, each thing a person might type is timed, a letter at a time, as Search asks it.

namespace {

double now_ms()
{
    timespec moment;
    clock_gettime(CLOCK_MONOTONIC, &moment);
    return static_cast<double>(moment.tv_sec) * 1000.0 + static_cast<double>(moment.tv_nsec) / 1'000'000.0;
}

void make_files(StringView home, int count)
{
    char path[512];
    snprintf(path, sizeof(path), "%.*s/Documents", static_cast<int>(home.length()), home.characters_without_null_termination());
    (void)mkdir(path, 0755);
    snprintf(path, sizeof(path), "%.*s/Documents/Many", static_cast<int>(home.length()), home.characters_without_null_termination());
    (void)mkdir(path, 0755);
    for (int folder = 0; folder <= (count - 1) / 1000; ++folder) {
        snprintf(path, sizeof(path), "%.*s/Documents/Many/folder%03d", static_cast<int>(home.length()), home.characters_without_null_termination(), folder);
        (void)mkdir(path, 0755);
    }
    for (int i = 0; i < count; ++i) {
        snprintf(path, sizeof(path), "%.*s/Documents/Many/folder%03d/note%06d.txt", static_cast<int>(home.length()), home.characters_without_null_termination(), i / 1000, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            printf("search-budget: %s: %s\n", path, strerror(errno));
            return;
        }
        char text[64];
        int length = snprintf(text, sizeof(text), "word%06d in a file\n", i);
        (void)write(fd, text, static_cast<size_t>(length));
        close(fd);
        if (i % 10000 == 0) {
            printf("search-budget: %d written\n", i);
            fflush(stdout);
        }
    }
}

}

int main(int argc, char** argv)
{
    int const count = argc > 1 ? atoi(argv[1]) : 100'000;
    auto port = IPC::look_up("com.vulture.indexd"_sv);
    if (port.is_error()) {
        printf("budget: search per keystroke: failed\n");
        return 1;
    }
    SearchProxy search(port.release_value());
    auto const home = Desktop::home_directory();
    double const started = now_ms();
    make_files(home.view(), count);
    printf("search-budget: %d files written in %.0f s\n", count, (now_ms() - started) / 1000.0);
    fflush(stdout);

    double const indexing = now_ms();
    for (;;) {
        (void)search.update();
        auto status = search.status();
        if (!status.is_error() && status.value().ready && status.value().files >= static_cast<u64>(count))
            break;
        if (now_ms() - indexing > 600'000.0) {
            printf("budget: search per keystroke: failed\n");
            return 1;
        }
        usleep(500'000);
    }
    printf("search-budget: indexed in %.0f s\n", (now_ms() - indexing) / 1000.0);
    fflush(stdout);

    // As a person types: the name of something, then a word in something's text, then a word that's everywhere.
    char const* const typed[] = { "note050000", "word077777", "file", "folder42", "terminal", "nothing like this anywhere", "a" };
    double worst = 0;
    double total = 0;
    int asked = 0;
    for (auto const* words : typed) {
        size_t const length = strlen(words);
        for (size_t letters = 1; letters <= length; ++letters) {
            double const before = now_ms();
            auto answer = search.query(StringView(words, letters), 8);
            double const taken = now_ms() - before;
            if (answer.is_error()) {
                printf("budget: search per keystroke: failed\n");
                return 1;
            }
            worst = max(worst, taken);
            total += taken;
            ++asked;
        }
    }
    // And it's no use being quick about the wrong answer.
    char expected[128];
    snprintf(expected, sizeof(expected), "note%06d.txt", count / 2);
    char by_name[64];
    snprintf(by_name, sizeof(by_name), "note%06d", count / 2);
    char by_word[64];
    snprintf(by_word, sizeof(by_word), "word%06d", count / 2);
    char const* const both[] = { by_name, by_word };
    for (auto const* wanted : both) {
        auto answer = search.query(StringView(wanted, strlen(wanted)), 8);
        if (answer.is_error() || !answer.value().view().contains(StringView(expected, strlen(expected)))) {
            printf("budget: search per keystroke: failed: %s wasn't found\n", wanted);
            return 1;
        }
    }
    printf("search-budget: found the right files\n");
    printf("search-budget: mean %.2f ms over %d keystrokes\n", total / asked, asked);
    printf("budget: search per keystroke over %d files: %.1f ms\n", count, worst);
    printf("search-budget: done\n");
    return 0;
}
