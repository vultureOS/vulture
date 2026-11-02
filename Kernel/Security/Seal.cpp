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
#include <LibBase/Atomic.h>
#include <LibBase/HashMap.h>
#include <LibBase/Vector.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/SHA2.h>
#include <SystemKey.h>

namespace Kernel::Seal {

namespace {

enum class Kind : u8 {
    Directory,
    File,
    Link,
    Open,
};

enum class ContentState : u8 {
    Unchecked,
    Good,
    Bad,
};

struct Entry {
    Kind kind { Kind::File };
    u8 hash[32] {};
    u64 size { 0 };
    Atomic<u8> state { static_cast<u8>(ContentState::Unchecked) };
};

constexpr size_t largest_manifest = 16 * MiB;

FileSystem* s_file_system { nullptr };
// Only set once it is whole: a half-made one would hide what hasn't been reached yet.
HashMap<u64, Entry*>* s_entries { nullptr };
HashMap<u64, Entry*>* s_building { nullptr };
String s_device;
// Sorted, for looking one up by binary search.
Vector<Array<u8, 32>>* s_code { nullptr };

[[noreturn]] void broken(StringView what, StringView path = {})
{
    FixedStringBuilder<256> reason;
    format_to(reason, "the system's seal is broken: {}{}{}", what, path.is_empty() ? ""_sv : ": "_sv, path);
    panic(reason.view());
}

ErrorOr<Vector<u8>> read_all(StringView path, size_t limit)
{
    auto description = TRY(VirtualFileSystem::open(path, "/"_sv, O_RDONLY, 0));
    Vector<u8> bytes;
    auto const size = TRY(description->stat()).st_size;
    if (size < 0 || static_cast<u64>(size) > limit)
        return Error::from_errno(EFBIG);
    TRY(bytes.try_resize(static_cast<size_t>(size)));
    size_t done = 0;
    while (done < bytes.size()) {
        size_t const got = TRY(description->read(IOBuffer::kernel(bytes.data() + done), bytes.size() - done));
        if (got == 0)
            break;
        done += got;
    }
    if (done != bytes.size())
        return Error::from_errno(EIO);
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
        int const high = hex_digit(text[i * 2]);
        int const low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<u8>(high << 4 | low);
    }
    return true;
}

// The next space-separated word of `line`, which loses it and the space.
StringView take_word(StringView& line)
{
    auto const space = line.find(' ');
    if (!space.has_value()) {
        auto const word = line;
        line = {};
        return word;
    }
    auto const word = line.substring_view(0, space.value());
    line = line.substring_view(space.value() + 1);
    return word;
}

bool parse_octal(StringView text, u32& value)
{
    value = 0;
    if (text.is_empty())
        return false;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '7')
            return false;
        value = value * 8 + static_cast<u32>(text[i] - '0');
    }
    return true;
}

bool parse_decimal(StringView text, u64& value)
{
    value = 0;
    if (text.is_empty())
        return false;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + static_cast<u64>(text[i] - '0');
    }
    return true;
}

void add_line(StringView line)
{
    StringView rest = line;
    auto const kind_word = take_word(rest);
    Kind kind;
    u32 mode = 0;
    u64 size = 0;
    u8 hash[32] = {};
    bool is_code = false;
    if (kind_word == "d"_sv) {
        kind = Kind::Directory;
        if (!parse_octal(take_word(rest), mode))
            broken("a bad line in the manifest"_sv, line);
    } else if (kind_word == "o"_sv) {
        kind = Kind::Open;
    } else if (kind_word == "f"_sv || kind_word == "x"_sv) {
        kind = Kind::File;
        is_code = kind_word == "x"_sv;
        if (!parse_octal(take_word(rest), mode) || !parse_decimal(take_word(rest), size) || !parse_hash(take_word(rest), hash))
            broken("a bad line in the manifest"_sv, line);
    } else if (kind_word == "l"_sv) {
        kind = Kind::Link;
        if (!parse_hash(take_word(rest), hash))
            broken("a bad line in the manifest"_sv, line);
    } else {
        broken("a bad line in the manifest"_sv, line);
    }

    // The manifest's paths are from the root, and may have spaces in them: all of what's left.
    auto const path = rest;
    if (path.is_empty())
        broken("a bad line in the manifest"_sv, line);
    auto resolved = VirtualFileSystem::resolve(path, "/"_sv, false);
    if (resolved.is_error())
        broken("something is missing"_sv, path);
    auto& inode = *resolved.value().inode;
    if (&inode.file_system() != s_file_system)
        broken("something is on another volume"_sv, path);
    auto const metadata = inode.metadata();

    switch (kind) {
    case Kind::Directory:
    case Kind::Open:
        if (metadata.type != InodeType::Directory)
            broken("something should be a directory"_sv, path);
        break;
    case Kind::File:
        if (metadata.type != InodeType::File)
            broken("something should be a file"_sv, path);
        if (metadata.size != size)
            broken("a file has changed size"_sv, path);
        break;
    case Kind::Link: {
        if (metadata.type != InodeType::SymbolicLink)
            broken("something should be a symbolic link"_sv, path);
        char target[VirtualFileSystem::max_path];
        auto length = inode.read_link({ reinterpret_cast<u8*>(target), sizeof(target) });
        if (length.is_error())
            broken("a link can't be read"_sv, path);
        auto const digest = Crypto::SHA256::hash({ reinterpret_cast<u8 const*>(target), length.value() });
        if (__builtin_memcmp(digest.data(), hash, 32) != 0)
            broken("a link goes somewhere else"_sv, path);
        break;
    }
    }
    if (kind == Kind::Directory || kind == Kind::File) {
        if ((metadata.permissions & 07777) != mode)
            broken("something has changed mode"_sv, path);
        if (metadata.uid != 0 || metadata.gid != 0)
            broken("something has changed owner"_sv, path);
    }

    auto* entry = new Entry;
    entry->kind = kind;
    entry->size = size;
    __builtin_memcpy(entry->hash, hash, 32);
    s_building->set(inode.index(), entry);
    if (is_code) {
        Array<u8, 32> code;
        __builtin_memcpy(code.data(), hash, 32);
        s_code->append(code);
    }
}

Entry* entry_for(Inode const& inode)
{
    if (!s_entries || &inode.file_system() != s_file_system)
        return nullptr;
    auto* found = s_entries->get(static_cast<u64>(inode.index()));
    return found ? *found : nullptr;
}

int compare_hashes(u8 const* a, u8 const* b)
{
    return __builtin_memcmp(a, b, 32);
}

}

void initialize(bool enabled)
{
    if (!enabled) {
        dmesgln("Seal: off, by the boot word noseal");
        return;
    }
    auto manifest = read_all("/System/Library/Seal/Manifest"_sv, largest_manifest);
    if (manifest.is_error())
        broken("there is no manifest"_sv);
    auto signature = read_all("/System/Library/Seal/Manifest.sig"_sv, Crypto::ed25519_signature_size);
    if (signature.is_error() || signature.value().size() != Crypto::ed25519_signature_size)
        broken("there is no signature"_sv);
    if (!Crypto::ed25519_verify(system_public_key, manifest.value().span(), signature.value().data()))
        broken("the manifest isn't signed by this system's key"_sv);

    auto root = VirtualFileSystem::resolve("/"_sv, "/"_sv);
    if (root.is_error())
        broken("there is no root"_sv);
    s_file_system = &root.value().inode->file_system();
    s_building = new HashMap<u64, Entry*>;
    s_code = new Vector<Array<u8, 32>>;

    StringView const text { reinterpret_cast<char const*>(manifest.value().data()), manifest.value().size() };
    size_t count = 0;
    bool first = true;
    text.for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
        if (first) {
            if (line != "seal 1"_sv)
                broken("a manifest of a kind this kernel doesn't know"_sv);
            first = false;
            return;
        }
        add_line(line);
        ++count;
    });

    // These two aren't listed: what they say is what is being checked.
    StringView const seal_files[] = { "/System/Library/Seal/Manifest"_sv, "/System/Library/Seal/Manifest.sig"_sv };
    for (auto path : seal_files) {
        auto resolved = VirtualFileSystem::resolve(path, "/"_sv, false);
        if (resolved.is_error())
            broken("something is missing"_sv, path);
        auto* entry = new Entry;
        entry->size = resolved.value().inode->metadata().size;
        entry->state.store(static_cast<u8>(ContentState::Good));
        s_building->set(resolved.value().inode->index(), entry);
    }
    s_entries = s_building;

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

    s_device = MUST(String::from_view(s_file_system->device_name()));
    dmesgln("Seal: {} entries, {} of them programs, all as signed", count, code.size());
}

bool is_active()
{
    return s_entries != nullptr;
}

bool is_sealed(Inode const& inode)
{
    auto const* entry = entry_for(inode);
    return entry && entry->kind != Kind::Open;
}

ErrorOr<void> check_may_change(Inode const& inode)
{
    if (is_sealed(inode))
        return Error::from_errno(EROFS);
    return {};
}

ErrorOr<void> check_child(Inode const& directory, Inode const& child)
{
    auto const* parent = entry_for(directory);
    if (!parent || parent->kind != Kind::Directory)
        return {};
    if (!entry_for(child))
        return Error::from_errno(ENOENT);
    return {};
}

ErrorOr<void> check_contents(Inode& inode)
{
    auto* entry = entry_for(inode);
    if (!entry || entry->kind != Kind::File)
        return {};
    auto state = static_cast<ContentState>(entry->state.load());
    if (state == ContentState::Unchecked) {
        // Two at once only do the work twice.
        constexpr size_t chunk_size = 256 * KiB;
        auto* chunk = static_cast<u8*>(kmalloc(chunk_size));
        if (!chunk)
            return Error::from_errno(ENOMEM);
        Crypto::SHA256 hasher;
        u64 offset = 0;
        ErrorOr<void> result = {};
        while (offset < entry->size) {
            auto got = inode.read(offset, min<u64>(chunk_size, entry->size - offset), IOBuffer::kernel(chunk));
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
        state = offset == entry->size && compare_hashes(digest.data(), entry->hash) == 0 ? ContentState::Good : ContentState::Bad;
        entry->state.store(static_cast<u8>(state));
        if (state == ContentState::Bad)
            dmesgln("Seal: inode {} isn't what the manifest says it is, so nothing may open it", inode.index());
    }
    if (state == ContentState::Bad)
        return Error::from_errno(EIO);
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
