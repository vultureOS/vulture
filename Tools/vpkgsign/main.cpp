/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Format.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibCrypto/Curve25519.h>

// vpkgsign keygen KEYFILE PUBLICFILE   makes a new key (keep the first of them secret)
// vpkgsign sign KEYFILE FILE SIGFILE   signs a file
// vpkgsign verify PUBLICFILE FILE SIGFILE
//
// Ed25519, keys and signatures as hexadecimal text: what Meta/vpkg.py signs a repository with.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
long write(int fd, void const* buffer, unsigned long count);
int chmod(char const* path, unsigned short mode);
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

bool write_hex(char const* path, ReadonlyBytes bytes)
{
    FILE* file = fopen(path, "wb");
    if (!file)
        return false;
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
        char const pair[2] = { digits[bytes[i] >> 4], digits[bytes[i] & 15] };
        fwrite(pair, 1, 2, file);
    }
    fwrite("\n", 1, 1, file);
    fclose(file);
    return true;
}

bool read_hex(char const* path, Bytes output)
{
    Vector<u8> text;
    if (!read_file(path, text))
        return false;
    while (!text.is_empty() && (text.last() == '\n' || text.last() == '\r' || text.last() == ' '))
        (void)text.take_last();
    if (text.size() != output.size() * 2)
        return false;
    for (size_t i = 0; i < output.size(); ++i) {
        auto digit = [](u8 c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        };
        int const high = digit(text[i * 2]);
        int const low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<u8>(high << 4 | low);
    }
    return true;
}

}

int main(int argc, char** argv)
{
    if (argc >= 2 && StringView { argv[1] } == "keygen"_sv && argc == 4) {
        u8 seed[Crypto::ed25519_seed_size];
        FILE* random = fopen("/dev/urandom", "rb");
        if (!random || fread(seed, 1, sizeof(seed), random) != sizeof(seed))
            return fail("vpkgsign: can't read /dev/urandom");
        fclose(random);
        u8 key[Crypto::ed25519_public_key_size];
        Crypto::ed25519_public_key(seed, key);
        if (!write_hex(argv[2], { seed, sizeof(seed) }) || !write_hex(argv[3], { key, sizeof(key) }))
            return fail("vpkgsign: can't write the keys");
        chmod(argv[2], 0600);
        return 0;
    }
    if (argc == 5 && StringView { argv[1] } == "sign"_sv) {
        u8 seed[Crypto::ed25519_seed_size];
        if (!read_hex(argv[2], { seed, sizeof(seed) }))
            return fail("vpkgsign: {} is not a key", StringView { argv[2] });
        Vector<u8> bytes;
        if (!read_file(argv[3], bytes))
            return fail("vpkgsign: can't read {}", StringView { argv[3] });
        u8 signature[Crypto::ed25519_signature_size];
        Crypto::ed25519_sign(seed, bytes.span(), signature);
        if (!write_hex(argv[4], { signature, sizeof(signature) }))
            return fail("vpkgsign: can't write {}", StringView { argv[4] });
        return 0;
    }
    if (argc == 5 && StringView { argv[1] } == "verify"_sv) {
        u8 key[Crypto::ed25519_public_key_size];
        u8 signature[Crypto::ed25519_signature_size];
        if (!read_hex(argv[2], { key, sizeof(key) }) || !read_hex(argv[4], { signature, sizeof(signature) }))
            return fail("vpkgsign: can't read the key or the signature");
        Vector<u8> bytes;
        if (!read_file(argv[3], bytes))
            return fail("vpkgsign: can't read {}", StringView { argv[3] });
        if (!Crypto::ed25519_verify(key, bytes.span(), signature))
            return fail("vpkgsign: the signature is bad");
        return 0;
    }
    return fail("usage: vpkgsign keygen KEYFILE PUBLICFILE | sign KEYFILE FILE SIGFILE | verify PUBLICFILE FILE SIGFILE");
}
