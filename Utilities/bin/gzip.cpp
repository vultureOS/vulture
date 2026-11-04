/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibCompress/Deflate.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// gzip [-cdfkltv1-9] [file...], gunzip and zcat: acts by the name it's run as. Compressing replaces a file with file.gz, and
// the other way round, unless told otherwise. (Compression levels are accepted; there's the one level here.)

namespace {

constexpr size_t output_limit = 1ull << 34;

ErrorOr<Vector<u8>> read_all(int fd)
{
    Vector<u8> data;
    char chunk[65536];
    for (;;) {
        ssize_t const got = read(fd, chunk, sizeof(chunk));
        if (got < 0)
            return Error::from_errno(errno);
        if (got == 0)
            return data;
        size_t const at = data.size();
        TRY(data.try_resize(at + static_cast<size_t>(got)));
        memcpy(data.data() + at, chunk, static_cast<size_t>(got));
    }
}

bool write_all(int fd, ReadonlyBytes data)
{
    size_t done = 0;
    while (done < data.size()) {
        ssize_t const wrote = write(fd, data.data() + done, data.size() - done);
        if (wrote < 0)
            return false;
        done += static_cast<size_t>(wrote);
    }
    return true;
}

// The size a gzip file says its contents are.
u32 stored_size(ReadonlyBytes data)
{
    if (data.size() < 4)
        return 0;
    u8 const* end = data.data() + data.size();
    return static_cast<u32>(end[-4]) | static_cast<u32>(end[-3]) << 8 | static_cast<u32>(end[-2]) << 16 | static_cast<u32>(end[-1]) << 24;
}

}

int main(int argc, char** argv)
{
    char const* name = last_component(argv[0]);
    bool decompress = strcmp(name, "gunzip") == 0 || strcmp(name, "zcat") == 0;
    bool to_stdout = strcmp(name, "zcat") == 0;
    bool force = false, keep = false, list = false, test = false, verbose = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'c':
                to_stdout = true;
                break;
            case 'd':
                decompress = true;
                break;
            case 'f':
                force = true;
                break;
            case 'k':
                keep = true;
                break;
            case 'l':
                list = true;
                break;
            case 't':
                test = true;
                break;
            case 'v':
                verbose = true;
                break;
            case 'q':
            case 'n':
            case 'N':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                break;
            default:
                fprintf(stderr, "usage: gzip [-cdfkltv1-9] [file ...]\n");
                return 1;
            }
        }
    }
    int status = 0;
    bool const from_stdin = first >= argc;
    if (list)
        printf("  compressed uncompressed  ratio uncompressed_name\n");
    for (int i = first; i < argc || (from_stdin && i == first); ++i) {
        char const* path = from_stdin ? nullptr : argv[i];
        bool const this_stdin = !path || strcmp(path, "-") == 0;
        int const in = this_stdin ? STDIN_FILENO : open(path, O_RDONLY);
        if (in < 0) {
            fprintf(stderr, "%s: can't stat: %s (%s): %s\n", name, path, path, strerror(errno));
            status = 1;
            continue;
        }
        struct stat information {};
        (void)fstat(in, &information);
        auto input = read_all(in);
        if (!this_stdin)
            close(in);
        if (input.is_error()) {
            fprintf(stderr, "%s: %s: %s\n", name, this_stdin ? "stdin" : path, strerror(input.error().code()));
            status = 1;
            continue;
        }
        auto const data = input.release_value();
        size_t const path_length = this_stdin ? 0 : strlen(path);
        bool const has_suffix = path_length > 3 && strcmp(path + path_length - 3, ".gz") == 0;
        if (list || test) {
            auto contents = Compress::gzip_decompress(data.span(), output_limit);
            if (contents.is_error()) {
                fprintf(stderr, "%s: %s: not in gzip format\n", name, this_stdin ? "stdin" : path);
                status = 1;
                continue;
            }
            if (list) {
                u32 const size = stored_size(data.span());
                double const ratio = size ? 100.0 * (1.0 - static_cast<double>(data.size()) / size) : 0;
                char plain[1024];
                snprintf(plain, sizeof(plain), "%.*s", static_cast<int>(has_suffix ? path_length - 3 : path_length), this_stdin ? "stdin" : path);
                printf("%12zu %12u %5.1f%% %s\n", data.size(), size, ratio, plain);
            } else if (verbose) {
                printf("%s: OK\n", this_stdin ? "stdin" : path);
            }
            continue;
        }
        if (!this_stdin && !decompress && has_suffix && !force) {
            fprintf(stderr, "%s: %s already has .gz suffix -- unchanged\n", name, path);
            status = 1;
            continue;
        }
        if (decompress && !this_stdin && !to_stdout && !has_suffix) {
            fprintf(stderr, "%s: %s: unknown suffix -- ignored\n", name, path);
            status = 1;
            continue;
        }
        auto converted = decompress ? Compress::gzip_decompress(data.span(), output_limit) : Compress::gzip_compress(data.span());
        if (converted.is_error()) {
            fprintf(stderr, "%s: %s: not in gzip format\n", name, this_stdin ? "stdin" : path);
            status = 1;
            continue;
        }
        if (this_stdin || to_stdout) {
            if (!write_all(STDOUT_FILENO, converted.value().span())) {
                warn(name, "stdout");
                return 1;
            }
            continue;
        }
        char target[1024];
        if (decompress)
            snprintf(target, sizeof(target), "%.*s", static_cast<int>(path_length - 3), path);
        else
            snprintf(target, sizeof(target), "%s.gz", path);
        int const out = open(target, O_WRONLY | O_CREAT | O_TRUNC | (force ? 0 : O_EXCL), information.st_mode & 0777);
        if (out < 0) {
            if (errno == EEXIST)
                fprintf(stderr, "%s: %s already exists -- not overwritten\n", name, target);
            else
                warn(name, target);
            status = 1;
            continue;
        }
        bool const wrote = write_all(out, converted.value().span());
        close(out);
        if (!wrote) {
            warn(name, target);
            status = 1;
            continue;
        }
        if (verbose) {
            size_t const larger = decompress ? converted.value().size() : data.size();
            size_t const smaller = decompress ? data.size() : converted.value().size();
            double const ratio = larger ? 100.0 * (1.0 - static_cast<double>(smaller) / static_cast<double>(larger)) : 0;
            fprintf(stderr, "%s:\t%4.1f%% -- replaced with %s\n", path, ratio, target);
        }
        if (!keep)
            (void)unlink(path);
    }
    return status;
}
