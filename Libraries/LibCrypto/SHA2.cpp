/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/SHA2.h>

namespace Crypto {

namespace {

constexpr u32 rotate_right(u32 value, unsigned count) { return (value >> count) | (value << (32 - count)); }
constexpr u64 rotate_right(u64 value, unsigned count) { return (value >> count) | (value << (64 - count)); }

constexpr u32 s_sha256_rounds[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr u64 s_sha512_rounds[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

}

void SHA256::reset()
{
    static constexpr u32 initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    for (size_t i = 0; i < 8; ++i)
        m_state[i] = initial[i];
    m_buffered = 0;
    m_length = 0;
}

void SHA256::transform(u8 const* block)
{
    u32 w[64];
    for (size_t i = 0; i < 16; ++i)
        w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) | (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
    for (size_t i = 16; i < 64; ++i) {
        u32 s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (size_t i = 0; i < 64; ++i) {
        u32 s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        u32 choice = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + choice + s_sha256_rounds[i] + w[i];
        u32 s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        u32 majority = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void SHA256::update(ReadonlyBytes data)
{
    m_length += data.size();
    size_t offset = 0;
    while (offset < data.size()) {
        size_t take = block_size - m_buffered;
        if (take > data.size() - offset)
            take = data.size() - offset;
        __builtin_memcpy(m_buffer + m_buffered, data.data() + offset, take);
        m_buffered += take;
        offset += take;
        if (m_buffered == block_size) {
            transform(m_buffer);
            m_buffered = 0;
        }
    }
}

SHA256::Digest SHA256::digest()
{
    u64 bits = m_length * 8;
    u8 padding[block_size * 2] = { 0x80 };
    size_t padding_length = (m_buffered < 56 ? 56 : 120) - m_buffered;
    update({ padding, padding_length });
    u8 length_bytes[8];
    for (size_t i = 0; i < 8; ++i)
        length_bytes[i] = static_cast<u8>(bits >> (56 - i * 8));
    update({ length_bytes, 8 });
    Digest digest {};
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<u8>(m_state[i] >> (24 - j * 8));
    }
    reset();
    return digest;
}

void SHA512::reset()
{
    static constexpr u64 initial[8] = { 0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1, 0x510e527fade682d1,
        0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179 };
    for (size_t i = 0; i < 8; ++i)
        m_state[i] = initial[i];
    m_buffered = 0;
    m_length = 0;
}

void SHA512::transform(u8 const* block)
{
    u64 w[80];
    for (size_t i = 0; i < 16; ++i) {
        u64 word = 0;
        for (size_t j = 0; j < 8; ++j)
            word = (word << 8) | block[i * 8 + j];
        w[i] = word;
    }
    for (size_t i = 16; i < 80; ++i) {
        u64 s0 = rotate_right(w[i - 15], 1) ^ rotate_right(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = rotate_right(w[i - 2], 19) ^ rotate_right(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u64 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (size_t i = 0; i < 80; ++i) {
        u64 s1 = rotate_right(e, 14) ^ rotate_right(e, 18) ^ rotate_right(e, 41);
        u64 choice = (e & f) ^ (~e & g);
        u64 t1 = h + s1 + choice + s_sha512_rounds[i] + w[i];
        u64 s0 = rotate_right(a, 28) ^ rotate_right(a, 34) ^ rotate_right(a, 39);
        u64 majority = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void SHA512::update(ReadonlyBytes data)
{
    m_length += data.size();
    size_t offset = 0;
    while (offset < data.size()) {
        size_t take = block_size - m_buffered;
        if (take > data.size() - offset)
            take = data.size() - offset;
        __builtin_memcpy(m_buffer + m_buffered, data.data() + offset, take);
        m_buffered += take;
        offset += take;
        if (m_buffered == block_size) {
            transform(m_buffer);
            m_buffered = 0;
        }
    }
}

SHA512::Digest SHA512::digest()
{
    u64 bits = m_length * 8;
    u8 padding[block_size * 2] = { 0x80 };
    size_t padding_length = (m_buffered < 112 ? 112 : 240) - m_buffered;
    update({ padding, padding_length });
    // A 128-bit length; the high half is zero.
    u8 length_bytes[16] = {};
    for (size_t i = 0; i < 8; ++i)
        length_bytes[8 + i] = static_cast<u8>(bits >> (56 - i * 8));
    update({ length_bytes, 16 });
    Digest digest {};
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = 0; j < 8; ++j)
            digest[i * 8 + j] = static_cast<u8>(m_state[i] >> (56 - j * 8));
    }
    reset();
    return digest;
}

}
