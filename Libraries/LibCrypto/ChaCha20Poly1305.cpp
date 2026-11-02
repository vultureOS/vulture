/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibCrypto/ChaCha20Poly1305.h>
#include <LibCrypto/HMAC.h>

namespace Crypto {

namespace {

inline u32 load_le32(u8 const* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

inline void store_le32(u8* bytes, u32 value)
{
    bytes[0] = static_cast<u8>(value);
    bytes[1] = static_cast<u8>(value >> 8);
    bytes[2] = static_cast<u8>(value >> 16);
    bytes[3] = static_cast<u8>(value >> 24);
}

inline u32 rotate_left(u32 value, int by)
{
    return (value << by) | (value >> (32 - by));
}

inline void quarter_round(u32& a, u32& b, u32& c, u32& d)
{
    a += b;
    d = rotate_left(d ^ a, 16);
    c += d;
    b = rotate_left(b ^ c, 12);
    a += b;
    d = rotate_left(d ^ a, 8);
    c += d;
    b = rotate_left(b ^ c, 7);
}

void apply_keystream(u8 const* key, u32 counter, u8 const* nonce, ReadonlyBytes input, u8* output)
{
    size_t done = 0;
    while (done < input.size()) {
        u8 block[64];
        chacha20_block(key, counter++, nonce, block);
        size_t take = input.size() - done < 64 ? input.size() - done : 64;
        for (size_t i = 0; i < take; ++i)
            output[done + i] = input[done + i] ^ block[i];
        done += take;
    }
    (void)counter;
}

class Poly1305State {
public:
    explicit Poly1305State(u8 const* key)
    {
        // r, clamped, in limbs of 26 bits.
        m_r[0] = load_le32(key) & 0x3ffffff;
        m_r[1] = (load_le32(key + 3) >> 2) & 0x3ffff03;
        m_r[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ff;
        m_r[3] = (load_le32(key + 9) >> 6) & 0x3f03fff;
        m_r[4] = (load_le32(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; ++i)
            m_pad[i] = load_le32(key + 16 + i * 4);
    }

    // A block of 16 bytes, or fewer at the end (then with the 1 right after them).
    void absorb(u8 const* bytes, size_t length)
    {
        u8 block[17] = {};
        __builtin_memcpy(block, bytes, length);
        block[length] = 1;
        m_h[0] += load_le32(block) & 0x3ffffff;
        m_h[1] += (load_le32(block + 3) >> 2) & 0x3ffffff;
        m_h[2] += (load_le32(block + 6) >> 4) & 0x3ffffff;
        m_h[3] += (load_le32(block + 9) >> 6) & 0x3ffffff;
        m_h[4] += (load_le32(block + 12) >> 8) | (static_cast<u32>(block[16]) << 24);

        u64 r0 = m_r[0], r1 = m_r[1], r2 = m_r[2], r3 = m_r[3], r4 = m_r[4];
        u64 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        u64 h0 = m_h[0], h1 = m_h[1], h2 = m_h[2], h3 = m_h[3], h4 = m_h[4];
        u64 d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
        u64 d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
        u64 d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
        u64 d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
        u64 d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
        u64 carry;
        carry = d0 >> 26;
        m_h[0] = static_cast<u32>(d0) & 0x3ffffff;
        d1 += carry;
        carry = d1 >> 26;
        m_h[1] = static_cast<u32>(d1) & 0x3ffffff;
        d2 += carry;
        carry = d2 >> 26;
        m_h[2] = static_cast<u32>(d2) & 0x3ffffff;
        d3 += carry;
        carry = d3 >> 26;
        m_h[3] = static_cast<u32>(d3) & 0x3ffffff;
        d4 += carry;
        carry = d4 >> 26;
        m_h[4] = static_cast<u32>(d4) & 0x3ffffff;
        m_h[0] += static_cast<u32>(carry * 5);
        m_h[1] += m_h[0] >> 26;
        m_h[0] &= 0x3ffffff;
    }

    void finish(u8* tag)
    {
        // Carry all the way, then h - p, kept if that didn't go below zero.
        u32 h0 = m_h[0], h1 = m_h[1], h2 = m_h[2], h3 = m_h[3], h4 = m_h[4];
        u32 carry = h1 >> 26;
        h1 &= 0x3ffffff;
        h2 += carry;
        carry = h2 >> 26;
        h2 &= 0x3ffffff;
        h3 += carry;
        carry = h3 >> 26;
        h3 &= 0x3ffffff;
        h4 += carry;
        carry = h4 >> 26;
        h4 &= 0x3ffffff;
        h0 += carry * 5;
        carry = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += carry;

        u32 g0 = h0 + 5;
        carry = g0 >> 26;
        g0 &= 0x3ffffff;
        u32 g1 = h1 + carry;
        carry = g1 >> 26;
        g1 &= 0x3ffffff;
        u32 g2 = h2 + carry;
        carry = g2 >> 26;
        g2 &= 0x3ffffff;
        u32 g3 = h3 + carry;
        carry = g3 >> 26;
        g3 &= 0x3ffffff;
        u32 g4 = h4 + carry - (1u << 26);

        u32 keep_g = (g4 >> 31) - 1;
        u32 keep_h = ~keep_g;
        h0 = (h0 & keep_h) | (g0 & keep_g);
        h1 = (h1 & keep_h) | (g1 & keep_g);
        h2 = (h2 & keep_h) | (g2 & keep_g);
        h3 = (h3 & keep_h) | (g3 & keep_g);
        h4 = (h4 & keep_h) | (g4 & keep_g);

        // As four 32-bit words, plus the pad.
        u32 w0 = h0 | (h1 << 26);
        u32 w1 = (h1 >> 6) | (h2 << 20);
        u32 w2 = (h2 >> 12) | (h3 << 14);
        u32 w3 = (h3 >> 18) | (h4 << 8);
        u64 sum = static_cast<u64>(w0) + m_pad[0];
        store_le32(tag, static_cast<u32>(sum));
        sum = (sum >> 32) + w1 + m_pad[1];
        store_le32(tag + 4, static_cast<u32>(sum));
        sum = (sum >> 32) + w2 + m_pad[2];
        store_le32(tag + 8, static_cast<u32>(sum));
        sum = (sum >> 32) + w3 + m_pad[3];
        store_le32(tag + 12, static_cast<u32>(sum));
    }

private:
    u32 m_r[5] {};
    u32 m_h[5] {};
    u32 m_pad[4] {};
};

}

void chacha20_block(u8 const* key, u32 counter, u8 const* nonce, u8* output)
{
    u32 initial[16] = {
        0x61707865,
        0x3320646e,
        0x79622d32,
        0x6b206574,
        load_le32(key),
        load_le32(key + 4),
        load_le32(key + 8),
        load_le32(key + 12),
        load_le32(key + 16),
        load_le32(key + 20),
        load_le32(key + 24),
        load_le32(key + 28),
        counter,
        load_le32(nonce),
        load_le32(nonce + 4),
        load_le32(nonce + 8),
    };
    u32 x[16];
    __builtin_memcpy(x, initial, sizeof(x));
    for (int round = 0; round < 10; ++round) {
        quarter_round(x[0], x[4], x[8], x[12]);
        quarter_round(x[1], x[5], x[9], x[13]);
        quarter_round(x[2], x[6], x[10], x[14]);
        quarter_round(x[3], x[7], x[11], x[15]);
        quarter_round(x[0], x[5], x[10], x[15]);
        quarter_round(x[1], x[6], x[11], x[12]);
        quarter_round(x[2], x[7], x[8], x[13]);
        quarter_round(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i)
        store_le32(output + i * 4, x[i] + initial[i]);
}

void poly1305(u8 const* key, ReadonlyBytes message, u8* tag)
{
    Poly1305State state(key);
    size_t at = 0;
    while (at < message.size()) {
        size_t take = message.size() - at < 16 ? message.size() - at : 16;
        state.absorb(message.data() + at, take);
        at += take;
    }
    state.finish(tag);
}

ChaCha20Poly1305::ChaCha20Poly1305(ReadonlyBytes key)
{
    VERIFY(key.size() == sizeof(m_key));
    __builtin_memcpy(m_key, key.data(), sizeof(m_key));
}

ChaCha20Poly1305::~ChaCha20Poly1305()
{
    __builtin_memset(m_key, 0, sizeof(m_key));
}

void ChaCha20Poly1305::compute_tag(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8* tag) const
{
    u8 block[64];
    chacha20_block(m_key, 0, nonce, block);
    Poly1305State state(block);
    __builtin_memset(block, 0, sizeof(block));

    auto absorb_padded = [&](ReadonlyBytes bytes) {
        size_t at = 0;
        while (at < bytes.size()) {
            u8 padded[16] = {};
            size_t take = bytes.size() - at < 16 ? bytes.size() - at : 16;
            __builtin_memcpy(padded, bytes.data() + at, take);
            state.absorb(padded, 16);
            at += take;
        }
    };
    absorb_padded(associated_data);
    absorb_padded(ciphertext);
    u8 lengths[16];
    u64 ad_length = associated_data.size();
    u64 text_length = ciphertext.size();
    for (int i = 0; i < 8; ++i) {
        lengths[i] = static_cast<u8>(ad_length >> (i * 8));
        lengths[8 + i] = static_cast<u8>(text_length >> (i * 8));
    }
    state.absorb(lengths, 16);
    state.finish(tag);
}

void ChaCha20Poly1305::seal(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes plaintext, u8* output, u8* tag) const
{
    apply_keystream(m_key, 1, nonce, plaintext, output);
    compute_tag(nonce, associated_data, { output, plaintext.size() }, tag);
}

bool ChaCha20Poly1305::open(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8 const* tag, u8* output) const
{
    u8 expected[16];
    compute_tag(nonce, associated_data, ciphertext, expected);
    if (!constant_time_equal({ expected, 16 }, { tag, 16 })) {
        __builtin_memset(output, 0, ciphertext.size());
        return false;
    }
    apply_keystream(m_key, 1, nonce, ciphertext, output);
    return true;
}

}
