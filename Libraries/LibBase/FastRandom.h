/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/Types.h>

// A fast, deterministic pseudo-random generator (xoshiro256**, seeded with
// SplitMix64) for tests, hashing and load spreading. Not for anything that
// needs unpredictability: it is not cryptographically secure.

namespace Base {

class FastRandom {
public:
    explicit constexpr FastRandom(u64 seed)
    {
        for (auto& word : m_state)
            word = split_mix(seed);
    }

    constexpr u64 next()
    {
        u64 result = rotate_left(m_state[1] * 5, 7) * 9;
        u64 t = m_state[1] << 17;
        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];
        m_state[2] ^= t;
        m_state[3] = rotate_left(m_state[3], 45);
        return result;
    }

    // A number in [0, bound), using the high bits of a 128-bit product.
    // The bias is at most bound / 2^64, which is negligible here.
    constexpr u64 next_below(u64 bound)
    {
        VERIFY(bound != 0);
        return static_cast<u64>((static_cast<unsigned __int128>(next()) * bound) >> 64);
    }

    // A number in [low, high].
    constexpr u64 next_between(u64 low, u64 high)
    {
        VERIFY(low <= high);
        if (high - low == ~0ull)
            return next();
        return low + next_below(high - low + 1);
    }

private:
    static constexpr u64 rotate_left(u64 value, unsigned count)
    {
        return (value << count) | (value >> (64 - count));
    }

    static constexpr u64 split_mix(u64& state)
    {
        u64 z = (state += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

    u64 m_state[4] {};
};

}

using Base::FastRandom;
