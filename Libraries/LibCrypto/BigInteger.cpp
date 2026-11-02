/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibCrypto/BigInteger.h>

namespace Crypto {

void BigInteger::trim()
{
    while (!m_words.is_empty() && m_words.last() == 0)
        (void)m_words.take_last();
}

BigInteger BigInteger::from_bytes(ReadonlyBytes bytes)
{
    BigInteger number;
    size_t word_count = (bytes.size() + 3) / 4;
    MUST(number.m_words.try_resize(word_count));
    for (size_t i = 0; i < word_count; ++i)
        number.m_words[i] = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        size_t from_end = bytes.size() - 1 - i;
        number.m_words[from_end / 4] |= static_cast<u32>(bytes[i]) << ((from_end % 4) * 8);
    }
    number.trim();
    return number;
}

bool BigInteger::to_bytes(Bytes output) const
{
    if (bit_length() > output.size() * 8)
        return false;
    for (size_t i = 0; i < output.size(); ++i) {
        size_t from_end = output.size() - 1 - i;
        size_t word = from_end / 4;
        output[i] = word < m_words.size() ? static_cast<u8>(m_words[word] >> ((from_end % 4) * 8)) : 0;
    }
    return true;
}

size_t BigInteger::bit_length() const
{
    if (m_words.is_empty())
        return 0;
    return (m_words.size() - 1) * 32 + (32 - static_cast<size_t>(__builtin_clz(m_words.last())));
}

int BigInteger::compare(BigInteger const& other) const
{
    if (m_words.size() != other.m_words.size())
        return m_words.size() < other.m_words.size() ? -1 : 1;
    for (size_t i = m_words.size(); i-- > 0;) {
        if (m_words[i] != other.m_words[i])
            return m_words[i] < other.m_words[i] ? -1 : 1;
    }
    return 0;
}

BigInteger BigInteger::reduced(BigInteger const& value) const
{
    // A bit at a time from the top: shift in, subtract if it fits.
    BigInteger remainder;
    MUST(remainder.m_words.try_resize(m_words.size() + 1));
    for (auto& word : remainder.m_words)
        word = 0;
    size_t bits = value.bit_length();
    for (size_t bit = bits; bit-- > 0;) {
        u32 carry = (value.m_words[bit / 32] >> (bit % 32)) & 1;
        for (auto& word : remainder.m_words) {
            u32 next = word >> 31;
            word = (word << 1) | carry;
            carry = next;
        }
        bool fits = remainder.m_words.last() != 0;
        if (!fits) {
            fits = true;
            for (size_t i = m_words.size(); i-- > 0;) {
                if (remainder.m_words[i] != m_words[i]) {
                    fits = remainder.m_words[i] > m_words[i];
                    break;
                }
            }
        }
        if (fits) {
            u64 borrow = 0;
            for (size_t i = 0; i < remainder.m_words.size(); ++i) {
                u64 subtracted = (i < m_words.size() ? m_words[i] : 0) + borrow;
                u64 word = remainder.m_words[i];
                borrow = word < subtracted ? 1 : 0;
                remainder.m_words[i] = static_cast<u32>(word - subtracted);
            }
        }
    }
    remainder.trim();
    return remainder;
}

BigInteger BigInteger::modular_power_of(BigInteger const& base, BigInteger const& exponent) const
{
    VERIFY(is_odd());
    size_t const n = m_words.size();

    // -1/m modulo 2^32, by Newton's method.
    u32 inverse = 1;
    for (int i = 0; i < 5; ++i)
        inverse *= 2 - m_words[0] * inverse;
    u32 const m_prime = 0 - inverse;

    // a * b / 2^(32 n), modulo this. Words are n long.
    auto montgomery_multiply = [&](u32 const* a, u32 const* b, u32* output) {
        Vector<u32> t;
        MUST(t.try_resize(n + 2));
        for (auto& word : t)
            word = 0;
        for (size_t i = 0; i < n; ++i) {
            u64 carry = 0;
            for (size_t j = 0; j < n; ++j) {
                u64 sum = static_cast<u64>(a[j]) * b[i] + t[j] + carry;
                t[j] = static_cast<u32>(sum);
                carry = sum >> 32;
            }
            u64 sum = static_cast<u64>(t[n]) + carry;
            t[n] = static_cast<u32>(sum);
            t[n + 1] = static_cast<u32>(sum >> 32);

            u32 q = t[0] * m_prime;
            sum = static_cast<u64>(q) * m_words[0] + t[0];
            carry = sum >> 32;
            for (size_t j = 1; j < n; ++j) {
                sum = static_cast<u64>(q) * m_words[j] + t[j] + carry;
                t[j - 1] = static_cast<u32>(sum);
                carry = sum >> 32;
            }
            sum = static_cast<u64>(t[n]) + carry;
            t[n - 1] = static_cast<u32>(sum);
            t[n] = t[n + 1] + static_cast<u32>(sum >> 32);
        }
        // At most one subtraction brings it in range.
        bool subtract = t[n] != 0;
        if (!subtract) {
            subtract = true;
            for (size_t i = n; i-- > 0;) {
                if (t[i] != m_words[i]) {
                    subtract = t[i] > m_words[i];
                    break;
                }
            }
        }
        if (subtract) {
            u64 borrow = 0;
            for (size_t i = 0; i < n; ++i) {
                u64 subtracted = static_cast<u64>(m_words[i]) + borrow;
                borrow = t[i] < subtracted ? 1 : 0;
                output[i] = static_cast<u32>(t[i] - subtracted);
            }
        } else {
            for (size_t i = 0; i < n; ++i)
                output[i] = t[i];
        }
    };

    auto to_montgomery = [&](BigInteger const& value, Vector<u32>& output) {
        // value * 2^(32 n) mod m, through shifting in zero words.
        BigInteger shifted;
        MUST(shifted.m_words.try_resize(value.m_words.size() + n));
        for (auto& word : shifted.m_words)
            word = 0;
        for (size_t i = 0; i < value.m_words.size(); ++i)
            shifted.m_words[i + n] = value.m_words[i];
        shifted.trim();
        auto reduced_value = reduced(shifted);
        MUST(output.try_resize(n));
        for (size_t i = 0; i < n; ++i)
            output[i] = i < reduced_value.m_words.size() ? reduced_value.m_words[i] : 0;
    };

    BigInteger one;
    one.m_words.append(1);
    Vector<u32> result, power;
    to_montgomery(one, result);
    to_montgomery(base, power);
    for (size_t bit = exponent.bit_length(); bit-- > 0;) {
        montgomery_multiply(result.data(), result.data(), result.data());
        if ((exponent.m_words[bit / 32] >> (bit % 32)) & 1)
            montgomery_multiply(result.data(), power.data(), result.data());
    }
    Vector<u32> unit;
    MUST(unit.try_resize(n));
    for (auto& word : unit)
        word = 0;
    unit[0] = 1;
    montgomery_multiply(result.data(), unit.data(), result.data());

    BigInteger answer;
    answer.m_words = move(result);
    answer.trim();
    return answer;
}

}
