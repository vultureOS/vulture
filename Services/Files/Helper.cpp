/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibArchive/Zip.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibImage/Image.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// FilesHelper compress ARCHIVE FOLDER NAME... | expand ARCHIVE DESTINATION | rotate left|right PICTURE: what Files does with
// things that could be anything, in a process of its own whose sandbox reaches only what it's working on.

namespace {

constexpr u64 most_expanded = 16ull * 1024 * 1024 * 1024;

ErrorOr<Vector<u8>> read_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    static u8 buffer[1024 * 1024];
    for (;;) {
        ssize_t const got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            int const saved = errno;
            close(fd);
            return Error::from_errno(saved);
        }
        if (got == 0)
            break;
        TRY(bytes.try_ensure_capacity(bytes.size() + static_cast<size_t>(got)));
        for (ssize_t i = 0; i < got; ++i)
            bytes.append(buffer[i]);
    }
    close(fd);
    return bytes;
}

ErrorOr<void> write_file(char const* path, ReadonlyBytes bytes, u32 mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return Error::from_errno(errno);
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t const wrote = write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote < 0) {
            int const saved = errno;
            close(fd);
            return Error::from_errno(saved);
        }
        done += static_cast<size_t>(wrote);
    }
    close(fd);
    return {};
}

void dos_time_of(time_t when, u16& date, u16& clock)
{
    tm parts;
    gmtime_r(&when, &parts);
    date = static_cast<u16>(((max(parts.tm_year - 80, 0)) << 9) | ((parts.tm_mon + 1) << 5) | parts.tm_mday);
    clock = static_cast<u16>((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
}

String joined(StringView a, StringView b)
{
    StringBuilder path;
    path.append(a);
    if (!a.is_empty() && !a.ends_with('/'))
        path.append('/');
    path.append(b);
    return MUST(path.to_string());
}

ErrorOr<void> add_tree(Archive::ZipWriter& zip, StringView disk_path, StringView member)
{
    auto terminated = TRY(String::from_view(disk_path));
    struct stat information {};
    if (lstat(terminated.characters(), &information) != 0)
        return Error::from_errno(errno);
    u16 date = 0;
    u16 clock = 0;
    dos_time_of(information.st_mtime, date, clock);
    if (S_ISLNK(information.st_mode)) {
        char target[1024];
        ssize_t const length = readlink(terminated.characters(), target, sizeof(target));
        if (length < 0)
            return Error::from_errno(errno);
        return zip.add_symbolic_link(member, StringView(target, static_cast<size_t>(length)), date, clock);
    }
    if (S_ISDIR(information.st_mode)) {
        TRY(zip.add_directory(member, information.st_mode & 07777, date, clock));
        DIR* directory = opendir(terminated.characters());
        if (!directory)
            return Error::from_errno(errno);
        while (auto* entry = readdir(directory)) {
            StringView name { entry->d_name, strlen(entry->d_name) };
            if (name == "."_sv || name == ".."_sv)
                continue;
            TRY(add_tree(zip, joined(disk_path, name).view(), joined(member, name).view()));
        }
        closedir(directory);
        return {};
    }
    auto bytes = TRY(read_file(terminated.characters()));
    return zip.add_file(member, information.st_mode & 07777, bytes.span(), date, clock);
}

ErrorOr<void> compress(char const* archive, char const* folder, int count, char** names)
{
    Archive::ZipWriter zip;
    StringView const folder_view { folder, strlen(folder) };
    for (int i = 0; i < count; ++i) {
        StringView const name { names[i], strlen(names[i]) };
        TRY(add_tree(zip, joined(folder_view, name).view(), name));
    }
    auto bytes = TRY(zip.finish());
    return write_file(archive, bytes.span(), 0644);
}

void make_parents(StringView destination, StringView member)
{
    for (size_t i = 0; i < member.length(); ++i) {
        if (member[i] == '/')
            (void)mkdir(joined(destination, member.substring_view(0, i)).characters(), 0755);
    }
}

ErrorOr<void> expand(char const* archive, char const* destination)
{
    auto bytes = TRY(read_file(archive));
    auto zip = TRY(Archive::ZipReader::parse(bytes.span()));
    StringView const root { destination, strlen(destination) };
    u64 total = 0;
    for (auto const& entry : zip.entries()) {
        auto member = entry.path.view();
        if (member.ends_with('/'))
            member = member.substring_view(0, member.length() - 1);
        // Nothing an archive says can put anything outside of where it's going.
        if (!Archive::is_safe_member_path(member))
            return Error::from_string_literal("an archive names somewhere it shouldn't");
        total += entry.size;
        if (total > most_expanded)
            return Error::from_string_literal("an archive says it's bigger than any should be");
        make_parents(root, member);
        auto path = joined(root, member);
        if (entry.is_directory) {
            (void)mkdir(path.characters(), 0755);
            continue;
        }
        auto data = TRY(zip.read(entry));
        if (entry.is_symbolic_link) {
            StringView const target { reinterpret_cast<char const*>(data.data()), data.size() };
            if (!Archive::is_safe_link_target(member, target))
                return Error::from_string_literal("an archive's link goes somewhere it shouldn't");
            if (symlink(MUST(String::from_view(target)).characters(), path.characters()) != 0)
                return Error::from_errno(errno);
            continue;
        }
        u32 const mode = entry.mode & 0777 ? entry.mode & 0777 : 0644;
        TRY(write_file(path.characters(), data.span(), mode));
    }
    return {};
}

ErrorOr<void> rotate(bool clockwise, char const* path)
{
    auto bytes = TRY(read_file(path));
    auto picture = TRY(Image::decode(bytes.span()));
    auto turned = TRY(Gfx::Bitmap::create({ picture->height(), picture->width() }));
    for (int y = 0; y < picture->height(); ++y) {
        for (int x = 0; x < picture->width(); ++x) {
            if (clockwise)
                turned->set_pixel_at(picture->height() - 1 - y, x, picture->pixel_at(x, y));
            else
                turned->set_pixel_at(y, picture->width() - 1 - x, picture->pixel_at(x, y));
        }
    }
    auto encoded = TRY(Image::encode_png(*turned));
    return write_file(path, encoded.span(), 0644);
}

}

int main(int argc, char** argv)
{
    ErrorOr<void> result = Error::from_errno(EINVAL);
    if (argc >= 5 && strcmp(argv[1], "compress") == 0)
        result = compress(argv[2], argv[3], argc - 4, argv + 4);
    else if (argc == 4 && strcmp(argv[1], "expand") == 0)
        result = expand(argv[2], argv[3]);
    else if (argc == 4 && strcmp(argv[1], "rotate") == 0)
        result = rotate(strcmp(argv[2], "right") == 0, argv[3]);
    if (result.is_error()) {
        fprintf(stderr, "FilesHelper: %s\n", result.error().is_errno() ? strerror(result.error().code()) : "it can't be done");
        return 1;
    }
    return 0;
}
