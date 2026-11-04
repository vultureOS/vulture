/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Security/Seal.h>
#include <LibBase/Array.h>
#include <LibBase/New.h>
#include <LibBase/Vector.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/SHA2.h>
#include <LibVultureFS/Sealed.h>
#include <SystemKey.h>

namespace Kernel::Seal {

namespace {

constexpr size_t largest_manifest = 16 * MiB;
constexpr u64 sectors_per_block = VultureFS::block_size / BlockDevice::block_size;

FileSystem* s_file_system { nullptr };
bool s_active { false };
String s_device;
// Sorted, for looking one up by binary search.
Vector<Array<u8, 32>>* s_code { nullptr };

// What the volume in a system image is: `hashes` being a hash for each block, or nothing to go by.
class SealedDevice final : public BlockDevice {
public:
    SealedDevice(BlockDevice& device, u64 first_sector, u64 sectors, u8* hashes, String name)
        : BlockDevice(move(name), sectors, true)
        , m_device(device)
        , m_first_sector(first_sector)
        , m_hashes(hashes)
    {
    }

    ~SealedDevice() override
    {
        if (m_hashes)
            kfree(m_hashes);
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes buffer) override
    {
        // (Only the volume on it reads this, and that goes by whole blocks.)
        if (first % sectors_per_block != 0 || count % sectors_per_block != 0)
            return Error::from_errno(EINVAL);
        TRY(m_device->read_blocks(m_first_sector + first, count, buffer));
        if (!m_hashes)
            return {};
        size_t const blocks = count / sectors_per_block;
        u64 const first_block = first / sectors_per_block;
        for (size_t i = 0; i < blocks; ++i) {
            auto const digest = Crypto::SHA256::hash({ buffer.data() + i * VultureFS::block_size, VultureFS::block_size });
            if (__builtin_memcmp(digest.data(), m_hashes + (first_block + i) * VultureFS::seal_hash_size, VultureFS::seal_hash_size) != 0) {
                dmesgln("Seal: block {} of the system isn't what was signed, so nothing may read it", first_block + i);
                __builtin_memset(buffer.data(), 0, buffer.size());
                return Error::from_errno(EIO);
            }
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64, size_t, ReadonlyBytes) override
    {
        return Error::from_errno(EROFS);
    }

private:
    NonnullRefPtr<BlockDevice> m_device;
    u64 m_first_sector;
    u8* m_hashes;
};

ErrorOr<Vector<u8>> read_all(StringView path, size_t limit)
{
    auto file = TRY(VirtualFileSystem::resolve(path, "/"_sv));
    u64 const size = file.inode->metadata().size;
    if (size > limit)
        return Error::from_errno(EFBIG);
    Vector<u8> bytes;
    TRY(bytes.try_resize(static_cast<size_t>(size)));
    size_t done = 0;
    while (done < size) {
        size_t const got = TRY(file.inode->read(done, size - done, IOBuffer::kernel(bytes.data() + done)));
        if (got == 0)
            return Error::from_errno(EIO);
        done += got;
    }
    return bytes;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool parse_hash(StringView text, u8* output)
{
    if (text.length() != 64)
        return false;
    for (size_t i = 0; i < 32; ++i) {
        int const high = hex_digit(text[2 * i]);
        int const low = hex_digit(text[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<u8>(high << 4 | low);
    }
    return true;
}

int compare_hashes(u8 const* a, u8 const* b)
{
    return __builtin_memcmp(a, b, 32);
}

}

void broken(StringView what)
{
    FixedStringBuilder<256> reason;
    format_to(reason, "the system's seal is broken: {}", what);
    panic(reason.view());
}

ErrorOr<NonnullRefPtr<BlockDevice>> open_system_image(BlockDevice& device, bool checking)
{
    u8 first_block[VultureFS::block_size];
    if (device.block_count() < 2 * sectors_per_block)
        return Error::from_errno(ENOTSUP);
    TRY(device.read_blocks(0, sectors_per_block, { first_block, sizeof(first_block) }));
    VultureFS::SealHeader header;
    __builtin_memcpy(&header, first_block, sizeof(header));
    if (__builtin_memcmp(header.magic, VultureFS::seal_magic, sizeof(header.magic)) != 0)
        return Error::from_errno(ENOTSUP);
    // This comes first: it's what says the rest can be believed.
    if (checking && !Crypto::ed25519_verify(system_public_key, { first_block, VultureFS::seal_signed_size() }, header.signature))
        return Error::from_errno(EPERM);
    u64 const volume_sectors = header.volume_blocks * sectors_per_block;
    u64 const first_sector = VultureFS::seal_blocks_before_volume(header) * sectors_per_block;
    if (first_sector + volume_sectors > device.block_count() || header.hash_blocks != VultureFS::seal_hash_blocks(header.volume_blocks))
        return Error::from_errno(ENOTSUP);

    u8* hashes = nullptr;
    if (checking) {
        size_t const hashes_size = header.hash_blocks * VultureFS::block_size;
        hashes = static_cast<u8*>(kmalloc(hashes_size));
        if (!hashes)
            return Error::from_errno(ENOMEM);
        auto read = device.read_blocks(sectors_per_block, header.hash_blocks * sectors_per_block, { hashes, hashes_size });
        if (read.is_error()) {
            kfree(hashes);
            return read.release_error();
        }
        auto const digest = Crypto::SHA256::hash({ hashes, hashes_size });
        if (__builtin_memcmp(digest.data(), header.hash_of_hashes, sizeof(header.hash_of_hashes)) != 0) {
            kfree(hashes);
            return Error::from_errno(EIO);
        }
    }

    auto name = TRY(String::from_view(device.name()));
    auto* sealed = new (Base::nothrow) SealedDevice(device, first_sector, volume_sectors, hashes, move(name));
    if (!sealed) {
        if (hashes)
            kfree(hashes);
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*static_cast<BlockDevice*>(sealed));
}

void initialize(FileSystem& volume, bool enabled)
{
    if (!enabled) {
        dmesgln("Seal: off, by the boot word noseal");
        return;
    }
    auto manifest = read_all("/System/Library/Seal/Manifest"_sv, largest_manifest);
    if (manifest.is_error())
        broken("there is no list of what may run"_sv);

    s_code = new Vector<Array<u8, 32>>;
    StringView const text { reinterpret_cast<char const*>(manifest.value().data()), manifest.value().size() };
    bool first = true;
    text.for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
        if (first) {
            if (line != "seal 2"_sv)
                broken("a list of a kind this kernel doesn't know"_sv);
            first = false;
            return;
        }
        Array<u8, 32> hash;
        if (line.length() < 2 + 64 || !line.starts_with("x "_sv) || !parse_hash(line.substring_view(2, 64), hash.data()))
            broken("a list that doesn't make sense"_sv);
        MUST(s_code->try_append(hash));
    });

    // For finding code by its hash. (Insertion sort would do, but a program may come in many
    // copies, and there are hundreds.)
    auto& code = *s_code;
    for (size_t gap = code.size() / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < code.size(); ++i) {
            auto const moving = code[i];
            size_t at = i;
            while (at >= gap && compare_hashes(code[at - gap].data(), moving.data()) > 0) {
                code[at] = code[at - gap];
                at -= gap;
            }
            code[at] = moving;
        }
    }

    s_file_system = &volume;
    s_device = MUST(String::from_view(volume.device_name()));
    s_active = true;
    dmesgln("Seal: all as signed, with {} programs", code.size());
}

bool is_active()
{
    return s_active;
}

bool is_sealed(Inode const& inode)
{
    return s_active && &inode.file_system() == s_file_system;
}

ErrorOr<void> check_may_change(Inode const& inode)
{
    if (is_sealed(inode))
        return Error::from_errno(EROFS);
    return {};
}

bool protects_block_device(StringView name)
{
    if (!is_active() || s_device.is_empty())
        return false;
    // The volume's own device, or the whole of the disk it is on (disk1 for disk1s1).
    return s_device.view() == name || s_device.view().starts_with(name);
}

bool is_system_code(u8 const* hash)
{
    if (!s_code)
        return false;
    size_t low = 0;
    size_t high = s_code->size();
    while (low < high) {
        size_t const middle = low + (high - low) / 2;
        int const order = compare_hashes((*s_code)[middle].data(), hash);
        if (order == 0)
            return true;
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return false;
}

}
