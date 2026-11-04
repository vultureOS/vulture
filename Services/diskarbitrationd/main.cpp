/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/sysctl.h>
#include <LibDiskManagement/Disks.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>

// What happens when a disk comes or goes: what's on it is mounted under /Volumes, and what was on it goes.
// And whoever's allowed asks here for changes to what's plugged in, a request per connection, a line each way.

using namespace DiskManagement;

namespace {

// Whatever has come is mounted, as long as it has something on it that can be, and isn't the system.
void disk_appeared(StringView name)
{
    auto disk = find_disk(name);
    if (disk.is_error() || disk.value().content().is_empty())
        return;
    auto const content = disk.value().content();
    if (content == "vulture-system"_sv || content == "vulture-data"_sv)
        return;
    // It's only for those already mounted (the start-up disk's own EFI partition) to stay as they are.
    if (!disk.value().mount_point.is_empty())
        return;
    // What a start-up disk has for its firmware is none of anyone's business.
    if (disk.value().label() == "EFI"_sv)
        return;
    auto const mounted = mount_disk(name);
    if (mounted.is_error())
        printf("diskarbitrationd: can't mount %.*s: %s\n", static_cast<int>(name.length()), name.characters_without_null_termination(), strerror(mounted.error().code()));
    else
        printf("diskarbitrationd: %.*s is at %s\n", static_cast<int>(name.length()), name.characters_without_null_termination(), mounted.value().characters());
    fflush(stdout);
}

// It's gone, so whatever was on it has too, whoever was using it.
void disk_disappeared(StringView name)
{
    size_t size = 0;
    if (sysctlbyname("vfs.mounts", nullptr, &size, nullptr, 0) != 0)
        return;
    Vector<vulture_mount_info> mounts;
    if (mounts.try_resize(size / sizeof(vulture_mount_info)).is_error())
        return;
    if (size && sysctlbyname("vfs.mounts", mounts.begin(), &size, nullptr, 0) != 0)
        return;
    for (auto const& mount : mounts) {
        // Something else may have the name by now: this hears of a disk going after whatever it was doing, which may have
        // been partitioning it.
        if (StringView { mount.device, strlen(mount.device) } != name || !mount.device_gone)
            continue;
        printf("diskarbitrationd: %s came out while it was in use\n", mount.point);
        fflush(stdout);
        unmount(mount.point, MNT_FORCE);
        rmdir(mount.point);
    }
}

bool may_change_disks(int client)
{
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(client, &uid, &gid) != 0)
        return false;
    if (uid == 0)
        return true;
    // Whoever is an administrator.
    auto const* user = getpwuid(uid);
    auto const* admin = getgrnam("admin");
    if (!user || !admin)
        return false;
    if (user->pw_gid == admin->gr_gid)
        return true;
    for (char** member = admin->gr_mem; member && *member; ++member) {
        if (strcmp(*member, user->pw_name) == 0)
            return true;
    }
    return false;
}

void say(int client, StringView line)
{
    (void)write(client, line.characters_without_null_termination(), line.length());
    (void)write(client, "\n", 1);
}

void finish(int client, ErrorOr<void> const& result)
{
    if (!result.is_error()) {
        say(client, "ok"_sv);
        return;
    }
    char text[32];
    snprintf(text, sizeof(text), "errno: %d", result.error().code());
    say(client, { text, strlen(text) });
}

// "mount NAME", "unmount NAME", "unmount-force NAME", "eject NAME", "verify NAME", "repair NAME", "set-startup NAME",
// "attach PATH", "attach-read-only PATH": a name goes on to the end of the line, as it may have spaces in it.
// "erase-volume DISK FORMAT LABEL", "erase-disk DISK FORMAT LABEL", and
// "partition DISK FORMAT SECTORS LABEL" with a tab and another of the last three, for each.
void handle(int client, char* line)
{
    char* rest = nullptr;
    char const* const command = strtok_r(line, " ", &rest);
    if (!command) {
        finish(client, Error::from_errno(EINVAL));
        return;
    }
    if (!may_change_disks(client)) {
        finish(client, Error::from_errno(EPERM));
        return;
    }
    auto const is = [&](char const* what) { return strcmp(command, what) == 0; };
    auto const word = [&]() -> StringView {
        char* const taken = strtok_r(nullptr, " ", &rest);
        return taken ? StringView { taken, strlen(taken) } : StringView {};
    };
    auto const remainder = [&]() -> StringView { return { rest ? rest : "", rest ? strlen(rest) : 0 }; };
    auto const tell = [&](StringView text) { say(client, text); };
    if (is("erase-volume") || is("erase-disk")) {
        StringView const name = word();
        StringView const format = word();
        StringView const label = remainder();
        finish(client, is("erase-volume") ? erase_volume(name, format, label) : erase_disk(name, format, label));
        return;
    }
    if (is("partition")) {
        StringView const name = word();
        Vector<NewPartition> wanted;
        char* remaining = rest;
        char* context = nullptr;
        for (char* part = strtok_r(remaining, "\t", &context); part; part = strtok_r(nullptr, "\t", &context)) {
            char* inner = nullptr;
            char const* const format = strtok_r(part, " ", &inner);
            char const* const sectors = strtok_r(nullptr, " ", &inner);
            if (!format || !sectors) {
                finish(client, Error::from_errno(EINVAL));
                return;
            }
            wanted.append(NewPartition { MUST(String::from_view({ format, strlen(format) })), MUST(String::from_view({ inner ? inner : "", inner ? strlen(inner) : 0 })), strtoull(sectors, nullptr, 10) });
        }
        finish(client, partition_disk(name, { wanted.begin(), wanted.size() }));
        return;
    }
    StringView const name = remainder();
    if (is("mount")) {
        auto const mounted = mount_disk(name);
        if (mounted.is_error()) {
            finish(client, mounted.error());
            return;
        }
        say(client, mounted.value().view());
        finish(client, {});
    } else if (is("unmount") || is("unmount-force")) {
        finish(client, unmount_disk(name, is("unmount-force")));
    } else if (is("eject")) {
        finish(client, eject_disk(name));
    } else if (is("verify") || is("repair")) {
        auto const result = is("verify") ? verify_volume(name, tell) : repair_volume(name, tell);
        if (result.is_error()) {
            finish(client, result.error());
            return;
        }
        say(client, result.value() ? "clean"_sv : "damaged"_sv);
        finish(client, {});
    } else if (is("set-startup")) {
        finish(client, set_startup_disk(name));
    } else if (is("attach") || is("attach-read-only")) {
        auto const attached = attach_image(name, is("attach-read-only"));
        if (attached.is_error()) {
            finish(client, attached.error());
            return;
        }
        say(client, attached.value().view());
        finish(client, {});
    } else {
        finish(client, Error::from_errno(EINVAL));
    }
}

}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    int const events = open("/dev/diskevents", O_RDONLY);
    if (events < 0) {
        perror("diskarbitrationd: /dev/diskevents");
        return 1;
    }
    int const listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%.*s", static_cast<int>(socket_path.length()), socket_path.characters_without_null_termination());
    unlink(address.sun_path);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        perror("diskarbitrationd: listening");
        return 1;
    }
    chmod(address.sun_path, 0666);
    printf("diskarbitrationd: ready\n");

    char pending[1024];
    size_t pending_size = 0;
    for (;;) {
        pollfd fds[2] = { { events, POLLIN, 0 }, { listener, POLLIN, 0 } };
        if (poll(fds, 2, -1) < 0)
            continue;
        if (fds[0].revents & POLLIN) {
            ssize_t const got = read(events, pending + pending_size, sizeof(pending) - pending_size);
            if (got > 0)
                pending_size += static_cast<size_t>(got);
            size_t start = 0;
            for (size_t i = 0; i < pending_size; ++i) {
                if (pending[i] != '\n')
                    continue;
                StringView const line { pending + start, i - start };
                if (line.length() > 1) {
                    auto const name = line.substring_view(1);
                    if (line[0] == '+')
                        disk_appeared(name);
                    else
                        disk_disappeared(name);
                }
                start = i + 1;
            }
            __builtin_memmove(pending, pending + start, pending_size - start);
            pending_size -= start;
        }
        if (fds[1].revents & POLLIN) {
            int const client = accept(listener, nullptr, nullptr);
            if (client >= 0) {
                char line[1024];
                size_t length = 0;
                while (length < sizeof(line) - 1) {
                    ssize_t const got = read(client, line + length, 1);
                    if (got <= 0 || line[length] == '\n')
                        break;
                    ++length;
                }
                line[length] = 0;
                handle(client, line);
                close(client);
            }
        }
    }
}
