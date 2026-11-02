/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibCrypto/AES.h>
#include <LibCrypto/ProcessorFeatures.h>

namespace Crypto {

namespace {

// The S-box, computed rather than written out: the inverse in GF(2^8), then an affine map.
struct SBox {
    alignas(64) u8 bytes[256];

    constexpr SBox()
        : bytes {}
    {
        u8 p = 1;
        u8 q = 1;
        do {
            // Multiply p by 3.
            p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            // Divide q by 3 (multiply by 0xf6).
            q = static_cast<u8>(q ^ (q << 1));
            q = static_cast<u8>(q ^ (q << 2));
            q = static_cast<u8>(q ^ (q << 4));
            if (q & 0x80)
                q ^= 0x09;
            auto rotated = [](u8 value, int by) { return static_cast<u8>((value << by) | (value >> (8 - by))); };
            bytes[p] = static_cast<u8>(q ^ rotated(q, 1) ^ rotated(q, 2) ^ rotated(q, 3) ^ rotated(q, 4) ^ 0x63);
        } while (p != 1);
        bytes[0] = 0x63;
    }
};

constexpr SBox s_box;

// The table's words, read every time; the wanted byte is kept by a mask.
inline u8 substitute(u8 input)
{
    u64 result = 0;
    auto const* words = reinterpret_cast<u64 const*>(s_box.bytes);
    for (unsigned i = 0; i < 32; ++i) {
        u64 word = words[i];
        // All ones if this is the word, nothing otherwise: without a branch.
        u64 difference = static_cast<u64>(input >> 3) ^ i;
        u64 mask = (difference - 1) >> 63;
        result |= word & (0 - mask);
    }
    return static_cast<u8>(result >> ((input & 7) * 8));
}

inline u8 double_in_field(u8 value)
{
    return static_cast<u8>((value << 1) ^ (0 - (value >> 7) & 0x1b));
}

void encrypt_in_software(u8 const* round_keys, int rounds, u8 const* input, u8* output)
{
    u8 state[16];
    for (int i = 0; i < 16; ++i)
        state[i] = input[i] ^ round_keys[i];
    for (int round = 1; round <= rounds; ++round) {
        u8 shifted[16];
        // SubBytes and ShiftRows at once.
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row)
                shifted[column * 4 + row] = substitute(state[((column + row) % 4) * 4 + row]);
        }
        if (round == rounds) {
            for (int i = 0; i < 16; ++i)
                state[i] = shifted[i];
        } else {
            for (int column = 0; column < 4; ++column) {
                u8 const* in = shifted + column * 4;
                u8* out = state + column * 4;
                u8 all = in[0] ^ in[1] ^ in[2] ^ in[3];
                out[0] = in[0] ^ all ^ double_in_field(in[0] ^ in[1]);
                out[1] = in[1] ^ all ^ double_in_field(in[1] ^ in[2]);
                out[2] = in[2] ^ all ^ double_in_field(in[2] ^ in[3]);
                out[3] = in[3] ^ all ^ double_in_field(in[3] ^ in[0]);
            }
        }
        for (int i = 0; i < 16; ++i)
            state[i] ^= round_keys[round * 16 + i];
    }
    for (int i = 0; i < 16; ++i)
        output[i] = state[i];
}

#if VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS

using Register128 = unsigned char __attribute__((vector_size(16)));

[[gnu::target("aes")]] void encrypt_in_hardware(u8 const* round_keys, int rounds, u8 const* input, u8* output)
{
    Register128 state;
    __builtin_memcpy(&state, input, 16);
    for (int round = 0; round < rounds - 1; ++round) {
        Register128 key;
        __builtin_memcpy(&key, round_keys + round * 16, 16);
        asm("aese %0.16b, %1.16b\n"
            "aesmc %0.16b, %0.16b"
            : "+w"(state)
            : "w"(key));
    }
    Register128 last;
    __builtin_memcpy(&last, round_keys + (rounds - 1) * 16, 16);
    asm("aese %0.16b, %1.16b" : "+w"(state) : "w"(last));
    Register128 final_key;
    __builtin_memcpy(&final_key, round_keys + rounds * 16, 16);
    state ^= final_key;
    __builtin_memcpy(output, &state, 16);
}

#endif

bool s_hardware_allowed = true;

}

bool AES::has_hardware()
{
    if (!s_hardware_allowed)
        return false;
#if VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS
    static bool const found = getauxval(auxiliary_hardware_capabilities) & capability_aes;
    return found;
#else
    return false;
#endif
}

void AES::set_hardware_allowed(bool allowed)
{
    s_hardware_allowed = allowed;
}

AES::AES(ReadonlyBytes key)
{
    VERIFY(key.size() == 16 || key.size() == 32);
    int key_words = static_cast<int>(key.size() / 4);
    m_rounds = key_words + 6;
    int total_words = (m_rounds + 1) * 4;
    for (size_t i = 0; i < key.size(); ++i)
        m_round_keys[i] = key[i];
    u8 round_constant = 1;
    for (int word = key_words; word < total_words; ++word) {
        u8 temporary[4];
        for (int i = 0; i < 4; ++i)
            temporary[i] = m_round_keys[(word - 1) * 4 + i];
        if (word % key_words == 0) {
            u8 first = temporary[0];
            temporary[0] = static_cast<u8>(substitute(temporary[1]) ^ round_constant);
            temporary[1] = substitute(temporary[2]);
            temporary[2] = substitute(temporary[3]);
            temporary[3] = substitute(first);
            round_constant = double_in_field(round_constant);
        } else if (key_words > 6 && word % key_words == 4) {
            for (int i = 0; i < 4; ++i)
                temporary[i] = substitute(temporary[i]);
        }
        for (int i = 0; i < 4; ++i)
            m_round_keys[word * 4 + i] = m_round_keys[(word - key_words) * 4 + i] ^ temporary[i];
    }
}

AES::~AES()
{
    __builtin_memset(m_round_keys, 0, sizeof(m_round_keys));
}

void AES::encrypt_block(u8 const* input, u8* output) const
{
#if VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS
    if (has_hardware()) {
        encrypt_in_hardware(m_round_keys, m_rounds, input, output);
        return;
    }
#endif
    encrypt_in_software(m_round_keys, m_rounds, input, output);
}

}
