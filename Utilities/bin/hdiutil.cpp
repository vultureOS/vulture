/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibDiskManagement/Disks.h>
#include <LibForeignFS/Volume.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// hdiutil: disk images, as on a Mac. A disk image, as far as this goes, is a plain one: what a Mac's hdiutil calls UDRW with
// no resource fork; with the last 512 bytes being its way of saying what it is ("koly") when it has come from one.

using namespace DiskManagement;

namespace {

[[noreturn]] void usage()
{
    fprintf(stderr, "Usage: hdiutil <verb> <options>\n<verb> is one of the following:\nhelp\nattach\ndetach\neject\ncreate\ninfo\nmount\nunmount\n");
    exit(1);
}

class ImageStore final : public ForeignFS::Store {
public:
    ImageStore(int fd, u64 sectors)
        : m_fd(fd)
        , m_sectors(sectors)
    {
    }

    [[nodiscard]] u64 sector_count() const override { return m_sectors; }
    [[nodiscard]] bool is_read_only() const override { return false; }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override
    {
        if (pread(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size)) != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(EIO);
        return {};
    }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        if (pwrite(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size)) != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(EIO);
        return {};
    }
    ErrorOr<void> flush() override { return {}; }

private:
    int m_fd;
    u64 m_sectors;
};

i64 now()
{
    return static_cast<i64>(time(nullptr));
}

// "10m", "1g": in bytes.
u64 parse_size(char const* text)
{
    char* end = nullptr;
    u64 size = strtoull(text, &end, 10);
    switch (*end) {
    case 't':
    case 'T':
        size *= 1024;
        [[fallthrough]];
    case 'g':
    case 'G':
        size *= 1024;
        [[fallthrough]];
    case 'm':
    case 'M':
        size *= 1024;
        [[fallthrough]];
    case 'k':
    case 'K':
        size *= 1024;
        break;
    case 'b':
    case 'B':
        size *= 512;
        break;
    default:
        break;
    }
    return size;
}

int create(int argc, char** argv)
{
    char const* size_text = nullptr;
    char const* format = "exfat";
    char const* label = "untitled";
    char const* output = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-size") == 0 && i + 1 < argc)
            size_text = argv[++i];
        else if (strcmp(argv[i], "-fs") == 0 && i + 1 < argc)
            format = argv[++i];
        else if (strcmp(argv[i], "-volname") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (strcmp(argv[i], "-type") == 0 || strcmp(argv[i], "-layout") == 0)
            ++i;
        else if (argv[i][0] != '-')
            output = argv[i];
    }
    if (!size_text || !output) {
        fprintf(stderr, "Usage: hdiutil create <sizespec> <imagepath>\n");
        return 1;
    }
    u64 const size = parse_size(size_text);
    // It's called what a Mac's would be called, unless told otherwise.
    StringBuilder path;
    path.append(StringView { output, strlen(output) });
    if (!StringView { output, strlen(output) }.ends_with(".dmg"_sv) && !StringView { output, strlen(output) }.ends_with(".iso"_sv))
        path.append(".dmg"_sv);
    auto const path_text = MUST(path.to_string());
    int const fd = open(path_text.characters(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fprintf(stderr, "hdiutil: create failed - %s\n", strerror(errno));
        return 1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        fprintf(stderr, "hdiutil: create failed - %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    ImageStore store(fd, size / ForeignFS::Store::sector_size);
    StringView const label_view { label, strlen(label) };
    ErrorOr<void> formatted = {};
    if (strcasecmp(format, "exfat") == 0)
        formatted = ForeignFS::format_exfat(store, label_view, now);
    else if (strcasecmp(format, "ms-dos") == 0 || strcasecmp(format, "fat32") == 0 || strcasecmp(format, "ms-dos fat32") == 0 || strcasecmp(format, "msdos") == 0)
        formatted = ForeignFS::format_fat(store, label_view, now);
    else if (strcasecmp(format, "none") != 0)
        formatted = Error::from_errno(EINVAL);
    close(fd);
    if (formatted.is_error()) {
        fprintf(stderr, "hdiutil: create failed - %s\n", strerror(formatted.error().code()));
        return 1;
    }
    printf("created: %s\n", path_text.characters());
    return 0;
}

int attach(int argc, char** argv)
{
    char const* image = nullptr;
    bool read_only = false;
    bool no_mount = false;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-readonly") == 0)
            read_only = true;
        else if (strcmp(argv[i], "-nomount") == 0)
            no_mount = true;
        else if (argv[i][0] != '-')
            image = argv[i];
    }
    if (!image) {
        fprintf(stderr, "Usage: hdiutil attach [options] <image>\n");
        return 1;
    }
    char whole_path[1024];
    if (!realpath(image, whole_path)) {
        fprintf(stderr, "hdiutil: attach failed - %s\n", strerror(errno));
        return 1;
    }
    StringBuilder line;
    line.appendff("{} {}", read_only ? "attach-read-only"_sv : "attach"_sv, StringView { whole_path, strlen(whole_path) });
    String disk;
    auto const attached = request(MUST(line.to_string()).view(), [&](StringView text) { disk = MUST(String::from_view(text)); });
    if (attached.is_error()) {
        fprintf(stderr, "hdiutil: attach failed - %s\n", strerror(attached.error().code()));
        return 1;
    }
    (void)no_mount;
    // Give diskarbitrationd a moment with what it's just heard about.
    timespec const moment { 0, 200'000'000 };
    nanosleep(&moment, nullptr);
    auto const disks = list_disks();
    if (!disks.is_error()) {
        for (auto const& one : disks.value()) {
            if (one.whole() != disk.view())
                continue;
            printf("/dev/%-16s\t%-24s\t%s\n", one.info.name, one.is_whole() ? "" : one.info.content, one.mount_point.is_empty() ? "" : one.mount_point.characters());
        }
    }
    return 0;
}

int detach(int argc, char** argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: hdiutil detach <devname>\n");
        return 1;
    }
    char const* const name = argv[argc - 1];
    StringBuilder line;
    line.appendff("eject {}", StringView { name, strlen(name) });
    auto const result = request(MUST(line.to_string()).view());
    if (result.is_error()) {
        fprintf(stderr, "hdiutil: detach failed - %s\n", strerror(result.error().code()));
        return 1;
    }
    printf("\"%s\" ejected.\n", name);
    return 0;
}

int info()
{
    printf("framework       : vultureOS\n");
    auto const disks = list_disks();
    if (disks.is_error())
        return 1;
    for (auto const& one : disks.value()) {
        if (strcmp(one.info.bus, "disk image") != 0)
            continue;
        printf("================================================\n");
        printf("/dev/%s\n", one.info.name);
    }
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
        usage();
    char const* const verb = argv[1];
    if (strcmp(verb, "create") == 0)
        return create(argc - 2, argv + 2);
    if (strcmp(verb, "attach") == 0 || strcmp(verb, "mount") == 0)
        return attach(argc - 2, argv + 2);
    if (strcmp(verb, "detach") == 0 || strcmp(verb, "eject") == 0)
        return detach(argc - 2, argv + 2);
    if (strcmp(verb, "unmount") == 0 && argc >= 3) {
        StringBuilder line;
        line.appendff("unmount {}", StringView { argv[2], strlen(argv[2]) });
        auto const result = request(MUST(line.to_string()).view());
        return result.is_error() ? 1 : 0;
    }
    if (strcmp(verb, "info") == 0)
        return info();
    usage();
}
