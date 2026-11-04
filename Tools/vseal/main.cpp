/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibBase/Format.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/SHA2.h>
#include <LibVultureFS/Sealed.h>

// vseal key DIRECTORY HEADER
//     The system's signing key, made in DIRECTORY if it isn't there yet, and its public half as a C++
//     header, for the kernel. (A build that matters keeps that key somewhere safer than a build
//     directory, and passes the directory by VULTURE_SIGNING_DIR.)
//
// vseal list ARCHIVE MANIFEST
//     What the system's programs and libraries are, by their hashes: what may run, wherever a copy of it is.
//
//     seal 2
//     x SHA256 bin/ls
//
// vseal seal VOLUME KEYFILE IMAGE
//     A system image, from a volume made by mkvfs: a hash of each of its blocks, and an Ed25519 signature
//     of those (see LibVultureFS/Sealed.h, and Kernel/Security/Seal.h).

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
long write(int fd, void const* buffer, unsigned long count);
int chmod(char const* path, unsigned short mode);
int mkdir(char const* path, unsigned short mode);
}

namespace {

class StandardError final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(2, text.characters_without_null_termination(), text.length()); }
};

template<typename... Args>
int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    StandardError output;
    output.write("vseal: "_sv);
    format_to(output, format, args...);
    output.write("\n"_sv);
    return 1;
}

bool read_file(char const* path, Vector<u8>& bytes)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t const at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return true;
}

bool write_file(char const* path, ReadonlyBytes bytes)
{
    FILE* file = fopen(path, "wb");
    if (!file)
        return false;
    bool const written = bytes.is_empty() || fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    return written;
}

void append_hex(StringBuilder& builder, ReadonlyBytes bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
        char const pair[2] = { digits[bytes[i] >> 4], digits[bytes[i] & 15] };
        builder.append(StringView { pair, 2 });
    }
}

int make_key(char const* directory, char const* header_path)
{
    StringBuilder key_path;
    key_path.appendff("{}/system.key", StringView { directory });
    auto key_file = MUST(key_path.to_string());
    (void)mkdir(directory, 0700);

    Vector<u8> seed;
    if (!read_file(key_file.characters(), seed) || seed.size() != Crypto::ed25519_seed_size) {
        seed.clear();
        MUST(seed.try_resize(Crypto::ed25519_seed_size));
        FILE* random = fopen("/dev/urandom", "rb");
        if (!random || fread(seed.data(), 1, seed.size(), random) != seed.size())
            return fail("can't read /dev/urandom");
        fclose(random);
        if (!write_file(key_file.characters(), seed.span()))
            return fail("can't write {}", key_file.view());
        chmod(key_file.characters(), 0600);
    }
    u8 public_key[Crypto::ed25519_public_key_size];
    Crypto::ed25519_public_key(seed.data(), public_key);

    StringBuilder header;
    header.append("// Made by vseal: the public half of the key the system's seal is signed with.\n"_sv);
    header.append("#pragma once\n\nstatic constexpr u8 system_public_key[32] = {"_sv);
    for (size_t i = 0; i < sizeof(public_key); ++i)
        header.appendff("{}{:#04x}", i ? ", "_sv : " "_sv, static_cast<u32>(public_key[i]));
    header.append(" };\n"_sv);

    // Left alone if it says the same, so that the kernel isn't built again for nothing.
    auto const text = header.view();
    Vector<u8> existing;
    if (read_file(header_path, existing) && existing.size() == text.length()
        && __builtin_memcmp(existing.data(), text.characters_without_null_termination(), text.length()) == 0)
        return 0;
    if (!write_file(header_path, { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() }))
        return fail("can't write {}", StringView { header_path });
    return 0;
}

int make_list(char const* archive_path, char const* manifest_path)
{
    Vector<u8> archive;
    if (!read_file(archive_path, archive))
        return fail("can't read {}", StringView { archive_path });
    auto reader = Archive::TarReader::parse(archive.span());
    if (reader.is_error())
        return fail("{} is not an archive", StringView { archive_path });

    StringBuilder manifest;
    manifest.append("seal 2\n"_sv);
    auto result = reader.value().for_each_entry([&](Archive::TarEntry const& entry) {
        if (entry.type != Archive::TarEntryType::File)
            return Archive::IterationDecision::Continue;
        bool const is_code = entry.data.size() >= 4 && entry.data[0] == 0x7f && entry.data[1] == 'E' && entry.data[2] == 'L' && entry.data[3] == 'F';
        if (is_code) {
            auto const digest = Crypto::SHA256::hash(entry.data);
            manifest.append("x "_sv);
            append_hex(manifest, digest.span());
            manifest.appendff(" {}\n", entry.path);
        }
        return Archive::IterationDecision::Continue;
    });
    if (result.is_error())
        return fail("can't go through {}", StringView { archive_path });
    auto const text = manifest.view();
    if (!write_file(manifest_path, { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() }))
        return fail("can't write {}", StringView { manifest_path });
    return 0;
}

int make_image(char const* volume_path, char const* key_path, char const* image_path)
{
    Vector<u8> volume;
    if (!read_file(volume_path, volume) || volume.size() == 0 || volume.size() % VultureFS::block_size != 0)
        return fail("{} isn't a volume", StringView { volume_path });
    Vector<u8> seed;
    if (!read_file(key_path, seed) || seed.size() != Crypto::ed25519_seed_size)
        return fail("{} is not a key", StringView { key_path });

    u64 const volume_blocks = volume.size() / VultureFS::block_size;
    u64 const hash_blocks = VultureFS::seal_hash_blocks(volume_blocks);
    Vector<u8> hashes;
    MUST(hashes.try_resize(hash_blocks * VultureFS::block_size));
    __builtin_memset(hashes.data(), 0, hashes.size());
    for (u64 block = 0; block < volume_blocks; ++block) {
        auto const digest = Crypto::SHA256::hash({ volume.data() + block * VultureFS::block_size, VultureFS::block_size });
        __builtin_memcpy(hashes.data() + block * VultureFS::seal_hash_size, digest.data(), VultureFS::seal_hash_size);
    }

    u8 block[VultureFS::block_size] {};
    auto* header = reinterpret_cast<VultureFS::SealHeader*>(block);
    __builtin_memcpy(header->magic, VultureFS::seal_magic, sizeof(header->magic));
    header->volume_blocks = volume_blocks;
    header->hash_blocks = hash_blocks;
    auto const of_hashes = Crypto::SHA256::hash(hashes.span());
    __builtin_memcpy(header->hash_of_hashes, of_hashes.data(), sizeof(header->hash_of_hashes));
    Crypto::ed25519_sign(seed.data(), { block, VultureFS::seal_signed_size() }, header->signature);

    FILE* image = fopen(image_path, "wb");
    if (!image)
        return fail("can't write {}", StringView { image_path });
    bool const written = fwrite(block, 1, sizeof(block), image) == sizeof(block)
        && fwrite(hashes.data(), 1, hashes.size(), image) == hashes.size()
        && fwrite(volume.data(), 1, volume.size(), image) == volume.size();
    fclose(image);
    return written ? 0 : fail("can't write {}", StringView { image_path });
}

}

int main(int argc, char** argv)
{
    if (argc == 4 && StringView { argv[1] } == "key"_sv)
        return make_key(argv[2], argv[3]);
    if (argc == 4 && StringView { argv[1] } == "list"_sv)
        return make_list(argv[2], argv[3]);
    if (argc == 5 && StringView { argv[1] } == "seal"_sv)
        return make_image(argv[2], argv[3], argv[4]);
    return fail("usage: vseal key DIRECTORY HEADER | vseal list ARCHIVE MANIFEST | vseal seal VOLUME KEYFILE IMAGE");
}
