/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/disk.h>
#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// What's plugged in, what's on it, and what can be done about that, for diskarbitrationd, diskutil, hdiutil and Disks.
// Asking is for anyone; changing things goes through diskarbitrationd, which decides who may.
namespace DiskManagement {

struct Disk {
    vulture_disk_info info;
    // Where it's mounted, if it is.
    String mount_point;
    [[nodiscard]] bool is_whole() const { return info.partition_number == 0; }
    [[nodiscard]] StringView name() const { return { info.name, __builtin_strlen(info.name) }; }
    [[nodiscard]] StringView whole() const { return { info.whole, __builtin_strlen(info.whole) }; }
    [[nodiscard]] StringView label() const { return { info.label, __builtin_strlen(info.label) }; }
    [[nodiscard]] StringView content() const { return { info.content, __builtin_strlen(info.content) }; }
    [[nodiscard]] u64 size() const { return info.sectors * info.sector_size; }
};

// In order: disk0, disk0s1, disk0s2, disk1...
[[nodiscard]] ErrorOr<Vector<Disk>> list_disks();
[[nodiscard]] ErrorOr<Disk> find_disk(StringView name);

// ---- Done with the privileges they need: diskarbitrationd does these, for those who ask it.

// At /Volumes/ whatever it's called, or a name of its own. Where that was.
[[nodiscard]] ErrorOr<String> mount_disk(StringView name, bool read_only = false);
ErrorOr<void> unmount_disk(StringView name, bool force = false);
// All of the disk it's on: unmounted, and then out.
ErrorOr<void> eject_disk(StringView name);
// A new, empty volume, called `label`: format is msdos, exfat, fat16, fat32 or fat12.
ErrorOr<void> erase_volume(StringView name, StringView format, StringView label);
// All of it: one partition taking up all the room, in a new table.
ErrorOr<void> erase_disk(StringView whole, StringView format, StringView label);
// What's to go on a disk, in order. No sectors means whatever's left.
struct NewPartition {
    String format;
    String label;
    u64 sectors { 0 };
};
// All of it, in a new table, each formatted and mounted.
ErrorOr<void> partition_disk(StringView whole, Span<NewPartition const>);

using LineCallback = Function<void(StringView line)>;
// Each thing it finds and each thing it does goes to `say`. Whether it's all right.
[[nodiscard]] ErrorOr<bool> verify_volume(StringView name, LineCallback say);
[[nodiscard]] ErrorOr<bool> repair_volume(StringView name, LineCallback say);
// As a disk. Its name.
[[nodiscard]] ErrorOr<String> attach_image(StringView path, bool read_only);
// Which system the machine starts from next time: its partition's identity goes on the EFI System Partition.
ErrorOr<void> set_startup_disk(StringView name);

// ---- Asking diskarbitrationd.

constexpr StringView socket_path = "/private/var/run/diskarbitrationd.socket"_sv;

// One request, on a line: what it says back, as it goes, goes to `say`. An error for "error: what's wrong".
ErrorOr<void> request(StringView line, LineCallback say = {});

// What these sizes are, as a Mac says them: in thousands. "4.0 GB".
[[nodiscard]] String size_text(u64 bytes);

}
