/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibVultureFS/Format.h>

// A system image, as a Mac has its signed system volume: a read-only volume, with a hash of every block in it,
// and a signature over those. The same bytes are a partition on a disk and, with no disk, what the loader brings
// into memory.
//
//     block 0             a SealHeader
//     blocks 1 to H       the SHA-256 of each block of the volume, in order
//     blocks H + 1 on     the volume
namespace VultureFS {

constexpr char seal_magic[8] = { 'v', 's', 'e', 'a', 'l', '0', '0', '2' };
constexpr size_t seal_hash_size = 32;
constexpr size_t seal_signature_size = 64;

struct SealHeader {
    char magic[8];
    u64 volume_blocks;
    u64 hash_blocks;
    // Of all the blocks of hashes.
    u8 hash_of_hashes[seal_hash_size];
    // Ed25519, of everything before it, by the key the kernel was built with.
    u8 signature[seal_signature_size];
};

[[nodiscard]] constexpr u64 seal_hash_blocks(u64 volume_blocks)
{
    return (volume_blocks * seal_hash_size + block_size - 1) / block_size;
}

// How much of the image comes before the volume.
[[nodiscard]] constexpr u64 seal_blocks_before_volume(SealHeader const& header)
{
    return 1 + header.hash_blocks;
}

[[nodiscard]] constexpr size_t seal_signed_size()
{
    return __builtin_offsetof(SealHeader, signature);
}

}
