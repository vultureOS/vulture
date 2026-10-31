/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

namespace {

constexpr u32 rotate_left(u32 value, int count) { return (value << count) | (value >> (32 - count)); }

void quarter_round(u32& a, u32& b, u32& c, u32& d)
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

// One 64-byte ChaCha20 block (RFC 8439) for `key`, `counter` and nonce 0.
void chacha20_block(u32 const (&key)[8], u64 counter, u8 (&output)[64])
{
    u32 state[16] = {
        0x6170'7865, 0x3320'646e, 0x7962'2d32, 0x6b20'6574,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        static_cast<u32>(counter), static_cast<u32>(counter >> 32), 0, 0
    };
    u32 working[16];
    for (int i = 0; i < 16; ++i)
        working[i] = state[i];
    for (int round = 0; round < 10; ++round) {
        quarter_round(working[0], working[4], working[8], working[12]);
        quarter_round(working[1], working[5], working[9], working[13]);
        quarter_round(working[2], working[6], working[10], working[14]);
        quarter_round(working[3], working[7], working[11], working[15]);
        quarter_round(working[0], working[5], working[10], working[15]);
        quarter_round(working[1], working[6], working[11], working[12]);
        quarter_round(working[2], working[7], working[8], working[13]);
        quarter_round(working[3], working[4], working[9], working[14]);
    }
    for (int i = 0; i < 16; ++i) {
        u32 word = working[i] + state[i];
        for (int byte = 0; byte < 4; ++byte)
            output[i * 4 + byte] = static_cast<u8>(word >> (8 * byte));
    }
}

SpinLock s_lock;
u32 s_key[8];
u64 s_counter;

// Folds `value` into the key (a cheap mix; ChaCha20 does the rest).
void mix(u64 value)
{
    for (int i = 0; i < 8; ++i) {
        s_key[i] ^= static_cast<u32>(value >> ((i % 2) * 32));
        s_key[i] = rotate_left(s_key[i] * 0x9e37'79b9u + static_cast<u32>(i), 13);
    }
}

}

void initialize_random(ReadonlyBytes firmware_entropy)
{
    SpinLockGuard guard(s_lock);
    for (size_t i = 0; i < firmware_entropy.size(); ++i)
        reinterpret_cast<u8*>(s_key)[i % sizeof(s_key)] ^= firmware_entropy[i];
    mix(Time::now());
}

void add_entropy(u64 value)
{
    SpinLockGuard guard(s_lock);
    mix(value);
}

void get_random_bytes(Bytes output)
{
    size_t done = 0;
    while (done < output.size()) {
        u8 block[64];
        {
            SpinLockGuard guard(s_lock);
            mix(Time::now());
            chacha20_block(s_key, s_counter++, block);
            // The first 32 bytes become the next key; the rest is output.
            for (int i = 0; i < 8; ++i)
                s_key[i] = static_cast<u32>(block[i * 4]) | (static_cast<u32>(block[i * 4 + 1]) << 8) | (static_cast<u32>(block[i * 4 + 2]) << 16) | (static_cast<u32>(block[i * 4 + 3]) << 24);
        }
        size_t length = min<size_t>(32, output.size() - done);
        __builtin_memcpy(output.data() + done, block + 32, length);
        __builtin_memset(block, 0, sizeof(block));
        done += length;
    }
}

}
