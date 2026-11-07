/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Format.h>
#include <LibForeignFS/Volume.h>

// foreignfs-exercise make IMAGE SIZE_MIB fat12|fat16|fat32|exfat LABEL
//     A new volume, with a bit of everything on it, for a Mac to check.
// foreignfs-exercise list IMAGE
//     What's on a volume, whoever made it: one line for each thing, and the first bytes of each file.
// foreignfs-exercise add IMAGE
//     Adds to what's there, for a Mac to check again.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fseeko(FILE*, long long offset, int origin);
long long ftello(FILE*);
int fflush(FILE*);
int fclose(FILE*);
int fileno(FILE*);
int ftruncate(int, long);
int fsync(int);
long write(int fd, void const* buffer, unsigned long count);
long time(long*);
}

using namespace ForeignFS;

namespace {

class Output final : public FormatSink {
public:
    explicit Output(int fd)
        : m_fd(fd)
    {
    }
    void write(StringView text) override { (void)::write(m_fd, text.characters_without_null_termination(), text.length()); }

private:
    int m_fd;
};

template<typename... Args>
void say(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    Output output(1);
    format_to(output, format, args...);
}

class FileStore final : public Store {
public:
    FileStore(FILE* file, u64 sectors)
        : m_file(file)
        , m_sectors(sectors)
    {
    }

    [[nodiscard]] u64 sector_count() const override { return m_sectors; }
    [[nodiscard]] bool is_read_only() const override { return false; }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override
    {
        if (fseeko(m_file, static_cast<long long>(first * sector_size), 0) != 0 || fread(buffer.data(), sector_size, count, m_file) != count)
            return Error::from_errno(EIO);
        return {};
    }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        if (fseeko(m_file, static_cast<long long>(first * sector_size), 0) != 0 || fwrite(buffer.data(), sector_size, count, m_file) != count)
            return Error::from_errno(EIO);
        return {};
    }
    ErrorOr<void> flush() override
    {
        fflush(m_file);
        return {};
    }

private:
    FILE* m_file;
    u64 m_sectors;
};

i64 clock_now()
{
    return time(nullptr);
}

ErrorOr<void> put(Volume& volume, NodeIndex directory, StringView name, StringView text)
{
    auto const file = TRY(volume.create(directory, name, false));
    (void)TRY(volume.write(file.index, 0, { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() }));
    return {};
}

ErrorOr<void> fill(Volume& volume)
{
    auto const root = volume.root();
    TRY(put(volume, root, "README.TXT"_sv, "A short name, in capitals.\n"_sv));
    TRY(put(volume, root, "lower.txt"_sv, "A short name, in lower case.\n"_sv));
    TRY(put(volume, root, "A rather long name for a file, with spaces.text"_sv, "A long name.\n"_sv));
    TRY(put(volume, root, "café ☕ 😀.txt"_sv, "Not ASCII.\n"_sv));
    auto const folder = TRY(volume.create(root, "A Folder"_sv, true));
    TRY(put(volume, folder.index, "inside.txt"_sv, "Inside a folder.\n"_sv));
    auto const deeper = TRY(volume.create(folder.index, "Deeper"_sv, true));
    TRY(put(volume, deeper.index, "more.txt"_sv, "Inside another.\n"_sv));

    // Longer than a cluster, in pieces, with something else in between to get in the way.
    auto const large = TRY(volume.create(root, "large.bin"_sv, false));
    auto const other = TRY(volume.create(root, "other.bin"_sv, false));
    Vector<u8> chunk;
    TRY(chunk.try_resize(16 * 1024));
    for (size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<u8>(i ^ (i >> 8));
    for (int i = 0; i < 12; ++i) {
        (void)TRY(volume.write(large.index, i * chunk.size(), chunk.span()));
        (void)TRY(volume.write(other.index, i * chunk.size(), chunk.span()));
    }

    // Many, and some taken away.
    auto const many = TRY(volume.create(root, "Many"_sv, true));
    for (int i = 0; i < 200; ++i) {
        StringBuilder name;
        name.appendff("File number {} of a good many.txt", i);
        TRY(put(volume, many.index, TRY(name.to_string()).view(), "x"_sv));
    }
    for (int i = 0; i < 200; i += 3) {
        StringBuilder name;
        name.appendff("File number {} of a good many.txt", i);
        auto const name_text = TRY(name.to_string());
        auto const going = TRY(volume.lookup(many.index, name_text.view()));
        TRY(volume.remove(many.index, name_text.view()));
        volume.forget(going.index);
    }

    // Renamed, and shortened.
    TRY(volume.rename(root, "lower.txt"_sv, folder.index, "renamed lower.txt"_sv));
    TRY(volume.truncate(other.index, 1000));
    return volume.flush();
}

ErrorOr<void> list_tree(Volume& volume, NodeIndex directory, StringView path)
{
    struct Line {
        String name;
        NodeInfo info;
    };
    Vector<Line> lines;
    TRY(volume.for_each_entry(directory, [&](StringView name, NodeInfo const& info) {
        lines.append(Line { MUST(String::from_view(name)), info });
        return Iteration::Continue;
    }));
    for (size_t i = 1; i < lines.size(); ++i) {
        for (size_t j = i; j > 0 && lines[j].name.view() < lines[j - 1].name.view(); --j) {
            auto moving = move(lines[j]);
            lines[j] = move(lines[j - 1]);
            lines[j - 1] = move(moving);
        }
    }
    for (auto const& line : lines) {
        StringBuilder whole;
        whole.append(path);
        whole.append('/');
        whole.append(line.name.view());
        auto const whole_path = TRY(whole.to_string());
        if (line.info.is_directory) {
            say("{}/\n", whole_path.view());
            TRY(list_tree(volume, line.info.index, whole_path.view()));
        } else {
            u8 bytes[32] {};
            size_t const got = TRY(volume.read(line.info.index, 0, { bytes, sizeof(bytes) }));
            u32 sum = 0;
            for (u64 at = 0; at < line.info.size; at += sizeof(bytes)) {
                size_t const part = TRY(volume.read(line.info.index, at, { bytes, sizeof(bytes) }));
                for (size_t i = 0; i < part; ++i)
                    sum = sum * 31 + bytes[i];
            }
            (void)got;
            say("{} {} {}\n", whole_path.view(), line.info.size, sum);
        }
    }
    return {};
}

}

int main(int argc, char** argv)
{
    Output error(2);
    if (argc >= 6 && StringView(argv[1]) == "make"_sv) {
        FILE* file = fopen(argv[2], "w+b");
        if (!file)
            return 1;
        auto const size = StringView(argv[3]).to_number<u64>().value_or(0);
        if (ftruncate(fileno(file), static_cast<long>(size * 1024 * 1024)) != 0)
            return 1;
        FileStore store(file, size * 1024 * 1024 / Store::sector_size);
        StringView const kind { argv[4] };
        ErrorOr<void> formatted = kind == "exfat"_sv ? format_exfat(store, StringView(argv[5]), clock_now)
            : format_fat(store, StringView(argv[5]), clock_now, kind == "fat12"_sv ? 12 : kind == "fat16"_sv ? 16 : 32);
        if (formatted.is_error()) {
            format_to(error, "exercise: format: {}\n", formatted.error());
            return 1;
        }
        auto volume = open_volume(store, clock_now, false);
        if (volume.is_error()) {
            format_to(error, "exercise: open: {}\n", volume.error());
            return 1;
        }
        auto filled = fill(*volume.value());
        if (filled.is_error()) {
            format_to(error, "exercise: fill: {}\n", filled.error());
            return 1;
        }
        fclose(file);
        say("exercise: made {}\n", StringView(argv[2]));
        return 0;
    }
    if (argc == 3 && (StringView(argv[1]) == "list"_sv || StringView(argv[1]) == "add"_sv)) {
        FILE* file = fopen(argv[2], "r+b");
        if (!file)
            return 1;
        fseeko(file, 0, 2);
        FileStore store(file, static_cast<u64>(ftello(file)) / Store::sector_size);
        auto volume = open_volume(store, clock_now, false);
        if (volume.is_error()) {
            format_to(error, "exercise: open: {}\n", volume.error());
            return 1;
        }
        if (StringView(argv[1]) == "add"_sv) {
            auto added = [&]() -> ErrorOr<void> {
                auto const root = volume.value()->root();
                auto const folder = TRY(volume.value()->create(root, "Added here"_sv, true));
                TRY(put(*volume.value(), folder.index, "written by vultureOS.txt"_sv, "Hello from over here.\n"_sv));
                return volume.value()->flush();
            }();
            if (added.is_error()) {
                format_to(error, "exercise: add: {}\n", added.error());
                return 1;
            }
        } else {
            say("label: {}\ntype: {}\n", volume.value()->label(), volume.value()->type_name());
            auto listed = list_tree(*volume.value(), volume.value()->root(), ""_sv);
            if (listed.is_error()) {
                format_to(error, "exercise: list: {}\n", listed.error());
                return 1;
            }
        }
        fclose(file);
        return 0;
    }
    // A file in the top folder, out to one here: for what the guest wrote on a stick, to be looked at.
    if (argc == 5 && StringView(argv[1]) == "get"_sv) {
        FILE* file = fopen(argv[2], "rb");
        if (!file)
            return 1;
        fseeko(file, 0, 2);
        FileStore store(file, static_cast<u64>(ftello(file)) / Store::sector_size);
        auto volume = open_volume(store, clock_now, false);
        if (volume.is_error()) {
            format_to(error, "exercise: open: {}\n", volume.error());
            return 1;
        }
        StringView const wanted { argv[3] };
        Optional<NodeInfo> found;
        auto looked = volume.value()->for_each_entry(volume.value()->root(), [&](StringView name, NodeInfo const& info) {
            if (name == wanted && !info.is_directory) {
                found = info;
                return Iteration::Stop;
            }
            return Iteration::Continue;
        });
        if (looked.is_error() || !found.has_value()) {
            format_to(error, "exercise: get: there's no {}\n", wanted);
            return 1;
        }
        FILE* out = fopen(argv[4], "wb");
        if (!out)
            return 1;
        u8 bytes[65536];
        for (u64 at = 0; at < found->size;) {
            auto const part = volume.value()->read(found->index, at, { bytes, sizeof(bytes) });
            if (part.is_error() || part.value() == 0) {
                format_to(error, "exercise: get: reading failed\n");
                return 1;
            }
            if (fwrite(bytes, 1, part.value(), out) != part.value())
                return 1;
            at += part.value();
        }
        fclose(out);
        fclose(file);
        say("exercise: got {} ({} bytes)\n", wanted, found->size);
        return 0;
    }
    format_to(error, "usage: foreignfs-exercise make IMAGE SIZE_MIB fat12|fat16|fat32|exfat LABEL | list IMAGE | add IMAGE | get IMAGE NAME OUT\n");
    return 2;
}
