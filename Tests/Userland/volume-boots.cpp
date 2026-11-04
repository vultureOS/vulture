/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsevents.h>
#include <sys/ioctl.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <unistd.h>

// What takes more than one start of the machine to see, for Meta/volume-test.sh: each time this runs, on the same disk,
// it does the next thing. A file says which that is, except that the first of them goes back to before it was written.

namespace {

constexpr char const* phase_file = "/Users/Shared/volume-boots-phase";
constexpr char const* kept = "/Users/Shared/volume-boots-kept";
constexpr char const* later = "/Users/Shared/volume-boots-later";
constexpr char const* watched = "/Users/Shared/volume-boots-watched";
constexpr char const* event_id_file = "/Users/Shared/volume-boots-event-id";
constexpr char const* damaged = "/Users/Shared/volume-boots-damaged";

void put(char const* path, void const* bytes, size_t size)
{
    int const fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, bytes, size) == static_cast<ssize_t>(size));
    close(fd);
}

void put(char const* path, char const* text)
{
    put(path, text, strlen(text));
}

bool holds(char const* path, char const* text)
{
    char bytes[256] {};
    int const fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t const got = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    return got == static_cast<ssize_t>(strlen(text)) && memcmp(bytes, text, got) == 0;
}

// Before: ask to go back to how things were, which happens at the next mount.
void write_and_go_back()
{
    mkdir("/Users/Shared", 0777);
    put(phase_file, "after rollback");
    put(kept, "original");
    unsigned long long latest = 0;
    int const stream = open("/dev/fsevents", O_RDONLY);
    CHECK(ioctl(stream, FSEVENTS_LATEST, &latest) == 0);
    close(stream);
    char id[32];
    snprintf(id, sizeof(id), "%llu", latest);
    put(event_id_file, id);
    put(watched, "something happened");
    sync();

    int const volume = open("/Users", O_RDONLY);
    CHECK(fs_snapshot_create(volume, "volume-boots", 0) == 0);
    put(kept, "changed since");
    put(later, "made since");
    CHECK(unlink(event_id_file) == 0);
    sync();
    CHECK(fs_snapshot_revert(volume, "volume-boots", 0) == 0);
    close(volume);
    printf("volume-boots: asked to go back\n");
}

void check_rollback_and_events()
{
    // As it was at that moment, exactly.
    CHECK(holds(kept, "original"));
    CHECK(access(later, F_OK) != 0);
    CHECK(access(event_id_file, F_OK) == 0);
    int const volume = open("/Users", O_RDONLY);
    char names[256];
    ssize_t const listed = fs_snapshot_list(volume, names, sizeof(names), 0);
    CHECK(listed == static_cast<ssize_t>(strlen("volume-boots") + 1) && strcmp(names, "volume-boots") == 0);
    CHECK(fs_snapshot_delete(volume, "volume-boots", 0) == 0);
    close(volume);
    printf("volume-boots: went back\n");

    // What happened, from before the machine last started, from the volume.
    char id[32] {};
    int const id_file = open(event_id_file, O_RDONLY);
    CHECK(read(id_file, id, sizeof(id) - 1) > 0);
    close(id_file);
    int const stream = open("/dev/fsevents", O_RDONLY);
    char const* const paths[] = { "/Users/Shared" };
    vulture_fsevents_watch watch {};
    watch.since = strtoull(id, nullptr, 10);
    watch.paths = paths;
    watch.path_count = 1;
    CHECK(ioctl(stream, FSEVENTS_WATCH, &watch) == 0);
    bool heard_created = false;
    bool heard_done = false;
    static char bytes[65536];
    for (int rounds = 0; rounds < 8 && !heard_done; ++rounds) {
        pollfd waiting { stream, POLLIN, 0 };
        if (poll(&waiting, 1, 2000) <= 0)
            break;
        ssize_t const got = read(stream, bytes, sizeof(bytes));
        for (ssize_t at = 0; at < got;) {
            auto const* event = reinterpret_cast<vulture_fsevent const*>(bytes + at);
            char const* path = bytes + at + sizeof(vulture_fsevent);
            if (strcmp(path, watched) == 0 && (event->flags & FSE_ITEM_CREATED))
                heard_created = true;
            if (event->flags & FSE_HISTORY_DONE)
                heard_done = true;
            at += (sizeof(vulture_fsevent) + event->path_length + 7) & ~7ul;
        }
    }
    close(stream);
    CHECK(heard_created);
    CHECK(heard_done);
    printf("volume-boots: heard what happened before the machine started\n");
    put(phase_file, "write what will be damaged");
}

// A pattern volume-test.sh can find on the disk, to change one bit of.
void write_for_damage()
{
    constexpr size_t size = 64 * 1024;
    auto* bytes = static_cast<char*>(malloc(size));
    for (size_t i = 0; i < size; ++i)
        bytes[i] = "volume-boots: damage me "[i % 24];
    put(damaged, bytes, size);
    free(bytes);
    put(phase_file, "read what has been damaged");
    sync();
    printf("volume-boots: wrote what will be damaged\n");
}

void read_damaged()
{
    constexpr size_t size = 64 * 1024;
    auto* bytes = static_cast<char*>(malloc(size));
    int const fd = open(damaged, O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    ssize_t const got = read(fd, bytes, size);
    CHECK(got < 0 && errno == EIO);
    close(fd);
    free(bytes);
    CHECK(unlink(damaged) == 0);
    put(phase_file, "finished");
    sync();
    printf("volume-boots: a damaged block says so\n");
}

}

int main()
{
    if (access(phase_file, F_OK) != 0)
        write_and_go_back();
    else if (holds(phase_file, "after rollback"))
        check_rollback_and_events();
    else if (holds(phase_file, "write what will be damaged"))
        write_for_damage();
    else if (holds(phase_file, "read what has been damaged"))
        read_damaged();
    else
        printf("volume-boots: all done\n");
    return finish();
}
