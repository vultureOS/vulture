/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/sysctl.h>
#include <LibDiskManagement/Disks.h>
#include <LibForeignFS/Volume.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace DiskManagement {

namespace {

ErrorOr<Vector<vulture_mount_info>> list_mounts()
{
    size_t size = 0;
    if (sysctlbyname("vfs.mounts", nullptr, &size, nullptr, 0) != 0)
        return Error::from_errno(errno);
    Vector<vulture_mount_info> mounts;
    TRY(mounts.try_resize(size / sizeof(vulture_mount_info)));
    if (size && sysctlbyname("vfs.mounts", mounts.begin(), &size, nullptr, 0) != 0)
        return Error::from_errno(errno);
    return mounts;
}

i64 now_in_seconds()
{
    return static_cast<i64>(time(nullptr));
}

// A disk, as what ForeignFS works on.
class DeviceStore final : public ForeignFS::Store {
public:
    static ErrorOr<NonnullOwnPtr<DeviceStore>> open_device(StringView name, bool read_only)
    {
        StringBuilder path;
        path.append("/dev/"_sv);
        path.append(name);
        auto const whole_path = TRY(path.to_string());
        int const fd = open(whole_path.characters(), read_only ? O_RDONLY : O_RDWR);
        if (fd < 0)
            return Error::from_errno(errno);
        u64 sectors = 0;
        if (ioctl(fd, DKIOCGETBLOCKCOUNT, &sectors) != 0) {
            close(fd);
            return Error::from_errno(errno);
        }
        auto* store = new (Base::nothrow) DeviceStore(fd, sectors, read_only);
        if (!store) {
            close(fd);
            return Error::from_errno(ENOMEM);
        }
        return adopt_own(*store);
    }

    ~DeviceStore() override { close(m_fd); }

    [[nodiscard]] u64 sector_count() const override { return m_sectors; }
    [[nodiscard]] bool is_read_only() const override { return m_read_only; }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override
    {
        ssize_t const got = pread(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size));
        if (got != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(got < 0 ? errno : EIO);
        return {};
    }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        ssize_t const wrote = pwrite(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size));
        if (wrote != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(wrote < 0 ? errno : EIO);
        return {};
    }
    ErrorOr<void> flush() override
    {
        if (ioctl(m_fd, DKIOCSYNCHRONIZE, 0) != 0)
            return Error::from_errno(errno);
        return {};
    }

private:
    DeviceStore(int fd, u64 sectors, bool read_only)
        : m_fd(fd)
        , m_sectors(sectors)
        , m_read_only(read_only)
    {
    }

    int m_fd;
    u64 m_sectors;
    bool m_read_only;
};

ErrorOr<bool> check_with(StringView name, DiskManagement::LineCallback& say, bool repair)
{
    auto const disk = TRY(find_disk(name));
    auto const content = disk.content();
    bool const fat = content == "msdos"_sv;
    bool const exfat = content == "exfat"_sv;
    if (!fat && !exfat) {
        say("That isn't something that can be checked from here."_sv);
        return Error::from_errno(ENOTSUP);
    }
    auto store = TRY(DeviceStore::open_device(disk.name(), !repair));
    auto report = fat ? TRY(ForeignFS::check_fat(*store, now_in_seconds)) : TRY(ForeignFS::check_exfat(*store, now_in_seconds));
    if (!report.is_clean() && repair) {
        // Opening it to change something, then putting it away, sets right all that can't have been lost.
        auto volume = TRY(ForeignFS::open_volume(*store, now_in_seconds, false));
        TRY(volume->repair([&](StringView text) { say(text); }));
        report = fat ? TRY(ForeignFS::check_fat(*store, now_in_seconds)) : TRY(ForeignFS::check_exfat(*store, now_in_seconds));
    }
    StringBuilder summary;
    summary.appendff("{} files, {} folders.", report.files, report.directories);
    say(TRY(summary.to_string()).view());
    for (auto const& problem : report.problems)
        say(problem.view());
    return report.is_clean();
}

}

ErrorOr<Vector<Disk>> list_disks()
{
    size_t size = 0;
    if (sysctlbyname("kern.disks", nullptr, &size, nullptr, 0) != 0)
        return Error::from_errno(errno);
    Vector<vulture_disk_info> rows;
    TRY(rows.try_resize(size / sizeof(vulture_disk_info)));
    if (size && sysctlbyname("kern.disks", rows.begin(), &size, nullptr, 0) != 0)
        return Error::from_errno(errno);
    TRY(rows.try_resize(size / sizeof(vulture_disk_info)));
    auto const mounts = TRY(list_mounts());
    Vector<Disk> disks;
    for (auto const& row : rows) {
        Disk disk;
        disk.info = row;
        for (auto const& mount : mounts) {
            if (strcmp(mount.device, row.name) == 0 && !mount.device_gone)
                disk.mount_point = TRY(String::from_view({ mount.point, strlen(mount.point) }));
        }
        TRY(disks.try_append(move(disk)));
    }
    return disks;
}

ErrorOr<Disk> find_disk(StringView name)
{
    if (name.starts_with("/dev/"_sv))
        name = name.substring_view(5);
    auto disks = TRY(list_disks());
    for (auto& disk : disks) {
        if (disk.name() == name)
            return move(disk);
    }
    // By where it's mounted, or what it's called: as diskutil takes them.
    for (auto& disk : disks) {
        if (disk.mount_point.view() == name || (!disk.label().is_empty() && disk.label() == name))
            return move(disk);
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<String> mount_disk(StringView name, bool read_only)
{
    auto const disk = TRY(find_disk(name));
    if (!disk.mount_point.is_empty())
        return String::from_view(disk.mount_point.view());
    if (disk.content().is_empty())
        return Error::from_errno(EINVAL);
    // Something a person would recognise it by.
    StringBuilder base;
    StringView const wanted = disk.label().is_empty() ? "Untitled"_sv : disk.label();
    for (size_t i = 0; i < wanted.length(); ++i)
        base.append(wanted[i] == '/' ? ':' : wanted[i]);
    auto const base_name = TRY(base.to_string());
    (void)mkdir("/Volumes", 0755);
    String place;
    for (int attempt = 0; attempt < 100; ++attempt) {
        StringBuilder path;
        path.appendff("/Volumes/{}", base_name.view());
        if (attempt)
            path.appendff(" {}", attempt);
        place = TRY(path.to_string());
        if (mkdir(place.characters(), 0755) == 0)
            break;
        if (errno != EEXIST)
            return Error::from_errno(errno);
        // Left over, and not in use: that'll do.
        if (rmdir(place.characters()) == 0 && mkdir(place.characters(), 0755) == 0)
            break;
    }
    StringBuilder device;
    device.append("/dev/"_sv);
    device.append(disk.name());
    auto const device_path = TRY(device.to_string());
    if (mount("auto", place.characters(), read_only ? MNT_RDONLY : 0, const_cast<char*>(device_path.characters())) != 0) {
        int const problem = errno;
        rmdir(place.characters());
        return Error::from_errno(problem);
    }
    return place;
}

ErrorOr<void> unmount_disk(StringView name, bool force)
{
    auto const disk = TRY(find_disk(name));
    if (disk.mount_point.is_empty())
        return {};
    if (unmount(disk.mount_point.characters(), force ? MNT_FORCE : 0) != 0)
        return Error::from_errno(errno);
    (void)rmdir(disk.mount_point.characters());
    return {};
}

ErrorOr<void> eject_disk(StringView name)
{
    auto const named = TRY(find_disk(name));
    auto const whole = TRY(String::from_view(named.whole()));
    auto const disks = TRY(list_disks());
    for (auto const& disk : disks) {
        if (disk.whole() == whole.view())
            TRY(unmount_disk(disk.name()));
    }
    StringBuilder path;
    path.appendff("/dev/{}", whole.view());
    auto const whole_path = TRY(path.to_string());
    int const fd = open(whole_path.characters(), O_RDONLY);
    if (fd < 0)
        return Error::from_errno(errno);
    int const result = ioctl(fd, DKIOCEJECT, 0);
    int const problem = errno;
    close(fd);
    if (result != 0)
        return Error::from_errno(problem);
    return {};
}

ErrorOr<void> erase_volume(StringView name, StringView format, StringView label)
{
    auto const disk = TRY(find_disk(name));
    auto const device = TRY(String::from_view(disk.name()));
    TRY(unmount_disk(device.view()));
    {
        auto store = TRY(DeviceStore::open_device(device.view(), false));
        if (format == "exfat"_sv)
            TRY(ForeignFS::format_exfat(*store, label, now_in_seconds));
        else if (format == "msdos"_sv || format == "fat"_sv)
            TRY(ForeignFS::format_fat(*store, label, now_in_seconds));
        else if (format == "fat12"_sv)
            TRY(ForeignFS::format_fat(*store, label, now_in_seconds, 12));
        else if (format == "fat16"_sv)
            TRY(ForeignFS::format_fat(*store, label, now_in_seconds, 16));
        else if (format == "fat32"_sv)
            TRY(ForeignFS::format_fat(*store, label, now_in_seconds, 32));
        else
            return Error::from_errno(EINVAL);
    }
    (void)TRY(mount_disk(device.view()));
    return {};
}

namespace {

// A Microsoft basic data partition, which is what both of these want to be in.
constexpr u8 basic_data_type[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };

u32 crc32_of(u8 const* bytes, size_t size)
{
    u32 crc = 0xffffffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

struct [[gnu::packed]] GPTHeader {
    char signature[8];
    u32 revision;
    u32 header_size;
    u32 header_crc;
    u32 reserved;
    u64 my_lba;
    u64 alternate_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8 disk_guid[16];
    u64 entries_lba;
    u32 entry_count;
    u32 entry_size;
    u32 entries_crc;
};

struct [[gnu::packed]] GPTEntry {
    u8 type[16];
    u8 unique[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
};

void random_guid(u8* output)
{
    (void)getentropy(output, 16);
    output[7] = (output[7] & 0x0f) | 0x40;
    output[8] = (output[8] & 0x3f) | 0x80;
}

ErrorOr<void> write_partition_table(ForeignFS::Store& store, Span<NewPartition const> wanted)
{
    constexpr size_t entries = 128;
    constexpr size_t entry_sectors = entries * sizeof(GPTEntry) / ForeignFS::Store::sector_size;
    u64 const sectors = store.sector_count();
    if (sectors < 4096 || wanted.is_empty() || wanted.size() > entries)
        return Error::from_errno(ENOSPC);
    u64 const last_usable = sectors - 2 - entry_sectors;

    u8 mbr[512] {};
    u32 const last_for_mbr = sectors - 1 > 0xffffffffull ? 0xffffffff : static_cast<u32>(sectors - 1);
    mbr[446 + 2] = 2;
    mbr[446 + 4] = 0xee;
    mbr[446 + 5] = 0xff;
    mbr[446 + 6] = 0xff;
    mbr[446 + 7] = 0xff;
    u32 const one = 1;
    __builtin_memcpy(mbr + 446 + 8, &one, 4);
    __builtin_memcpy(mbr + 446 + 12, &last_for_mbr, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    TRY(store.write_sectors(0, 1, { mbr, sizeof(mbr) }));

    Vector<u8> table;
    TRY(table.try_resize(entries * sizeof(GPTEntry)));
    __builtin_memset(table.begin(), 0, table.size());
    u64 first = 2048;
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (first > last_usable)
            return Error::from_errno(ENOSPC);
        u64 const room = last_usable - first + 1;
        u64 const count = (wanted[i].sectors && wanted[i].sectors <= room ? wanted[i].sectors : room) / 8 * 8;
        if (count < 2048)
            return Error::from_errno(ENOSPC);
        GPTEntry entry {};
        __builtin_memcpy(entry.type, basic_data_type, 16);
        random_guid(entry.unique);
        entry.first_lba = first;
        entry.last_lba = first + count - 1;
        auto const label = wanted[i].label.view();
        for (size_t c = 0; c < label.length() && c < 35; ++c) {
            u16 const unit = static_cast<u8>(label[c]);
            __builtin_memcpy(reinterpret_cast<u8*>(&entry) + offsetof(GPTEntry, name) + c * 2, &unit, 2);
        }
        __builtin_memcpy(table.begin() + i * sizeof(GPTEntry), &entry, sizeof(entry));
        first += count;
    }
    u32 const entries_crc = crc32_of(table.begin(), table.size());

    GPTHeader header {};
    __builtin_memcpy(header.signature, "EFI PART", 8);
    header.revision = 0x00010000;
    header.header_size = sizeof(GPTHeader);
    header.first_usable_lba = 2 + entry_sectors;
    header.last_usable_lba = last_usable;
    random_guid(header.disk_guid);
    header.entry_count = entries;
    header.entry_size = sizeof(GPTEntry);
    header.entries_crc = entries_crc;
    auto const write_header = [&](u64 my_lba, u64 alternate_lba, u64 entries_lba) -> ErrorOr<void> {
        header.my_lba = my_lba;
        header.alternate_lba = alternate_lba;
        header.entries_lba = entries_lba;
        header.header_crc = 0;
        header.header_crc = crc32_of(reinterpret_cast<u8 const*>(&header), sizeof(header));
        u8 sector[512] {};
        __builtin_memcpy(sector, &header, sizeof(header));
        TRY(store.write_sectors(my_lba, 1, { sector, sizeof(sector) }));
        return store.write_sectors(entries_lba, entry_sectors, { table.begin(), table.size() });
    };
    TRY(write_header(1, sectors - 1, 2));
    TRY(write_header(sectors - 1, 1, sectors - 1 - entry_sectors));
    return store.flush();
}

}

ErrorOr<void> partition_disk(StringView whole, Span<NewPartition const> wanted)
{
    auto const named = TRY(find_disk(whole));
    auto const whole_name = TRY(String::from_view(named.whole()));
    auto const disks = TRY(list_disks());
    for (auto const& disk : disks) {
        if (disk.whole() == whole_name.view())
            TRY(unmount_disk(disk.name()));
    }
    {
        auto store = TRY(DeviceStore::open_device(whole_name.view(), false));
        TRY(write_partition_table(*store, wanted));
    }
    StringBuilder path;
    path.appendff("/dev/{}", whole_name.view());
    auto const whole_path = TRY(path.to_string());
    int const fd = open(whole_path.characters(), O_RDONLY);
    if (fd < 0)
        return Error::from_errno(errno);
    int const result = ioctl(fd, VULTURE_DKIOCRESCAN, 0);
    int const problem = errno;
    close(fd);
    if (result < 0)
        return Error::from_errno(problem);
    for (size_t i = 0; i < wanted.size(); ++i) {
        StringBuilder partition;
        partition.appendff("{}s{}", whole_name.view(), i + 1);
        TRY(erase_volume(TRY(partition.to_string()).view(), wanted[i].format.view(), wanted[i].label.view()));
    }
    return {};
}

ErrorOr<void> erase_disk(StringView whole, StringView format, StringView label)
{
    NewPartition const all { TRY(String::from_view(format)), TRY(String::from_view(label)), 0 };
    return partition_disk(whole, { &all, 1 });
}

ErrorOr<bool> verify_volume(StringView name, LineCallback say)
{
    auto const disk = TRY(find_disk(name));
    auto const device = TRY(String::from_view(disk.name()));
    bool const was_mounted = !disk.mount_point.is_empty();
    // It's only fair to the volume with nothing changing it. If something's using it, it's as good as can be got.
    bool unmounted = false;
    if (was_mounted) {
        unmounted = !unmount_disk(device.view()).is_error();
        if (!unmounted)
            sync();
    }
    auto const result = check_with(device.view(), say, false);
    if (unmounted)
        (void)TRY(mount_disk(device.view()));
    return result;
}

ErrorOr<bool> repair_volume(StringView name, LineCallback say)
{
    auto const disk = TRY(find_disk(name));
    auto const device = TRY(String::from_view(disk.name()));
    auto const place = TRY(String::from_view(disk.mount_point.view()));
    bool const was_mounted = !place.is_empty();
    TRY(unmount_disk(device.view()));
    auto const result = check_with(device.view(), say, true);
    if (was_mounted)
        (void)TRY(mount_disk(device.view()));
    return result;
}

ErrorOr<String> attach_image(StringView path, bool read_only)
{
    auto const path_text = TRY(String::from_view(path));
    int const file = open(path_text.characters(), read_only ? O_RDONLY : O_RDWR);
    if (file < 0)
        return Error::from_errno(errno);
    int const images = open("/dev/diskimages", O_RDONLY);
    if (images < 0) {
        int const problem = errno;
        close(file);
        return Error::from_errno(problem);
    }
    int const number = ioctl(images, read_only ? VULTURE_DISKIMAGE_ATTACH_READ_ONLY : VULTURE_DISKIMAGE_ATTACH, &file);
    int const problem = errno;
    close(images);
    close(file);
    if (number < 0)
        return Error::from_errno(problem);
    StringBuilder name;
    name.appendff("disk{}", number);
    return name.to_string();
}

ErrorOr<void> set_startup_disk(StringView name)
{
    auto const chosen = TRY(find_disk(name));
    if (chosen.content() != "vulture-system"_sv)
        return Error::from_errno(EINVAL);
    static constexpr u8 efi_type[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
    auto const disks = TRY(list_disks());
    for (auto const& disk : disks) {
        if (memcmp(disk.info.partition_type, efi_type, 16) != 0 || disk.whole() != chosen.whole())
            continue;
        auto store = TRY(DeviceStore::open_device(disk.name(), false));
        auto volume = TRY(ForeignFS::open_volume(*store, now_in_seconds, false));
        constexpr StringView file_name = "StartupDisk"_sv;
        auto existing = volume->lookup(volume->root(), file_name);
        auto const file = existing.is_error() ? TRY(volume->create(volume->root(), file_name, false)) : existing.release_value();
        TRY(volume->truncate(file.index, 0));
        (void)TRY(volume->write(file.index, 0, { chosen.info.partition_unique, sizeof(chosen.info.partition_unique) }));
        return volume->flush();
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<void> request(StringView line, LineCallback say)
{
    int const fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return Error::from_errno(errno);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%.*s", static_cast<int>(socket_path.length()), socket_path.characters_without_null_termination());
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        int const problem = errno;
        close(fd);
        return Error::from_errno(problem);
    }
    StringBuilder message;
    message.append(line);
    message.append('\n');
    auto const text = TRY(message.to_string());
    if (write(fd, text.characters(), text.view().length()) != static_cast<ssize_t>(text.view().length())) {
        close(fd);
        return Error::from_errno(EIO);
    }
    StringBuilder pending;
    char buffer[1024];
    ErrorOr<void> result = Error::from_errno(ECONNRESET);
    bool finished = false;
    while (!finished) {
        ssize_t const got = read(fd, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; ++i) {
            if (buffer[i] != '\n') {
                pending.append(buffer[i]);
                continue;
            }
            auto const whole = TRY(pending.to_string());
            pending = {};
            auto const view = whole.view();
            if (view == "ok"_sv) {
                result = ErrorOr<void> {};
                finished = true;
            } else if (view.starts_with("errno: "_sv)) {
                result = Error::from_errno(view.substring_view(7).to_number<int>().value_or(EIO));
                finished = true;
            } else if (say) {
                say(view);
            }
        }
    }
    close(fd);
    return result;
}

String size_text(u64 bytes)
{
    StringBuilder builder;
    auto const with_unit = [&](u64 unit, StringView name) {
        u64 const scaled = (bytes * 10 + unit / 2) / unit;
        builder.appendff("{}.{} ", scaled / 10, scaled % 10);
        builder.append(name);
    };
    if (bytes >= 1'000'000'000'000ull)
        with_unit(1'000'000'000'000ull, "TB"_sv);
    else if (bytes >= 1'000'000'000ull)
        with_unit(1'000'000'000ull, "GB"_sv);
    else if (bytes >= 1'000'000ull)
        with_unit(1'000'000ull, "MB"_sv);
    else if (bytes >= 1'000ull)
        with_unit(1'000ull, "KB"_sv);
    else
        builder.appendff("{} Bytes", bytes);
    return MUST(builder.to_string());
}

}
