/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibBase/Format.h>

// mkarchive OUTPUT ROOT FILE... | mkarchive OUTPUT ROOT @LIST
//
// Packs files (paths relative to ROOT) into a ustar boot archive, adding
// each parent directory once. ELF files become executable (0755), others
// readable (0644). A path ending in "/" is a directory, optionally
// followed by a tab and its octal mode (0755 if not). The build runs it
// with the root filesystem's list, one entry per line in LIST (names like
// "[" do not survive a CMake list).

// Host tools are built without system headers; these are the C library
// functions this one needs.
extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fseek(FILE*, long offset, int origin);
long ftell(FILE*);
int fclose(FILE*);
void* malloc(size_t);
void free(void*);
ssize_t write(int fd, void const* buffer, size_t count);
}

static constexpr int seek_set = 0;
static constexpr int seek_end = 2;

namespace {

class StandardError final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(2, text.characters_without_null_termination(), text.length()); }
};

class FileSink final : public Archive::TarSink {
public:
    explicit FileSink(FILE* file)
        : m_file(file)
    {
    }

    void write(ReadonlyBytes bytes) override
    {
        if (fwrite(bytes.data(), 1, bytes.size(), m_file) != bytes.size())
            m_failed = true;
    }

    [[nodiscard]] bool failed() const { return m_failed; }

private:
    FILE* m_file;
    bool m_failed { false };
};

}

template<typename... Args>
static int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    StandardError error;
    error.write("mkarchive: "_sv);
    format_to(error, format, args...);
    error.write("\n"_sv);
    return 1;
}

static constexpr size_t max_path = 1024;

// Directories already written, so each parent is added once.
static constexpr size_t max_directories = 1024;
static char s_directories[max_directories][max_path];
static size_t s_directory_count = 0;

static bool add_directory_once(Archive::TarWriter& writer, StringView path, u32 mode = 0755)
{
    for (size_t i = 0; i < s_directory_count; ++i) {
        if (StringView(s_directories[i]) == path)
            return true;
    }
    if (s_directory_count == max_directories || path.length() >= max_path)
        return false;
    __builtin_memcpy(s_directories[s_directory_count], path.characters_without_null_termination(), path.length());
    s_directories[s_directory_count][path.length()] = '\0';
    ++s_directory_count;
    return !writer.add_directory(path, mode).is_error();
}

static int add_file(Archive::TarWriter& writer, StringView root, StringView relative)
{
    // A directory: "path/" or "path/<tab>mode".
    u32 directory_mode = 0755;
    for (size_t i = 0; i < relative.length(); ++i) {
        if (relative[i] != '\t')
            continue;
        directory_mode = 0;
        for (size_t j = i + 1; j < relative.length() && relative[j] >= '0' && relative[j] <= '7'; ++j)
            directory_mode = directory_mode * 8 + static_cast<u32>(relative[j] - '0');
        relative = relative.substring_view(0, i);
        break;
    }
    bool is_directory = relative.length() > 1 && relative[relative.length() - 1] == '/';
    if (is_directory)
        relative = relative.substring_view(0, relative.length() - 1);

    // Parent directories first.
    for (size_t slash = 0; slash < relative.length(); ++slash) {
        if (relative[slash] == '/' && !add_directory_once(writer, relative.substring_view(0, slash)))
            return fail("cannot add the directories of {}", relative);
    }
    if (is_directory)
        return add_directory_once(writer, relative, directory_mode) ? 0 : fail("cannot add the directory {}", relative);

    char host_path[max_path];
    if (root.length() + 1 + relative.length() >= max_path)
        return fail("path too long: {}", relative);
    __builtin_memcpy(host_path, root.characters_without_null_termination(), root.length());
    host_path[root.length()] = '/';
    __builtin_memcpy(host_path + root.length() + 1, relative.characters_without_null_termination(), relative.length());
    host_path[root.length() + 1 + relative.length()] = '\0';

    FILE* input = fopen(host_path, "rb");
    if (!input)
        return fail("cannot open {}", StringView(host_path));
    fseek(input, 0, seek_end);
    long size = ftell(input);
    fseek(input, 0, seek_set);
    auto* data = static_cast<u8*>(malloc(size > 0 ? static_cast<size_t>(size) : 1));
    if (!data || (size > 0 && fread(data, 1, static_cast<size_t>(size), input) != static_cast<size_t>(size))) {
        fclose(input);
        return fail("cannot read {}", StringView(host_path));
    }
    fclose(input);

    ReadonlyBytes bytes { data, static_cast<size_t>(size) };
    bool is_elf = bytes.size() >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
    auto result = writer.add_file(relative, is_elf ? 0755 : 0644, bytes);
    free(data);
    if (result.is_error())
        return fail("{}: {}", relative, result.error());
    return 0;
}

// Reads LIST (one path per line) into memory; the paths point into it.
static char* read_list(char const* path, size_t& length)
{
    FILE* input = fopen(path, "rb");
    if (!input)
        return nullptr;
    fseek(input, 0, seek_end);
    long size = ftell(input);
    fseek(input, 0, seek_set);
    auto* text = static_cast<char*>(malloc(static_cast<size_t>(size > 0 ? size : 0) + 1));
    if (!text || (size > 0 && fread(text, 1, static_cast<size_t>(size), input) != static_cast<size_t>(size))) {
        fclose(input);
        return nullptr;
    }
    fclose(input);
    length = static_cast<size_t>(size > 0 ? size : 0);
    text[length] = '\0';
    return text;
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return fail("usage: mkarchive OUTPUT ROOT [FILE... | @LIST]");

    FILE* output = fopen(argv[1], "wb");
    if (!output)
        return fail("cannot create {}", argv[1]);
    FileSink sink(output);
    Archive::TarWriter writer(sink);
    StringView root(argv[2]);

    for (int i = 3; i < argc; ++i) {
        if (argv[i][0] != '@') {
            if (int result = add_file(writer, root, StringView(argv[i])); result != 0)
                return result;
            continue;
        }
        size_t length = 0;
        char* list = read_list(argv[i] + 1, length);
        if (!list)
            return fail("cannot read {}", StringView(argv[i] + 1));
        size_t start = 0;
        for (size_t position = 0; position <= length; ++position) {
            if (position < length && list[position] != '\n')
                continue;
            if (position > start) {
                if (int result = add_file(writer, root, StringView(list + start, position - start)); result != 0)
                    return result;
            }
            start = position + 1;
        }
        free(list);
    }
    writer.finish();
    if (sink.failed() || fclose(output) != 0)
        return fail("cannot write {}", argv[1]);
    return 0;
}
