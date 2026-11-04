/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/clonefile.h>
#include <sys/fsevents.h>
#include <sys/ioctl.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

// How the system is laid out on its two volumes, and what a data volume can do that isn't in POSIX: access control lists,
// snapshots, what's changed, compression. (Meta/volume-test.sh takes it through what needs the disk from outside.)

namespace {

constexpr uid_t someone_else = 501;

void put(char const* path, char const* text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == static_cast<ssize_t>(strlen(text)));
    close(fd);
}

bool link_says(char const* path, char const* target)
{
    char text[256] {};
    ssize_t const length = readlink(path, text, sizeof(text) - 1);
    return length == static_cast<ssize_t>(strlen(target)) && memcmp(text, target, length) == 0;
}

bool same_thing(char const* a, char const* b)
{
    struct stat first, second;
    return stat(a, &first) == 0 && stat(b, &second) == 0 && first.st_ino == second.st_ino && first.st_dev == second.st_dev;
}

bool refused(int result, int expected)
{
    return result < 0 && errno == expected;
}

void test_layout()
{
    CHECK(link_says("/tmp", "private/tmp"));
    CHECK(link_says("/etc", "private/etc"));
    CHECK(link_says("/var", "private/var"));
    char real[256];
    CHECK(realpath("/tmp", real) && strcmp(real, "/private/tmp") == 0);
    struct stat status;
    CHECK(stat("/tmp", &status) == 0 && (status.st_mode & 07777) == 01777);

    // What's in the template came across, and belongs to whoever it should.
    CHECK(stat("/etc/passwd", &status) == 0 && (status.st_mode & 0777) == 0644);
    CHECK(stat("/etc/master.passwd", &status) == 0 && (status.st_mode & 0777) == 0600);
    CHECK(same_thing("/etc/passwd", "/System/Volumes/Data/private/etc/passwd"));
    CHECK(link_says("/etc/ssl/cert.pem", "/System/Library/Security/cert.pem"));
    CHECK(stat("/System/Library/Security/cert.pem", &status) == 0 && status.st_size > 1000);

    // Each firmlink is the same place as what it leads to, on the other volume.
    char const* const firmlinks[][2] = {
        { "/Applications", "/System/Volumes/Data/Applications" },
        { "/Library", "/System/Volumes/Data/Library" },
        { "/System/Library/Caches", "/System/Volumes/Data/System/Library/Caches" },
        { "/Users", "/System/Volumes/Data/Users" },
        { "/Volumes", "/System/Volumes/Data/Volumes" },
        { "/cores", "/System/Volumes/Data/cores" },
        { "/opt", "/System/Volumes/Data/opt" },
        { "/private", "/System/Volumes/Data/private" },
        { "/usr/local", "/System/Volumes/Data/usr/local" },
    };
    struct stat root;
    CHECK(stat("/", &root) == 0);
    for (auto const& firmlink : firmlinks) {
        if (!same_thing(firmlink[0], firmlink[1]))
            fprintf(stderr, "data-volume: %s isn't %s\n", firmlink[0], firmlink[1]);
        CHECK(same_thing(firmlink[0], firmlink[1]));
        CHECK(stat(firmlink[0], &status) == 0 && status.st_dev != root.st_dev);
    }
    // It's only the one way to go back up, too.
    CHECK(same_thing("/Users/..", "/"));
    CHECK(same_thing("/usr/local/..", "/usr"));

    // What's written through one shows through the other, and a rename from one to another is no move at all.
    mkdir("/Users/Shared", 0777);
    put("/Users/Shared/data-volume-test", "text");
    CHECK(access("/System/Volumes/Data/Users/Shared/data-volume-test", F_OK) == 0);
    CHECK(stat("/Users/Shared/data-volume-test", &status) == 0);
    ino_t const before = status.st_ino;
    CHECK(rename("/Users/Shared/data-volume-test", "/tmp/data-volume-test") == 0);
    CHECK(stat("/tmp/data-volume-test", &status) == 0 && status.st_ino == before);
    CHECK(unlink("/tmp/data-volume-test") == 0);

    // All the rest is the system's, and can't be changed.
    CHECK(refused(mkdir("/made-up", 0755), EROFS));
    CHECK(refused(mkdir("/System/made-up", 0755), EROFS));
    CHECK(refused(open("/System/Library/made-up", O_WRONLY | O_CREAT, 0644), EROFS));
    CHECK(refused(open("/usr/bin/true", O_WRONLY), EROFS));
    CHECK(refused(unlink("/usr/bin/true"), EROFS));
    CHECK(refused(rename("/System/Applications/Calculator.app", "/System/Applications/Other.app"), EROFS));
    CHECK(refused(rename("/usr/bin/true", "/tmp/stolen"), EXDEV));
    CHECK(refused(chmod("/usr/bin/true", 0777), EROFS));
    CHECK(refused(setxattr("/usr/bin/true", "org.example.test", "1", 1, 0, 0), EROFS));
    CHECK(refused(symlink("x", "/usr/bin/made-up"), EROFS));
    CHECK(refused(link("/etc/passwd", "/usr/bin/made-up"), EXDEV) || errno == EROFS);
    CHECK(refused(rmdir("/Users"), EBUSY) || errno == EROFS);
    CHECK(access("/System/Applications/Calculator.app", F_OK) == 0);
    CHECK(access("/Applications/Calculator.app", F_OK) != 0);

    // What's left of the disk underneath it can't be written to either, whichever one that is.
    vulture_mount_info mounts[16];
    size_t size = sizeof(mounts);
    CHECK(sysctlbyname("vfs.mounts", mounts, &size, nullptr, 0) == 0);
    bool checked_the_disk = false;
    for (size_t i = 0; i < size / sizeof(mounts[0]); ++i) {
        if (strcmp(mounts[i].point, "/") != 0 || strncmp(mounts[i].device, "disk", 4) != 0)
            continue;
        char device[64];
        snprintf(device, sizeof(device), "/dev/%s", mounts[i].device);
        CHECK(refused(open(device, O_WRONLY), EPERM));
        checked_the_disk = true;
    }
    CHECK(checked_the_disk);
}

// Runs this as someone else.
template<typename Check>
bool as_someone_else(Check check)
{
    pid_t const child = fork();
    if (child == 0) {
        setgid(someone_else);
        setuid(someone_else);
        _exit(check() ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

vulture_acl_entry line(unsigned char kind, unsigned char tag, unsigned id, unsigned permissions, unsigned short inheritance = 0)
{
    vulture_acl_entry entry {};
    entry.kind = kind;
    entry.tag = tag;
    entry.id = id;
    entry.permissions = permissions;
    entry.inheritance = inheritance;
    return entry;
}

void test_access_control_lists()
{
    mkdir("/tmp/acl", 0755);
    char const* const secret = "/tmp/acl/secret";
    put(secret, "secret");
    chmod(secret, 0600);
    CHECK(as_someone_else([&] { return refused(open(secret, O_RDONLY), EACCES); }));

    vulture_acl_entry entries[8];
    CHECK(vulture_acl_get(secret, entries, 8, 1) == 0);
    entries[0] = line(VULTURE_ACL_ALLOW, VULTURE_ACL_USER, someone_else, VULTURE_ACL_READ_DATA);
    CHECK(vulture_acl_set(secret, entries, 1, 1) == 0);
    vulture_acl_entry seen[8] {};
    CHECK(vulture_acl_get(secret, seen, 8, 1) == 1);
    CHECK(seen[0].kind == VULTURE_ACL_ALLOW && seen[0].tag == VULTURE_ACL_USER && seen[0].id == someone_else && seen[0].permissions == VULTURE_ACL_READ_DATA);
    // It lets them read it, and no more than that.
    CHECK(as_someone_else([&] {
        int const fd = open(secret, O_RDONLY);
        if (fd < 0)
            return false;
        close(fd);
        return true;
    }));
    CHECK(as_someone_else([&] { return refused(open(secret, O_WRONLY), EACCES); }));

    // Saying no first beats what the mode says, and what the next line does.
    char const* const open_file = "/tmp/acl/open";
    put(open_file, "open");
    chmod(open_file, 0666);
    entries[0] = line(VULTURE_ACL_DENY, VULTURE_ACL_USER, someone_else, VULTURE_ACL_WRITE_DATA);
    entries[1] = line(VULTURE_ACL_ALLOW, VULTURE_ACL_EVERYONE, 0, VULTURE_ACL_WRITE_DATA | VULTURE_ACL_READ_DATA);
    CHECK(vulture_acl_set(open_file, entries, 2, 1) == 0);
    CHECK(as_someone_else([&] { return refused(open(open_file, O_WRONLY), EACCES); }));
    CHECK(as_someone_else([&] {
        int const fd = open(open_file, O_RDONLY);
        if (fd < 0)
            return false;
        close(fd);
        return true;
    }));

    // Being able to delete it, in a directory they couldn't otherwise.
    mkdir("/tmp/acl/closed", 0755);
    put("/tmp/acl/closed/removable", "x");
    CHECK(as_someone_else([&] { return refused(unlink("/tmp/acl/closed/removable"), EACCES); }));
    entries[0] = line(VULTURE_ACL_ALLOW, VULTURE_ACL_USER, someone_else, VULTURE_ACL_DELETE);
    CHECK(vulture_acl_set("/tmp/acl/closed/removable", entries, 1, 1) == 0);
    CHECK(as_someone_else([&] { return unlink("/tmp/acl/closed/removable") == 0; }));

    // Lines that are for inheriting go on what's made in there.
    mkdir("/tmp/acl/parent", 0755);
    entries[0] = line(VULTURE_ACL_ALLOW, VULTURE_ACL_USER, someone_else, VULTURE_ACL_READ_DATA, VULTURE_ACL_FILE_INHERIT);
    entries[1] = line(VULTURE_ACL_ALLOW, VULTURE_ACL_USER, 502, VULTURE_ACL_READ_DATA, VULTURE_ACL_DIRECTORY_INHERIT);
    CHECK(vulture_acl_set("/tmp/acl/parent", entries, 2, 1) == 0);
    put("/tmp/acl/parent/file", "x");
    CHECK(vulture_acl_get("/tmp/acl/parent/file", seen, 8, 1) == 1);
    CHECK(seen[0].id == someone_else && (seen[0].inheritance & VULTURE_ACL_INHERITED));
    mkdir("/tmp/acl/parent/sub", 0755);
    CHECK(vulture_acl_get("/tmp/acl/parent/sub", seen, 8, 1) == 1 && seen[0].id == 502);

    // Only its owner can change them; and with none, it's as if they had never been.
    CHECK(as_someone_else([&] { return refused(vulture_acl_set(secret, entries, 1, 1), EPERM); }));
    CHECK(vulture_acl_set(secret, entries, 0, 1) == 0);
    CHECK(vulture_acl_get(secret, seen, 8, 1) == 0);
    CHECK(as_someone_else([&] { return refused(open(secret, O_RDONLY), EACCES); }));

    // A clone gets them too.
    CHECK(clonefile(open_file, "/tmp/acl/clone", 0) == 0);
    CHECK(vulture_acl_get("/tmp/acl/clone", seen, 8, 1) == 2);
    system("rm -rf /tmp/acl");
}

void test_snapshots()
{
    int const volume = open("/private", O_RDONLY);
    CHECK(volume >= 0);
    char names[1024];
    ssize_t const before = fs_snapshot_list(volume, names, sizeof(names), 0);
    CHECK(before >= 0);
    CHECK(fs_snapshot_create(volume, "data-volume-test", 0) == 0);
    CHECK(refused(fs_snapshot_create(volume, "data-volume-test", 0), EEXIST));
    ssize_t const listed = fs_snapshot_list(volume, names, sizeof(names), 0);
    CHECK(listed == before + static_cast<ssize_t>(strlen("data-volume-test") + 1));
    bool found = false;
    for (char const* name = names; name < names + listed; name += strlen(name) + 1)
        found = found || strcmp(name, "data-volume-test") == 0;
    CHECK(found);
    CHECK(as_someone_else([&] { return refused(fs_snapshot_create(volume, "theirs", 0), EPERM); }));
    CHECK(as_someone_else([&] { return refused(fs_snapshot_delete(volume, "data-volume-test", 0), EPERM); }));
    CHECK(refused(fs_snapshot_delete(volume, "no such snapshot", 0), ENOENT));
    CHECK(fs_snapshot_delete(volume, "data-volume-test", 0) == 0);
    CHECK(fs_snapshot_list(volume, names, sizeof(names), 0) == before);
    close(volume);

    // The system's own volume can't have any.
    int const system_volume = open("/usr", O_RDONLY);
    CHECK(refused(fs_snapshot_create(system_volume, "no", 0), EROFS));
    close(system_volume);
}

struct Heard {
    char path[256];
    unsigned flags;
    unsigned long long id;
};

// What's waiting: however much it is, as long as it's soon.
int listen_for_events(int stream, Heard* heard, int room)
{
    int count = 0;
    char bytes[65536];
    for (;;) {
        pollfd waiting { stream, POLLIN, 0 };
        if (poll(&waiting, 1, count ? 100 : 2000) <= 0)
            return count;
        ssize_t const got = read(stream, bytes, sizeof(bytes));
        if (got <= 0)
            return count;
        for (ssize_t at = 0; at < got && count < room;) {
            auto const* event = reinterpret_cast<vulture_fsevent const*>(bytes + at);
            auto const* name = bytes + at + sizeof(vulture_fsevent);
            snprintf(heard[count].path, sizeof(heard[count].path), "%s", name);
            heard[count].flags = event->flags;
            heard[count].id = event->id;
            ++count;
            at += (sizeof(vulture_fsevent) + event->path_length + 7) & ~7ul;
        }
    }
}

bool was_heard(Heard const* heard, int count, char const* path, unsigned flags)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(heard[i].path, path) == 0 && (heard[i].flags & flags) == flags)
            return true;
    }
    return false;
}

void test_events()
{
    mkdir("/tmp/events", 0755);
    mkdir("/tmp/elsewhere", 0755);
    int const stream = open("/dev/fsevents", O_RDONLY);
    CHECK(stream >= 0);
    char const* const paths[] = { "/tmp/events" };
    vulture_fsevents_watch watch {};
    watch.since = FSEVENTS_SINCE_NOW;
    watch.paths = paths;
    watch.path_count = 1;
    CHECK(ioctl(stream, FSEVENTS_WATCH, &watch) == 0);
    unsigned long long latest = 0;
    CHECK(ioctl(stream, FSEVENTS_LATEST, &latest) == 0 && latest > 0);
    unsigned long long const started_at = latest;

    put("/tmp/events/file", "text");
    mkdir("/tmp/events/directory", 0755);
    chmod("/tmp/events/file", 0600);
    chown("/tmp/events/file", someone_else, someone_else);
    setxattr("/tmp/events/file", "org.example.event", "1", 1, 0, 0);
    symlink("file", "/tmp/events/link");
    put("/tmp/elsewhere/quiet", "not for them");
    rename("/tmp/events/file", "/tmp/events/moved");
    int const appending = open("/tmp/events/moved", O_WRONLY | O_APPEND);
    (void)write(appending, "more", 4);
    close(appending);
    clonefile("/tmp/events/moved", "/tmp/events/clone", 0);
    unlink("/tmp/events/moved");
    rmdir("/tmp/events/directory");

    static Heard heard[256];
    int const count = listen_for_events(stream, heard, 256);
    CHECK(count > 0);
    CHECK(was_heard(heard, count, "/private/tmp/events/file", FSE_ITEM_CREATED | FSE_ITEM_IS_FILE));
    CHECK(was_heard(heard, count, "/private/tmp/events/file", FSE_ITEM_METADATA_CHANGED));
    CHECK(was_heard(heard, count, "/private/tmp/events/file", FSE_ITEM_OWNER_CHANGED));
    CHECK(was_heard(heard, count, "/private/tmp/events/file", FSE_ITEM_XATTR_CHANGED));
    CHECK(was_heard(heard, count, "/private/tmp/events/directory", FSE_ITEM_CREATED | FSE_ITEM_IS_DIRECTORY));
    CHECK(was_heard(heard, count, "/private/tmp/events/link", FSE_ITEM_CREATED | FSE_ITEM_IS_SYMBOLIC_LINK));
    CHECK(was_heard(heard, count, "/private/tmp/events/file", FSE_ITEM_RENAMED));
    CHECK(was_heard(heard, count, "/private/tmp/events/moved", FSE_ITEM_RENAMED));
    CHECK(was_heard(heard, count, "/private/tmp/events/moved", FSE_ITEM_MODIFIED));
    CHECK(was_heard(heard, count, "/private/tmp/events/clone", FSE_ITEM_CREATED | FSE_ITEM_CLONED));
    CHECK(was_heard(heard, count, "/private/tmp/events/moved", FSE_ITEM_REMOVED | FSE_ITEM_IS_FILE));
    CHECK(was_heard(heard, count, "/private/tmp/events/directory", FSE_ITEM_REMOVED | FSE_ITEM_IS_DIRECTORY));
    CHECK(!was_heard(heard, count, "/private/tmp/elsewhere/quiet", 0));
    for (int i = 1; i < count; ++i)
        CHECK(heard[i].id > heard[i - 1].id);

    // And all that again, from when it started: it says when it has caught up.
    int const again = open("/dev/fsevents", O_RDONLY);
    watch.since = started_at;
    CHECK(ioctl(again, FSEVENTS_WATCH, &watch) == 0);
    static Heard replay[256];
    int const replayed = listen_for_events(again, replay, 256);
    CHECK(was_heard(replay, replayed, "/private/tmp/events/file", FSE_ITEM_CREATED));
    CHECK(was_heard(replay, replayed, "/", FSE_HISTORY_DONE));
    CHECK(replayed == count + 1);
    close(again);

    // What they can't get to isn't for them to hear about.
    mkdir("/tmp/events/private", 0700);
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t const listener = fork();
    if (listener == 0) {
        setgid(someone_else);
        setuid(someone_else);
        int const theirs = open("/dev/fsevents", O_RDONLY);
        char const* const all[] = { "/tmp/events" };
        vulture_fsevents_watch mine {};
        mine.since = FSEVENTS_SINCE_NOW;
        mine.paths = all;
        mine.path_count = 1;
        if (ioctl(theirs, FSEVENTS_WATCH, &mine) != 0)
            _exit(2);
        (void)write(ready[1], "x", 1);
        static Heard theirs_heard[16];
        int const heard_count = listen_for_events(theirs, theirs_heard, 16);
        bool const told_of_private = was_heard(theirs_heard, heard_count, "/private/tmp/events/private/hidden", 0);
        bool const told_of_public = was_heard(theirs_heard, heard_count, "/private/tmp/events/public", 0);
        fprintf(stderr, "data-volume: someone else heard of %d things, of which the private one %d, the public one %d\n", heard_count, told_of_private, told_of_public);
        _exit(!told_of_private && told_of_public ? 0 : 1);
    }
    char byte = 0;
    CHECK(read(ready[0], &byte, 1) == 1);
    put("/tmp/events/private/hidden", "x");
    put("/tmp/events/public", "x");
    int listener_status = 0;
    waitpid(listener, &listener_status, 0);
    CHECK(WIFEXITED(listener_status) && WEXITSTATUS(listener_status) == 0);
    close(ready[0]);
    close(ready[1]);
    close(stream);
    system("rm -rf /tmp/events /tmp/elsewhere");
}

blkcnt_t blocks_of(char const* path)
{
    struct stat status;
    return stat(path, &status) == 0 ? status.st_blocks : -1;
}

void test_compression()
{
    char const* const path = "/tmp/compressible";
    size_t const size = 8 * 1024 * 1024 + 123;
    auto* bytes = static_cast<char*>(malloc(size));
    auto* seen = static_cast<char*>(malloc(size));
    for (size_t i = 0; i < size; ++i)
        bytes[i] = "the same few words, over and over "[i % 34];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(write(fd, bytes, size) == static_cast<ssize_t>(size));
    fsync(fd);
    close(fd);
    blkcnt_t const whole = blocks_of(path);
    CHECK(whole >= static_cast<blkcnt_t>(size / 512));
    CHECK(chflags(path, UF_COMPRESSED) == 0);
    struct stat status;
    CHECK(stat(path, &status) == 0 && (status.st_flags & UF_COMPRESSED) && status.st_size == static_cast<off_t>(size));
    blkcnt_t const compressed = blocks_of(path);
    fprintf(stderr, "data-volume: %ld blocks compressed to %ld\n", static_cast<long>(whole), static_cast<long>(compressed));
    CHECK(compressed < whole / 8);
    auto read_all = [&] {
        int const reader = open(path, O_RDONLY);
        ssize_t const got = read(reader, seen, size);
        close(reader);
        return got == static_cast<ssize_t>(size);
    };
    CHECK(read_all() && memcmp(seen, bytes, size) == 0);
    fd = open(path, O_WRONLY);
    CHECK(pwrite(fd, "CHANGED", 7, 5 * 1024 * 1024) == 7);
    close(fd);
    memcpy(bytes + 5 * 1024 * 1024, "CHANGED", 7);
    CHECK(read_all() && memcmp(seen, bytes, size) == 0);
    CHECK(chflags(path, 0) == 0);
    CHECK(read_all() && memcmp(seen, bytes, size) == 0);
    CHECK(blocks_of(path) >= static_cast<blkcnt_t>(size / 512));
    unlink(path);
    free(bytes);
    free(seen);
}

long milliseconds_since(timespec const& start)
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
}

// A file this many MiB, cloned: how long that takes, and that it needs no more room.
void test_instant_clone(size_t mebibytes)
{
    char const* const path = "/tmp/big";
    char const* const copy = "/tmp/big-clone";
    size_t const chunk_size = 1024 * 1024;
    auto* chunk = static_cast<char*>(malloc(chunk_size));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (size_t i = 0; i < mebibytes; ++i) {
        memset(chunk, static_cast<int>(i), chunk_size);
        CHECK(write(fd, chunk, chunk_size) == static_cast<ssize_t>(chunk_size));
    }
    fsync(fd);
    close(fd);
    timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(clonefile(path, copy, 0) == 0);
    long const took = milliseconds_since(start);
    fprintf(stderr, "data-volume: cloned %zu MiB in %ld ms\n", mebibytes, took);
    if (mebibytes >= 1024)
        printf("budget: clone 1 GiB: %ld ms\n", took);
    CHECK(took < 50);
    // Its blocks are the same blocks, until one or the other changes.
    fd = open(copy, O_WRONLY);
    memset(chunk, 0xee, chunk_size);
    CHECK(pwrite(fd, chunk, chunk_size, 0) == static_cast<ssize_t>(chunk_size));
    close(fd);
    fd = open(path, O_RDONLY);
    CHECK(read(fd, chunk, chunk_size) == static_cast<ssize_t>(chunk_size));
    CHECK(static_cast<unsigned char>(chunk[0]) == 0);
    close(fd);
    unlink(path);
    unlink(copy);
    free(chunk);
}

}

int main(int argc, char** argv)
{
    // What's to go through volume-test.sh, in one more file on the disk, as big as there's room for.
    if (argc == 2 && strcmp(argv[1], "clone-a-gibibyte") == 0) {
        test_instant_clone(1024);
        return finish();
    }
    test_layout();
    test_access_control_lists();
    test_snapshots();
    test_events();
    test_compression();
    test_instant_clone(64);
    return finish();
}
