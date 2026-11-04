/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FAT32.h"
#include "Image.h"
#include "Populate.h"
#include <LibVultureFS/Volume.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// vinstall DISK: erases /dev/DISK, and makes it what this was started from, as mkimage makes a disk on the
// machine that builds this: an EFI System Partition with what's on this one, the system image that's
// there too on a partition of its own, and an empty data volume. What it says is for installd: "P percent" as it goes,
// and last "S", or "F why".

using namespace VultureFS;

namespace {

constexpr u64 esp_sectors = 300 * MiB / Tools::sector_size;

#if defined(__x86_64__)
constexpr StringView other_architecture = "aarch64"_sv;
constexpr StringView native_archive = "System/Library/Kernels/vulture.bootarchive.x86_64"_sv;
constexpr StringView other_loader = "BOOTAA64.EFI"_sv;
#else
constexpr StringView other_architecture = "x86_64"_sv;
constexpr StringView native_archive = "System/Library/Kernels/vulture.bootarchive.aarch64"_sv;
constexpr StringView other_loader = "BOOTX64.EFI"_sv;
#endif

int fail(StringView why)
{
    printf("F %.*s\n", static_cast<int>(why.length()), why.characters_without_null_termination());
    fflush(stdout);
    return 1;
}

int fail_with(StringView what, Error const& error)
{
    StringBuilder text;
    text.appendff("{}: {}", what, error);
    return fail(text.view());
}

void say_progress(u32 percent)
{
    printf("P %u\n", percent);
    fflush(stdout);
}

// Everything on the EFI System Partition, apart from another kind of machine's, and one run's settings.
ErrorOr<void> read_system_partition(StringView directory, StringView relative, Vector<Tools::FATFile>& files)
{
    StringBuilder path;
    path.append(directory);
    if (!relative.is_empty()) {
        path.append('/');
        path.append(relative);
    }
    path.append('\0');
    DIR* listing = opendir(path.view().characters_without_null_termination());
    if (!listing)
        return Error::from_errno(errno);
    while (auto* entry = readdir(listing)) {
        StringView const name(entry->d_name, strlen(entry->d_name));
        if (name == "."_sv || name == ".."_sv || name == "boot-args"_sv || name == "Logs"_sv || name == other_loader || name.ends_with(other_architecture))
            continue;
        StringBuilder inner;
        if (!relative.is_empty()) {
            inner.append(relative);
            inner.append('/');
        }
        inner.append(name);
        StringBuilder full;
        full.append(directory);
        full.append('/');
        full.append(inner.view());
        full.append('\0');
        struct stat status;
        if (stat(full.view().characters_without_null_termination(), &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode)) {
            TRY(read_system_partition(directory, inner.view(), files));
            continue;
        }
        auto data = TRY(Tools::read_file(full.view().characters_without_null_termination()));
        files.append(Tools::FATFile { TRY(String::from_view(inner.view())), move(data) });
    }
    closedir(listing);
    return {};
}

}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 2)
        return fail("Usage: vinstall DISK"_sv);
    if (getuid() != 0)
        return fail("Only root can do this."_sv);

    StringBuilder device;
    device.append("/dev/"_sv);
    device.append(StringView(argv[1], strlen(argv[1])));
    device.append('\0');

    constexpr StringView esp = "/Volumes/EFI"_sv;
    Vector<Tools::FATFile> files;
    if (auto result = read_system_partition(esp, ""_sv, files); result.is_error())
        return fail_with("Couldn't read the EFI System Partition"_sv, result.error());
    // (New ones: the logs of what this started from aren't the new disk's.)
    Tools::add_log_files(files);
    say_progress(5);

    // The system's files: its own archive, on a disk with more than one.
    Vector<u8> archive;
    bool found_archive = false;
    for (auto const& file : files) {
        if (file.path.view() == native_archive || file.path.view() == "System/Library/Kernels/vulture.bootarchive"_sv) {
            archive = file.data;
            found_archive = true;
            if (file.path.view() == native_archive)
                break;
        }
    }
    if (!found_archive)
        return fail("There's no system on what this started from."_sv);

    FILE* image = fopen(device.view().characters_without_null_termination(), "r+b");
    if (!image)
        return fail(StringView(strerror(errno), strlen(strerror(errno))));
    fseek(image, 0, SEEK_END);
    u64 const size = static_cast<u64>(ftell(image));
    if (size < 1024 * MiB) {
        fclose(image);
        return fail("That disk is too small: it takes at least 1 GB."_sv);
    }

    u64 const system_first = Tools::partition_start_sector + esp_sectors;
    u64 const after_esp = Tools::last_usable_sector(size) - system_first + 1;
    auto system_sectors = Tools::system_partition_sectors(archive.size(), after_esp);
    if (system_sectors.is_error())
        return fail_with("That disk won't do"_sv, system_sectors.error());
    u64 const data_first = system_first + system_sectors.value();
    u64 const data_count = (after_esp - system_sectors.value()) / 8 * 8;
    Tools::PartitionSpec const partitions[] = {
        { Tools::efi_system_partition_type, "EFI System Partition"_sv, Tools::partition_start_sector, esp_sectors },
        { VultureFS::system_partition_type, Tools::system_volume_name, system_first, system_sectors.value() },
        { VultureFS::data_partition_type, Tools::data_volume_name, data_first, data_count },
    };
    if (auto result = Tools::write_partition_table(image, size, partitions); result.is_error())
        return fail_with("Couldn't write the partition table"_sv, result.error());
    // What this started from was for a Raspberry Pi: so is what it makes.
    for (auto const& file : files) {
        if (file.path.view() != "kernel8.img"_sv)
            continue;
        if (auto result = Tools::add_partition_to_mbr(image, Tools::partition_start_sector, esp_sectors); result.is_error())
            return fail_with("Couldn't write the partition table"_sv, result.error());
    }
    say_progress(10);
    if (auto result = Tools::write_fat32(image, Tools::partition_start_sector, esp_sectors, "EFI"_sv, files); result.is_error())
        return fail_with("Couldn't write the EFI System Partition"_sv, result.error());
    say_progress(40);

    if (auto result = Tools::write_system(image, system_first, system_sectors.value(), { archive.begin(), archive.size() }); result.is_error())
        return fail_with("Couldn't write the system"_sv, result.error());
    say_progress(90);
    if (auto result = Tools::format_data(image, data_first, data_count, Tools::data_volume_name); result.is_error())
        return fail_with("Couldn't format the disk"_sv, result.error());
    fflush(image);
    fsync(fileno(image));
    fclose(image);
    say_progress(100);
    printf("S\n");
    fflush(stdout);
    return 0;
}
