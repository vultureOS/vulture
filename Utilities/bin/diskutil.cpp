/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibDiskManagement/Disks.h>
#include <unistd.h>

// diskutil: as on a Mac, for what's the same here.

using namespace DiskManagement;

namespace {

[[noreturn]] void usage()
{
    fprintf(stderr, "Disk Utility Tool\n"
                    "Utility to manage local disks and volumes\n"
                    "Most commands require an administrator or root user\n"
                    "\n"
                    "WARNING: Most destructive operations are not prompted\n"
                    "\n"
                    "Usage:  diskutil [quiet] <verb> <options>, where <verb> is as follows:\n"
                    "\n"
                    "     list                 (List the partitions of a disk)\n"
                    "     info[rmation]        (Get information on a specific disk or partition)\n"
                    "     listFilesystems      (List file systems available for formatting)\n"
                    "\n"
                    "     u[n]mount            (Unmount a single volume)\n"
                    "     unmountDisk          (Unmount an entire disk (all volumes))\n"
                    "     eject                (Eject a disk)\n"
                    "     mount                (Mount a single volume)\n"
                    "     mountDisk            (Mount an entire disk (all mountable volumes))\n"
                    "\n"
                    "     verifyVolume         (Verify the file system data structures of a volume)\n"
                    "     repairVolume         (Repair the file system data structures of a volume)\n"
                    "\n"
                    "     eraseDisk            (Erase an existing disk, removing all volumes)\n"
                    "     eraseVolume          (Erase an existing volume)\n"
                    "     partitionDisk        ((re)Partition a disk, removing all volumes)\n"
                    "\n"
                    "diskutil <verb> with no options will provide help on that verb\n");
    exit(1);
}

StringView partition_kind(Disk const& disk)
{
    auto const content = disk.content();
    if (disk.is_whole()) {
        // A volume with no partition table, as a stick may be; or else what it's divided by.
        if (content == "msdos"_sv)
            return "DOS_FAT"_sv;
        if (content == "exfat"_sv)
            return "ExFAT"_sv;
        if (content == "cd9660"_sv)
            return "CD_ROM_Mode_1"_sv;
        return "GUID_partition_scheme"_sv;
    }
    if (content == "msdos"_sv)
        return "Microsoft Basic Data"_sv;
    if (content == "exfat"_sv)
        return "Microsoft Basic Data"_sv;
    if (content == "cd9660"_sv)
        return "ISO 9660"_sv;
    if (content == "vulture-system"_sv)
        return "vultureFS System"_sv;
    if (content == "vulture-data"_sv)
        return "vultureFS Data"_sv;
    static constexpr u8 efi[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
    if (memcmp(disk.info.partition_type, efi, 16) == 0)
        return "EFI"_sv;
    return "Unknown"_sv;
}

int list_all(char const* only)
{
    auto const disks = list_disks();
    if (disks.is_error()) {
        fprintf(stderr, "Could not list disks: %s\n", strerror(disks.error().code()));
        return 1;
    }
    bool first = true;
    for (auto const& disk : disks.value()) {
        if (!disk.is_whole())
            continue;
        if (only && strcmp(only, disk.info.name) != 0 && strcmp(only + (strncmp(only, "/dev/", 5) == 0 ? 5 : 0), disk.info.name) != 0)
            continue;
        if (!first)
            putchar('\n');
        first = false;
        bool const removable = disk.info.removable;
        bool const image = strcmp(disk.info.bus, "disk image") == 0;
        printf("/dev/%s (%s, %s):\n", disk.info.name, removable && !image ? "external" : image ? "disk image" : "internal", "physical");
        printf("   #:                       TYPE NAME                    SIZE       IDENTIFIER\n");
        int number = 0;
        auto const row = [&](Disk const& one) {
            auto const size = size_text(one.size());
            auto const kind = partition_kind(one);
            char size_field[16];
            snprintf(size_field, sizeof(size_field), "%s%s", one.is_whole() ? "*" : "", size.characters());
            char name_field[32];
            StringView const label = one.label();
            if (label.length() > 23)
                snprintf(name_field, sizeof(name_field), "%.20s...", label.characters_without_null_termination());
            else
                snprintf(name_field, sizeof(name_field), "%.*s", static_cast<int>(label.length()), label.characters_without_null_termination());
            printf("%4d:%27.*s %-23s %10s  %s\n", number++, static_cast<int>(kind.length()), kind.characters_without_null_termination(), name_field, size_field, one.info.name);
        };
        row(disk);
        for (auto const& partition : disks.value()) {
            if (!partition.is_whole() && partition.whole() == disk.name())
                row(partition);
        }
    }
    return 0;
}

void field(char const* name, char const* value)
{
    printf("   %-27s%s\n", name, value);
}

int show_information(char const* name)
{
    auto const disk = find_disk({ name, strlen(name) });
    if (disk.is_error()) {
        fprintf(stderr, "Could not find disk: %s\n", name);
        return 1;
    }
    auto const& one = disk.value();
    char node[64];
    snprintf(node, sizeof(node), "/dev/%s", one.info.name);
    field("Device Identifier:", one.info.name);
    field("Device Node:", node);
    field("Whole:", one.is_whole() ? "Yes" : "No");
    field("Part of Whole:", one.info.whole);
    putchar('\n');
    bool const has_volume = !one.content().is_empty();
    field("Volume Name:", has_volume ? (one.label().is_empty() ? "" : one.info.label) : "Not applicable (no file system)");
    field("Mounted:", has_volume ? (one.mount_point.is_empty() ? "No" : "Yes") : "Not applicable (no file system)");
    if (!one.mount_point.is_empty())
        field("Mount Point:", one.mount_point.characters());
    field("File System:", has_volume ? one.info.content : "None");
    putchar('\n');
    auto const kind = partition_kind(one);
    char kind_text[64];
    snprintf(kind_text, sizeof(kind_text), "%.*s", static_cast<int>(kind.length()), kind.characters_without_null_termination());
    field(one.is_whole() ? "Content (IOContent):" : "Partition Type:", kind_text);
    field("Protocol:", one.info.bus);
    putchar('\n');
    char size[96];
    auto const size_words = size_text(one.size());
    snprintf(size, sizeof(size), "%s (%llu Bytes) (exactly %llu 512-Byte-Units)", size_words.characters(), static_cast<unsigned long long>(one.size()), static_cast<unsigned long long>(one.size() / 512));
    field("Disk Size:", size);
    char block[32];
    snprintf(block, sizeof(block), "%u Bytes", one.info.sector_size);
    field("Device Block Size:", block);
    putchar('\n');
    field("Media Read-Only:", one.info.read_only ? "Yes" : "No");
    putchar('\n');
    field("Device Location:", one.info.removable ? "External" : "Internal");
    field("Removable Media:", one.info.removable ? "Removable" : "Fixed");
    return 0;
}

int run(StringView line, char const* doing, char const* done)
{
    if (doing)
        printf("%s\n", doing);
    fflush(stdout);
    auto const result = request(line, [](StringView text) {
        printf("%.*s\n", static_cast<int>(text.length()), text.characters_without_null_termination());
    });
    if (result.is_error()) {
        fprintf(stderr, "%s\n", strerror(result.error().code()));
        return 1;
    }
    if (done)
        printf("%s\n", done);
    return 0;
}

String joined_request(char const* verb, char const* name, char const* extra = nullptr, char const* last = nullptr)
{
    StringBuilder line;
    line.appendff("{} {}", StringView { verb, strlen(verb) }, StringView { name, strlen(name) });
    if (extra)
        line.appendff(" {}", StringView { extra, strlen(extra) });
    if (last)
        line.appendff(" {}", StringView { last, strlen(last) });
    return MUST(line.to_string());
}

// As diskutil says them: "ExFAT", "MS-DOS FAT32".
char const* format_from(char const* format)
{
    if (strcasecmp(format, "exfat") == 0)
        return "exfat";
    if (strcasecmp(format, "ms-dos") == 0 || strcasecmp(format, "msdos") == 0 || strcasecmp(format, "fat") == 0)
        return "msdos";
    if (strcasecmp(format, "ms-dos fat12") == 0 || strcasecmp(format, "fat12") == 0)
        return "fat12";
    if (strcasecmp(format, "ms-dos fat16") == 0 || strcasecmp(format, "fat16") == 0)
        return "fat16";
    if (strcasecmp(format, "ms-dos fat32") == 0 || strcasecmp(format, "fat32") == 0)
        return "fat32";
    return nullptr;
}

}

int main(int argc, char** argv)
{
    int first = 1;
    if (first < argc && strcmp(argv[first], "quiet") == 0)
        ++first;
    if (first >= argc)
        usage();
    char const* const verb = argv[first];
    char const* const a = first + 1 < argc ? argv[first + 1] : nullptr;
    char const* const b = first + 2 < argc ? argv[first + 2] : nullptr;
    char const* const c = first + 3 < argc ? argv[first + 3] : nullptr;
    auto const named = [&](char const* what) {
        return strcasecmp(verb, what) == 0;
    };
    if (named("list"))
        return list_all(a);
    if (named("info") || named("information")) {
        if (!a) {
            fprintf(stderr, "Usage:  diskutil info[rmation] [-plist] MountPoint|DiskIdentifier|DeviceNode|\"all\"\n");
            return 1;
        }
        return show_information(a);
    }
    if (named("listFilesystems")) {
        printf("Formattable file systems\n\nThese file system personalities can be used for erasing and partitioning.\n"
               "When specifying a personality as a parameter to a verb, case is not considered.\n\n");
        printf("-------------------------------------------------------------------------------\n");
        printf("PERSONALITY                     USER VISIBLE NAME\n");
        printf("-------------------------------------------------------------------------------\n");
        printf("ExFAT                           ExFAT\n");
        printf("MS-DOS                          MS-DOS (FAT)\n");
        printf("MS-DOS FAT12                    MS-DOS (FAT12)\n");
        printf("MS-DOS FAT16                    MS-DOS (FAT16)\n");
        printf("MS-DOS FAT32                    MS-DOS (FAT32)\n");
        return 0;
    }
    if (named("unmount") || named("umount")) {
        bool const force = a && strcmp(a, "force") == 0;
        char const* const name = force ? b : a;
        if (!name) {
            fprintf(stderr, "Usage:  diskutil u[n]mount [force] MountPoint|DiskIdentifier|DeviceNode\n");
            return 1;
        }
        StringBuilder done;
        done.appendff("Volume {} on {} unmounted", StringView { name, strlen(name) }, StringView { name, strlen(name) });
        return run(joined_request(force ? "unmount-force" : "unmount", name).view(), nullptr, MUST(done.to_string()).characters());
    }
    if (named("mount")) {
        if (!a) {
            fprintf(stderr, "Usage:  diskutil mount [readOnly] [-mountOptions Options] [-mountPoint Path] DiskIdentifier|DeviceNode\n");
            return 1;
        }
        return run(joined_request("mount", a).view(), nullptr, nullptr);
    }
    if (named("unmountDisk") || named("mountDisk")) {
        if (!a) {
            fprintf(stderr, "Usage:  diskutil %s [force] DiskIdentifier|DeviceNode\n", verb);
            return 1;
        }
        auto const whole = find_disk({ a, strlen(a) });
        if (whole.is_error()) {
            fprintf(stderr, "Could not find disk: %s\n", a);
            return 1;
        }
        int status = 0;
        auto const disks = MUST(list_disks());
        for (auto const& disk : disks) {
            if (disk.whole() != whole.value().whole() || disk.content().is_empty())
                continue;
            status |= run(joined_request(named("mountDisk") ? "mount" : "unmount", disk.info.name).view(), nullptr, nullptr);
        }
        return status;
    }
    if (named("eject")) {
        if (!a) {
            fprintf(stderr, "Usage:  diskutil eject MountPoint|DiskIdentifier|DeviceNode\n");
            return 1;
        }
        StringBuilder done;
        done.appendff("Disk {} ejected", StringView { a, strlen(a) });
        return run(joined_request("eject", a).view(), nullptr, MUST(done.to_string()).characters());
    }
    if (named("verifyVolume") || named("repairVolume")) {
        if (!a) {
            fprintf(stderr, "Usage:  diskutil %s MountPoint|DiskIdentifier|DeviceNode\n", verb);
            return 1;
        }
        bool const repairing = named("repairVolume");
        StringBuilder started;
        started.appendff("Started file system {} on {}", repairing ? "repair"_sv : "verification"_sv, StringView { a, strlen(a) });
        StringBuilder finished;
        finished.appendff("Finished file system {} on {}", repairing ? "repair"_sv : "verification"_sv, StringView { a, strlen(a) });
        bool damaged = false;
        printf("%s\n", MUST(started.to_string()).characters());
        fflush(stdout);
        auto const result = request(joined_request(repairing ? "repair" : "verify", a).view(), [&](StringView text) {
            if (text == "damaged"_sv)
                damaged = true;
            else if (text != "clean"_sv)
                printf("%.*s\n", static_cast<int>(text.length()), text.characters_without_null_termination());
        });
        if (result.is_error()) {
            fprintf(stderr, "%s\n", strerror(result.error().code()));
            return 1;
        }
        printf("%s\n", MUST(finished.to_string()).characters());
        if (damaged) {
            printf("File system check exit code is 8\n");
            return 1;
        }
        printf("File system check exit code is 0\n");
        return 0;
    }
    if (named("eraseVolume") || named("eraseDisk")) {
        bool const disk_too = named("eraseDisk");
        if (!a || !b || !c) {
            fprintf(stderr, disk_too ? "Usage:  diskutil eraseDisk format name [APM[Format]|MBR[Format]|GPT[Format]] MountPoint|DiskIdentifier|DeviceNode\n" : "Usage:  diskutil eraseVolume format name MountPoint|DiskIdentifier|DeviceNode\n");
            return 1;
        }
        // For a whole disk, a scheme may come before the disk. And it goes by its own name from here, however it was given.
        char const* const given = argv[argc - 1];
        auto const found = find_disk({ given, strlen(given) });
        if (found.is_error()) {
            fprintf(stderr, "Could not find disk: %s\n", given);
            return 1;
        }
        char const* const device = found.value().info.name;
        char const* const format = format_from(a);
        if (!format) {
            fprintf(stderr, "%s does not appear to be a valid file system format\n", a);
            return 1;
        }
        StringBuilder line;
        line.appendff("{} {} {} {}", disk_too ? "erase-disk"_sv : "erase-volume"_sv, StringView { device, strlen(device) }, StringView { format, strlen(format) }, StringView { b, strlen(b) });
        StringBuilder started;
        started.appendff("Started erase on {}", StringView { device, strlen(device) });
        StringBuilder finished;
        finished.appendff("Finished erase on {}", StringView { device, strlen(device) });
        return run(MUST(line.to_string()).view(), MUST(started.to_string()).characters(), MUST(finished.to_string()).characters());
    }
    if (named("partitionDisk")) {
        // diskutil partitionDisk DISK [COUNT] [SCHEME] FORMAT NAME SIZE ...: each size a percentage, or R for what's left.
        if (!a) {
            fprintf(stderr, "Usage:  diskutil partitionDisk MountPoint|DiskIdentifier|DeviceNode [numberOfPartitions] [APM[Format]|MBR[Format]|GPT[Format]]\n"
                            "        [part1Format part1Name part1Size part2Format part2Name part2Size ...]\n");
            return 1;
        }
        auto const whole = find_disk({ a, strlen(a) });
        if (whole.is_error()) {
            fprintf(stderr, "Could not find disk: %s\n", a);
            return 1;
        }
        int at = first + 2;
        if (at < argc && argv[at][0] >= '0' && argv[at][0] <= '9')
            ++at;
        if (at < argc && (strncasecmp(argv[at], "GPT", 3) == 0 || strncasecmp(argv[at], "MBR", 3) == 0 || strncasecmp(argv[at], "APM", 3) == 0))
            ++at;
        StringBuilder line;
        line.appendff("partition {} ", StringView { whole.value().info.whole, strlen(whole.value().info.whole) });
        bool any = false;
        for (; at + 2 < argc; at += 3) {
            char const* const format = format_from(argv[at]);
            if (!format) {
                fprintf(stderr, "%s does not appear to be a valid file system format\n", argv[at]);
                return 1;
            }
            u64 sectors = 0;
            if (strcasecmp(argv[at + 2], "R") != 0)
                sectors = whole.value().info.sectors * strtoull(argv[at + 2], nullptr, 10) / 100;
            if (any)
                line.append('\t');
            line.appendff("{} {} {}", StringView { format, strlen(format) }, sectors, StringView { argv[at + 1], strlen(argv[at + 1]) });
            any = true;
        }
        if (!any) {
            fprintf(stderr, "Usage:  diskutil partitionDisk DISK GPT FORMAT NAME SIZE ...\n");
            return 1;
        }
        StringBuilder started;
        started.appendff("Started partitioning on {}", StringView { a, strlen(a) });
        StringBuilder finished;
        finished.appendff("Finished partitioning on {}", StringView { a, strlen(a) });
        return run(MUST(line.to_string()).view(), MUST(started.to_string()).characters(), MUST(finished.to_string()).characters());
    }
    usage();
}
