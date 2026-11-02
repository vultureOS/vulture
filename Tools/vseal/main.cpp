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

// vseal key DIRECTORY HEADER
//     The system's signing key, made in DIRECTORY if it isn't there yet, and its public half as a C++
//     header, for the kernel. (A build that matters keeps that key somewhere safer than a build
//     directory, and passes the directory by VULTURE_SIGNING_DIR.)
//
// vseal seal ARCHIVE KEYFILE MANIFEST SIGNATURE
//     What the kernel checks the system against: for each thing under the sealed parts of the boot
//     archive, a line of what it is; and an Ed25519 signature of that (see Kernel/Security/Seal.h).
//
//     seal 1
//     d 0755 System                     a directory, with nothing in it but what is listed
//     f 0644 SIZE SHA256 etc/ssl/cert.pem   a file
//     x 0755 SIZE SHA256 bin/ls         the same, for a program or library: this is also what
//                                       may run, wherever a copy of it is
//     l SHA256 usr/x                    a symbolic link, by the hash of its target
//     o usr/local                       a directory left open, inside a sealed one
//
// Whatever is sealed, nobody can change while the system runs, root included.

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

// Sealed: these, whole. (Applications is open, for what gets installed; what the system came with
// in it is sealed, one application at a time.)
bool is_sealed_path(StringView path)
{
    auto is_under = [&](StringView root) {
        return path == root || (path.starts_with(root) && path.length() > root.length() && path[root.length()] == '/');
    };
    if (is_under("usr/local"_sv))
        return false;
    if (path.starts_with("Applications/"_sv))
        return true;
    return is_under("System"_sv) || is_under("bin"_sv) || is_under("sbin"_sv) || is_under("usr"_sv) || is_under("etc/ssl"_sv);
}

int make_seal(char const* archive_path, char const* key_path, char const* manifest_path, char const* signature_path)
{
    Vector<u8> archive;
    if (!read_file(archive_path, archive))
        return fail("can't read {}", StringView { archive_path });
    Vector<u8> seed;
    if (!read_file(key_path, seed) || seed.size() != Crypto::ed25519_seed_size)
        return fail("{} is not a key", StringView { key_path });
    auto reader = Archive::TarReader::parse(archive.span());
    if (reader.is_error())
        return fail("{} is not an archive", StringView { archive_path });

    StringBuilder manifest;
    manifest.append("seal 1\n"_sv);
    // What holds the seal is sealed too, as a directory; it can't list itself.
    manifest.append("d 0755 System/Library/Seal\n"_sv);
    size_t count = 0;
    auto result = reader.value().for_each_entry([&](Archive::TarEntry const& entry) {
        if (entry.path == "usr/local"_sv) {
            manifest.append("o usr/local\n"_sv);
            return Archive::IterationDecision::Continue;
        }
        if (!is_sealed_path(entry.path) || entry.path.starts_with("System/Library/Seal"_sv))
            return Archive::IterationDecision::Continue;
        ++count;
        u32 const mode = entry.mode & 07777;
        switch (entry.type) {
        case Archive::TarEntryType::Directory:
            manifest.appendff("d {:04o} {}\n", mode, entry.path);
            break;
        case Archive::TarEntryType::File: {
            auto const digest = Crypto::SHA256::hash(entry.data);
            bool const is_code = entry.data.size() >= 4 && entry.data[0] == 0x7f && entry.data[1] == 'E' && entry.data[2] == 'L' && entry.data[3] == 'F';
            manifest.appendff("{} {:04o} {} ", is_code ? "x"_sv : "f"_sv, mode, entry.data.size());
            append_hex(manifest, digest.span());
            manifest.appendff(" {}\n", entry.path);
            break;
        }
        case Archive::TarEntryType::SymbolicLink: {
            auto const digest = Crypto::SHA256::hash({ reinterpret_cast<u8 const*>(entry.link_target.characters_without_null_termination()), entry.link_target.length() });
            manifest.append("l "_sv);
            append_hex(manifest, digest.span());
            manifest.appendff(" {}\n", entry.path);
            break;
        }
        case Archive::TarEntryType::Other:
            break;
        }
        return Archive::IterationDecision::Continue;
    });
    if (result.is_error())
        return fail("can't go through {}", StringView { archive_path });

    auto const text = manifest.view();
    ReadonlyBytes const bytes { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() };
    u8 signature[Crypto::ed25519_signature_size];
    Crypto::ed25519_sign(seed.data(), bytes, signature);
    if (!write_file(manifest_path, bytes))
        return fail("can't write {}", StringView { manifest_path });
    if (!write_file(signature_path, { signature, sizeof(signature) }))
        return fail("can't write {}", StringView { signature_path });
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc == 4 && StringView { argv[1] } == "key"_sv)
        return make_key(argv[2], argv[3]);
    if (argc == 6 && StringView { argv[1] } == "seal"_sv)
        return make_seal(argv[2], argv[3], argv[4], argv[5]);
    return fail("usage: vseal key DIRECTORY HEADER | vseal seal ARCHIVE KEYFILE MANIFEST SIGNATURE");
}
