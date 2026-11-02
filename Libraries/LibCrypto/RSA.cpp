/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/HMAC.h>
#include <LibCrypto/RSA.h>
#include <LibCrypto/SHA2.h>

namespace Crypto {

namespace {

// The longest digest there is here: SHA-512's.
constexpr size_t largest_digest = SHA512::digest_size;

size_t digest_size_of(SignatureHash hash)
{
    switch (hash) {
    case SignatureHash::SHA256:
        return SHA256::digest_size;
    case SignatureHash::SHA384:
        return SHA384::digest_size;
    case SignatureHash::SHA512:
        return SHA512::digest_size;
    }
    return 0;
}

template<typename Hash>
void hash_pieces(ReadonlyBytes const* pieces, size_t count, u8* output)
{
    Hash hasher;
    for (size_t i = 0; i < count; ++i)
        hasher.update(pieces[i]);
    auto digest = hasher.digest();
    __builtin_memcpy(output, digest.data(), Hash::digest_size);
}

void hash_with(SignatureHash hash, ReadonlyBytes const* pieces, size_t count, u8* output)
{
    switch (hash) {
    case SignatureHash::SHA256:
        hash_pieces<SHA256>(pieces, count, output);
        return;
    case SignatureHash::SHA384:
        hash_pieces<SHA384>(pieces, count, output);
        return;
    case SignatureHash::SHA512:
        hash_pieces<SHA512>(pieces, count, output);
        return;
    }
}

// s^e mod n, as many bytes as the modulus. False if the signature is no number below n.
bool apply_public_key(RSAPublicKey const& key, ReadonlyBytes signature, Vector<u8>& output)
{
    // A key too small to mean anything, or too large to be worth the time an attacker
    // could make us spend on it.
    size_t bits = key.modulus.bit_length();
    if (bits < 1024 || bits > 8192 || !key.modulus.is_odd())
        return false;
    size_t size = (bits + 7) / 8;
    if (signature.size() != size)
        return false;
    auto number = BigInteger::from_bytes(signature);
    if (number.compare(key.modulus) >= 0)
        return false;
    auto result = key.modulus.modular_power_of(number, key.exponent);
    MUST(output.try_resize(size));
    return result.to_bytes({ output.data(), size });
}

}

bool rsa_verify_pkcs1(RSAPublicKey const& key, SignatureHash hash, ReadonlyBytes message, ReadonlyBytes signature)
{
    Vector<u8> encoded;
    if (!apply_public_key(key, signature, encoded))
        return false;

    static constexpr u8 prefix_sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static constexpr u8 prefix_sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static constexpr u8 prefix_sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    u8 const* prefix = hash == SignatureHash::SHA256 ? prefix_sha256 : (hash == SignatureHash::SHA384 ? prefix_sha384 : prefix_sha512);
    size_t const prefix_size = sizeof(prefix_sha256);
    size_t const digest_size = digest_size_of(hash);
    size_t const tail = prefix_size + digest_size;

    // 00 01 FF ... FF 00 prefix digest, with at least eight bytes of FF.
    if (encoded.size() < tail + 11)
        return false;
    u8 digest[largest_digest];
    hash_with(hash, &message, 1, digest);
    size_t const padding_end = encoded.size() - tail - 1;
    if (encoded[0] != 0x00 || encoded[1] != 0x01 || encoded[padding_end] != 0x00)
        return false;
    for (size_t i = 2; i < padding_end; ++i) {
        if (encoded[i] != 0xff)
            return false;
    }
    return constant_time_equal({ encoded.data() + padding_end + 1, prefix_size }, { prefix, prefix_size })
        && constant_time_equal({ encoded.data() + padding_end + 1 + prefix_size, digest_size }, { digest, digest_size });
}

bool rsa_verify_pss(RSAPublicKey const& key, SignatureHash hash, ReadonlyBytes message, ReadonlyBytes signature)
{
    Vector<u8> whole;
    if (!apply_public_key(key, signature, whole))
        return false;

    size_t const digest_size = digest_size_of(hash);
    size_t const em_bits = key.modulus.bit_length() - 1;
    size_t const em_length = (em_bits + 7) / 8;
    // The encoded message is em_bits long; when that is a whole byte shorter than the
    // modulus, the first byte of what came out must be zero.
    u8 const* em = whole.data() + (whole.size() - em_length);
    if (whole.size() != em_length && whole[0] != 0)
        return false;
    if (em_length < digest_size * 2 + 2 || em[em_length - 1] != 0xbc)
        return false;

    size_t const masked_length = em_length - digest_size - 1;
    u8 const* masked = em;
    u8 const* h = em + masked_length;
    u8 const top_mask = static_cast<u8>(0xff >> (em_length * 8 - em_bits));
    if (masked[0] & ~top_mask)
        return false;

    // MGF1.
    Vector<u8> db;
    MUST(db.try_resize(masked_length));
    size_t produced = 0;
    for (u32 counter = 0; produced < masked_length; ++counter) {
        u8 number[4] = { static_cast<u8>(counter >> 24), static_cast<u8>(counter >> 16), static_cast<u8>(counter >> 8), static_cast<u8>(counter) };
        ReadonlyBytes const pieces[] = { { h, digest_size }, { number, 4 } };
        u8 block[largest_digest];
        hash_with(hash, pieces, 2, block);
        size_t take = masked_length - produced < digest_size ? masked_length - produced : digest_size;
        for (size_t i = 0; i < take; ++i)
            db[produced + i] = masked[produced + i] ^ block[i];
        produced += take;
    }
    db[0] &= top_mask;

    size_t const salt_length = digest_size;
    size_t const zeroes = masked_length - salt_length - 1;
    for (size_t i = 0; i < zeroes; ++i) {
        if (db[i] != 0)
            return false;
    }
    if (db[zeroes] != 0x01)
        return false;

    u8 digest[largest_digest];
    hash_with(hash, &message, 1, digest);
    static constexpr u8 eight_zeroes[8] = {};
    ReadonlyBytes const pieces[] = { { eight_zeroes, 8 }, { digest, digest_size }, { db.data() + zeroes + 1, salt_length } };
    u8 expected[largest_digest];
    hash_with(hash, pieces, 3, expected);
    return constant_time_equal({ expected, digest_size }, { h, digest_size });
}

}
