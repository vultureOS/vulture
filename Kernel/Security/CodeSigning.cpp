/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Security/CodeSigning.h>
#include <Kernel/Security/Seal.h>
#include <LibBase/Array.h>
#include <LibBase/Vector.h>
#include <LibCrypto/SHA2.h>

namespace Kernel::CodeSigning {

namespace {

bool s_enforced { false };
SpinLock s_lock;
Vector<Array<u8, hash_size>> s_trusted;

bool is_trusted(u8 const* hash)
{
    SpinLockGuard guard(s_lock);
    for (auto const& entry : s_trusted) {
        if (__builtin_memcmp(entry.data(), hash, hash_size) == 0)
            return true;
    }
    return false;
}

Origin origin_of(u8 const* hash)
{
    return Seal::is_system_code(hash) ? Origin::System : Origin::ThirdParty;
}

bool is_vouched_for(u8 const* hash)
{
    return Seal::is_system_code(hash) || is_trusted(hash);
}

}

void initialize(bool enforced)
{
    // Without a seal there's nothing to vouch for what the system came with.
    s_enforced = enforced && Seal::is_active();
    dmesgln("Code signing: {}", s_enforced ? "enforced"_sv : "off"_sv);
}

bool is_enforced()
{
    return s_enforced;
}

ErrorOr<Origin> check_program(Inode& inode, ReadonlyBytes bytes, StringView path)
{
    if (!Seal::is_active())
        return Origin::ThirdParty;
    // Something sealed has one place it must match: the manifest.
    if (Seal::is_sealed(inode)) {
        TRY(Seal::check_contents(inode));
        return Origin::System;
    }
    auto const digest = Crypto::SHA256::hash(bytes);
    if (s_enforced && !is_vouched_for(digest.data())) {
        dmesgln("Code signing: {} isn't signed, so it may not run", path);
        return Error::from_errno(EPERM);
    }
    return origin_of(digest.data());
}

ErrorOr<void> check_mapped_file(Inode& inode)
{
    if (!Seal::is_active())
        return {};
    if (Seal::is_sealed(inode))
        return Seal::check_contents(inode);
    if (!s_enforced)
        return {};
    // Pages of a file are shared with whoever writes it, so this must be nobody untrusted.
    auto const metadata = inode.metadata();
    if (metadata.uid != 0 || (metadata.permissions & 0022) != 0) {
        dmesgln("Code signing: inode {} could be changed after it is checked, so it may not be mapped to run", inode.index());
        return Error::from_errno(EPERM);
    }
    constexpr size_t chunk_size = 256 * KiB;
    auto* chunk = static_cast<u8*>(kmalloc(chunk_size));
    if (!chunk)
        return Error::from_errno(ENOMEM);
    Crypto::SHA256 hasher;
    u64 offset = 0;
    ErrorOr<void> result = {};
    while (offset < metadata.size) {
        auto got = inode.read(offset, min<u64>(chunk_size, metadata.size - offset), IOBuffer::kernel(chunk));
        if (got.is_error()) {
            result = got.release_error();
            break;
        }
        if (got.value() == 0)
            break;
        hasher.update({ chunk, got.value() });
        offset += got.value();
    }
    kfree(chunk);
    TRY(result);
    auto const digest = hasher.digest();
    if (!is_vouched_for(digest.data())) {
        dmesgln("Code signing: inode {} isn't signed, so it may not be mapped to run", inode.index());
        return Error::from_errno(EPERM);
    }
    return {};
}

ErrorOr<void> trust(u8 const* hash)
{
    if (is_trusted(hash))
        return {};
    Array<u8, hash_size> entry;
    __builtin_memcpy(entry.data(), hash, hash_size);
    SpinLockGuard guard(s_lock);
    return s_trusted.try_append(entry);
}

void distrust(u8 const* hash)
{
    SpinLockGuard guard(s_lock);
    s_trusted.remove_all_matching([hash](auto const& entry) { return __builtin_memcmp(entry.data(), hash, hash_size) == 0; });
}

}
