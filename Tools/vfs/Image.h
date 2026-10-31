/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>
#include <LibBase/Vector.h>
#include <LibVultureFS/BlockStore.h>

// Host tools are built without system headers; these are the C library
// functions the vultureFS tools need.
extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fseek(FILE*, long offset, int origin);
long ftell(FILE*);
int fflush(FILE*);
int fileno(FILE*);
int fsync(int fd);
int fclose(FILE*);
ssize_t write(int fd, void const* buffer, size_t count);
}

namespace Tools {

constexpr size_t sector_size = 512;
// Partitions start 1 MiB in, as disk utilities do.
constexpr u64 partition_start_sector = 2048;

class StandardError final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(2, text.characters_without_null_termination(), text.length()); }
};

class StandardOutput final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(1, text.characters_without_null_termination(), text.length()); }
};

template<typename... Args>
void print(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    StandardOutput output;
    format_to(output, format, args...);
}

// A file on the host, read whole.
ErrorOr<Vector<u8>> read_file(char const* path);

// Random bytes from the host, for GUIDs and UUIDs.
void random_bytes(Bytes);

// Nanoseconds since 1970.
i64 host_clock();

// Part of an image file seen as vultureFS blocks.
class ImageStore final : public VultureFS::BlockStore {
public:
    ImageStore(FILE* file, u64 byte_offset, u64 block_count)
        : m_file(file)
        , m_offset(byte_offset)
        , m_blocks(block_count)
    {
    }

    [[nodiscard]] u64 block_count() const override { return m_blocks; }
    ErrorOr<void> read_blocks(VultureFS::BlockIndex first, size_t count, Bytes) override;
    ErrorOr<void> write_blocks(VultureFS::BlockIndex first, size_t count, ReadonlyBytes) override;
    ErrorOr<void> flush() override;

private:
    FILE* m_file;
    u64 m_offset;
    u64 m_blocks;
};

struct Partition {
    u64 first_sector;
    u64 sector_count;
};

// Writes a protective MBR and a GPT (primary and backup) with one
// vultureFS partition filling the disk, to a new sparse file of `size`
// bytes.
ErrorOr<Partition> create_disk_image(FILE*, u64 size, StringView partition_name);

// The vultureFS partition of a GPT disk image, or, if the image is a bare
// volume, all of it.
ErrorOr<Partition> find_vulturefs_partition(FILE*);

}
