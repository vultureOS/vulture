/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/RaspberryPi/Payload.h>
#include <LibBase/Format.h>
#include <LibBase/Vector.h>

// mkpiimage STUB KERNEL ARCHIVE OUTPUT
//
// Makes kernel8.img, the one file a Raspberry Pi's firmware loads: the stub, padded to a page, then a header, the
// kernel and the boot archive, each on a page boundary. It also says how big it is, in the header the stub starts with.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fseek(FILE*, long offset, int origin);
long ftell(FILE*);
int fclose(FILE*);
int printf(char const* format, ...);
}

using namespace Boot::RaspberryPi;

static constexpr size_t page_size = 4096;
static constexpr size_t image_size_offset = 16;

static bool read_file(char const* path, Vector<u8>& out)
{
    FILE* file = fopen(path, "rb");
    if (!file) {
        printf("mkpiimage: can't open %s\n", path);
        return false;
    }
    constexpr int seek_set = 0;
    constexpr int seek_end = 2;
    fseek(file, 0, seek_end);
    long const size = ftell(file);
    fseek(file, 0, seek_set);
    if (size < 0 || out.try_resize(static_cast<size_t>(size)).is_error() || fread(out.begin(), 1, out.size(), file) != out.size()) {
        printf("mkpiimage: can't read %s\n", path);
        fclose(file);
        return false;
    }
    fclose(file);
    return true;
}

static void pad_to_page(Vector<u8>& data)
{
    MUST(data.try_resize(align_up<size_t>(data.size(), page_size)));
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        printf("usage: mkpiimage STUB KERNEL ARCHIVE OUTPUT\n");
        return 2;
    }
    Vector<u8> image;
    Vector<u8> kernel;
    Vector<u8> archive;
    if (!read_file(argv[1], image) || !read_file(argv[2], kernel) || !read_file(argv[3], archive))
        return 1;
    if (image.size() < image_size_offset + 8) {
        printf("mkpiimage: %s isn't a stub\n", argv[1]);
        return 1;
    }
    pad_to_page(image);
    size_t const header_at = image.size();

    PayloadHeader header {};
    __builtin_memcpy(header.magic, payload_magic, sizeof(header.magic));
    header.kernel_offset = page_size;
    header.kernel_size = kernel.size();
    header.archive_offset = header.kernel_offset + align_up<size_t>(kernel.size(), page_size);
    header.archive_size = archive.size();

    MUST(image.try_resize(header_at + page_size));
    __builtin_memcpy(image.begin() + header_at, &header, sizeof(header));

    size_t const kernel_at = image.size();
    MUST(image.try_resize(kernel_at + align_up<size_t>(kernel.size(), page_size)));
    __builtin_memcpy(image.begin() + kernel_at, kernel.begin(), kernel.size());

    size_t const archive_at = image.size();
    MUST(image.try_resize(archive_at + align_up<size_t>(archive.size(), page_size)));
    __builtin_memcpy(image.begin() + archive_at, archive.begin(), archive.size());

    u64 const total = image.size();
    __builtin_memcpy(image.begin() + image_size_offset, &total, sizeof(total));

    FILE* output = fopen(argv[4], "wb");
    if (!output || fwrite(image.begin(), 1, image.size(), output) != image.size()) {
        printf("mkpiimage: can't write %s\n", argv[4]);
        return 1;
    }
    fclose(output);
    return 0;
}
