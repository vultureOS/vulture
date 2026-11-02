/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/GCM.h>
#include <LibCrypto/HMAC.h>
#include <LibCrypto/ProcessorFeatures.h>

namespace Crypto {

namespace {

inline u64 load_be64(u8 const* bytes)
{
    u64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

inline void store_be64(u8* bytes, u64 value)
{
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<u8>(value);
        value >>= 8;
    }
}

// Carry-less multiplication of 64 bits by 64, with no branch or table a secret could show
// through: each of four lanes holds every fourth bit, so that carries from the integer
// multiply land in bits that are thrown away.
inline void multiply_without_carries(u64 x, u64 y, u64& high, u64& low)
{
    constexpr u64 m1 = 0x1111111111111111ull;
    constexpr u64 m2 = 0x2222222222222222ull;
    constexpr u64 m4 = 0x4444444444444444ull;
    constexpr u64 m8 = 0x8888888888888888ull;
    auto multiply = [](u64 a, u64 b) -> unsigned __int128 { return static_cast<unsigned __int128>(a) * b; };
    u64 x1 = x & m1, x2 = x & m2, x4 = x & m4, x8 = x & m8;
    u64 y1 = y & m1, y2 = y & m2, y4 = y & m4, y8 = y & m8;
    unsigned __int128 z1 = multiply(x1, y1) ^ multiply(x2, y8) ^ multiply(x4, y4) ^ multiply(x8, y2);
    unsigned __int128 z2 = multiply(x1, y2) ^ multiply(x2, y1) ^ multiply(x4, y8) ^ multiply(x8, y4);
    unsigned __int128 z4 = multiply(x1, y4) ^ multiply(x2, y2) ^ multiply(x4, y1) ^ multiply(x8, y8);
    unsigned __int128 z8 = multiply(x1, y8) ^ multiply(x2, y4) ^ multiply(x4, y2) ^ multiply(x8, y1);
    constexpr unsigned __int128 lane1 = (static_cast<unsigned __int128>(m1) << 64) | m1;
    constexpr unsigned __int128 lane2 = (static_cast<unsigned __int128>(m2) << 64) | m2;
    constexpr unsigned __int128 lane4 = (static_cast<unsigned __int128>(m4) << 64) | m4;
    constexpr unsigned __int128 lane8 = (static_cast<unsigned __int128>(m8) << 64) | m8;
    unsigned __int128 z = (z1 & lane1) | (z2 & lane2) | (z4 & lane4) | (z8 & lane8);
    high = static_cast<u64>(z >> 64);
    low = static_cast<u64>(z);
}

// Y := Y * H in GF(2^128), GCM's way around: bit 0 is the top bit of the first byte.
void multiply_in_field_software(u64& y_high, u64& y_low, u64 h_high, u64 h_low)
{
    // Karatsuba's three products.
    u64 a1, a0, b1, b0, c1, c0;
    multiply_without_carries(y_high, h_high, a1, a0);
    multiply_without_carries(y_low, h_low, b1, b0);
    multiply_without_carries(y_high ^ y_low, h_high ^ h_low, c1, c0);
    c1 ^= a1 ^ b1;
    c0 ^= a0 ^ b0;
    u64 z3 = a1;
    u64 z2 = a0 ^ c1;
    u64 z1 = b1 ^ c0;
    u64 z0 = b0;
    // With bits reversed in meaning, the product is one place too low.
    z3 = (z3 << 1) | (z2 >> 63);
    z2 = (z2 << 1) | (z1 >> 63);
    z1 = (z1 << 1) | (z0 >> 63);
    z0 <<= 1;
    // Reduced by x^128 + x^7 + x^2 + x + 1.
    z1 ^= (z0 << 63) ^ (z0 << 62) ^ (z0 << 57);
    z2 ^= z0 ^ (z0 >> 1) ^ (z0 >> 2) ^ (z0 >> 7);
    z2 ^= (z1 << 63) ^ (z1 << 62) ^ (z1 << 57);
    z3 ^= z1 ^ (z1 >> 1) ^ (z1 >> 2) ^ (z1 >> 7);
    y_high = z3;
    y_low = z2;
}

#if VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS

using Register128 = unsigned long long __attribute__((vector_size(16)));

[[gnu::target("aes")]] inline void multiply_with_pmull(u64 x, u64 y, u64& high, u64& low)
{
    Register128 a { x, 0 };
    Register128 b { y, 0 };
    Register128 result;
    asm("pmull %0.1q, %1.1d, %2.1d" : "=w"(result) : "w"(a), "w"(b));
    low = result[0];
    high = result[1];
}

[[gnu::target("aes")]] void multiply_in_field_hardware(u64& y_high, u64& y_low, u64 h_high, u64 h_low)
{
    u64 a1, a0, b1, b0, c1, c0;
    multiply_with_pmull(y_high, h_high, a1, a0);
    multiply_with_pmull(y_low, h_low, b1, b0);
    multiply_with_pmull(y_high ^ y_low, h_high ^ h_low, c1, c0);
    c1 ^= a1 ^ b1;
    c0 ^= a0 ^ b0;
    u64 z3 = a1;
    u64 z2 = a0 ^ c1;
    u64 z1 = b1 ^ c0;
    u64 z0 = b0;
    z3 = (z3 << 1) | (z2 >> 63);
    z2 = (z2 << 1) | (z1 >> 63);
    z1 = (z1 << 1) | (z0 >> 63);
    z0 <<= 1;
    z1 ^= (z0 << 63) ^ (z0 << 62) ^ (z0 << 57);
    z2 ^= z0 ^ (z0 >> 1) ^ (z0 >> 2) ^ (z0 >> 7);
    z2 ^= (z1 << 63) ^ (z1 << 62) ^ (z1 << 57);
    z3 ^= z1 ^ (z1 >> 1) ^ (z1 >> 2) ^ (z1 >> 7);
    y_high = z3;
    y_low = z2;
}

inline bool have_pmull()
{
    if (!AES::has_hardware())
        return false;
    static bool const found = getauxval(auxiliary_hardware_capabilities) & capability_pmull;
    return found;
}

#endif

inline void multiply_in_field(u64& y_high, u64& y_low, u64 h_high, u64 h_low)
{
#if VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS
    if (have_pmull()) {
        multiply_in_field_hardware(y_high, y_low, h_high, h_low);
        return;
    }
#endif
    multiply_in_field_software(y_high, y_low, h_high, h_low);
}

}

AESGCM::AESGCM(ReadonlyBytes key)
    : m_cipher(key)
{
    u8 zeroes[16] = {};
    u8 h[16];
    m_cipher.encrypt_block(zeroes, h);
    m_h_high = load_be64(h);
    m_h_low = load_be64(h + 8);
}

AESGCM::~AESGCM()
{
    m_h_high = 0;
    m_h_low = 0;
}

void AESGCM::apply_counter_mode(u8 const* nonce, ReadonlyBytes input, u8* output) const
{
    u8 counter[16];
    __builtin_memcpy(counter, nonce, aead_nonce_size);
    u32 block_number = 2;
    size_t done = 0;
    while (done < input.size()) {
        counter[12] = static_cast<u8>(block_number >> 24);
        counter[13] = static_cast<u8>(block_number >> 16);
        counter[14] = static_cast<u8>(block_number >> 8);
        counter[15] = static_cast<u8>(block_number);
        ++block_number;
        u8 stream[16];
        m_cipher.encrypt_block(counter, stream);
        size_t take = input.size() - done < 16 ? input.size() - done : 16;
        for (size_t i = 0; i < take; ++i)
            output[done + i] = input[done + i] ^ stream[i];
        done += take;
    }
}

void AESGCM::compute_tag(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8* tag) const
{
    u64 y_high = 0;
    u64 y_low = 0;
    auto absorb = [&](ReadonlyBytes bytes) {
        size_t at = 0;
        while (at < bytes.size()) {
            u8 block[16] = {};
            size_t take = bytes.size() - at < 16 ? bytes.size() - at : 16;
            __builtin_memcpy(block, bytes.data() + at, take);
            y_high ^= load_be64(block);
            y_low ^= load_be64(block + 8);
            multiply_in_field(y_high, y_low, m_h_high, m_h_low);
            at += take;
        }
    };
    absorb(associated_data);
    absorb(ciphertext);
    y_high ^= static_cast<u64>(associated_data.size()) * 8;
    y_low ^= static_cast<u64>(ciphertext.size()) * 8;
    multiply_in_field(y_high, y_low, m_h_high, m_h_low);

    u8 counter[16];
    __builtin_memcpy(counter, nonce, aead_nonce_size);
    counter[12] = 0;
    counter[13] = 0;
    counter[14] = 0;
    counter[15] = 1;
    u8 mask[16];
    m_cipher.encrypt_block(counter, mask);
    store_be64(tag, y_high);
    store_be64(tag + 8, y_low);
    for (int i = 0; i < 16; ++i)
        tag[i] ^= mask[i];
}

void AESGCM::seal(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes plaintext, u8* output, u8* tag) const
{
    apply_counter_mode(nonce, plaintext, output);
    compute_tag(nonce, associated_data, { output, plaintext.size() }, tag);
}

bool AESGCM::open(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8 const* tag, u8* output) const
{
    u8 expected[16];
    compute_tag(nonce, associated_data, ciphertext, expected);
    if (!constant_time_equal({ expected, 16 }, { tag, 16 })) {
        __builtin_memset(output, 0, ciphertext.size());
        return false;
    }
    apply_counter_mode(nonce, ciphertext, output);
    return true;
}

}
