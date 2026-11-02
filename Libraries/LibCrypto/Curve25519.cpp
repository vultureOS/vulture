/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/Curve25519.h>
#include <LibCrypto/SHA2.h>

namespace Crypto {

namespace {

using u128 = unsigned __int128;

// A number modulo 2^255 - 19, in five limbs of 51 bits. A limb may run a little over that
// between operations; multiplication copes with up to 54 bits.
struct FieldElement {
    u64 limbs[5];
};

constexpr u64 mask51 = (1ull << 51) - 1;

constexpr FieldElement field_zero { { 0, 0, 0, 0, 0 } };
constexpr FieldElement field_one { { 1, 0, 0, 0, 0 } };

inline FieldElement add(FieldElement const& a, FieldElement const& b)
{
    FieldElement r;
    for (int i = 0; i < 5; ++i)
        r.limbs[i] = a.limbs[i] + b.limbs[i];
    return r;
}

// a - b, kept positive by adding a multiple of the modulus.
inline FieldElement subtract(FieldElement const& a, FieldElement const& b)
{
    FieldElement r;
    r.limbs[0] = a.limbs[0] + 0xfffffffffffdaull - b.limbs[0];
    for (int i = 1; i < 5; ++i)
        r.limbs[i] = a.limbs[i] + 0xffffffffffffeull - b.limbs[i];
    u64 carry = r.limbs[0] >> 51;
    r.limbs[0] &= mask51;
    for (int i = 1; i < 5; ++i) {
        r.limbs[i] += carry;
        carry = r.limbs[i] >> 51;
        r.limbs[i] &= mask51;
    }
    r.limbs[0] += carry * 19;
    return r;
}

inline FieldElement carry_products(u128 r0, u128 r1, u128 r2, u128 r3, u128 r4)
{
    FieldElement r;
    u64 carry;
    carry = static_cast<u64>(r0 >> 51);
    r.limbs[0] = static_cast<u64>(r0) & mask51;
    r1 += carry;
    carry = static_cast<u64>(r1 >> 51);
    r.limbs[1] = static_cast<u64>(r1) & mask51;
    r2 += carry;
    carry = static_cast<u64>(r2 >> 51);
    r.limbs[2] = static_cast<u64>(r2) & mask51;
    r3 += carry;
    carry = static_cast<u64>(r3 >> 51);
    r.limbs[3] = static_cast<u64>(r3) & mask51;
    r4 += carry;
    carry = static_cast<u64>(r4 >> 51);
    r.limbs[4] = static_cast<u64>(r4) & mask51;
    r.limbs[0] += carry * 19;
    carry = r.limbs[0] >> 51;
    r.limbs[0] &= mask51;
    r.limbs[1] += carry;
    return r;
}

inline FieldElement multiply(FieldElement const& a, FieldElement const& b)
{
    u64 const* x = a.limbs;
    u64 const* y = b.limbs;
    auto m = [](u64 p, u64 q) { return static_cast<u128>(p) * q; };
    u128 r0 = m(x[0], y[0]) + m(x[1] * 19, y[4]) + m(x[2] * 19, y[3]) + m(x[3] * 19, y[2]) + m(x[4] * 19, y[1]);
    u128 r1 = m(x[0], y[1]) + m(x[1], y[0]) + m(x[2] * 19, y[4]) + m(x[3] * 19, y[3]) + m(x[4] * 19, y[2]);
    u128 r2 = m(x[0], y[2]) + m(x[1], y[1]) + m(x[2], y[0]) + m(x[3] * 19, y[4]) + m(x[4] * 19, y[3]);
    u128 r3 = m(x[0], y[3]) + m(x[1], y[2]) + m(x[2], y[1]) + m(x[3], y[0]) + m(x[4] * 19, y[4]);
    u128 r4 = m(x[0], y[4]) + m(x[1], y[3]) + m(x[2], y[2]) + m(x[3], y[1]) + m(x[4], y[0]);
    return carry_products(r0, r1, r2, r3, r4);
}

inline FieldElement square(FieldElement const& a)
{
    return multiply(a, a);
}

inline FieldElement multiply_small(FieldElement const& a, u64 small)
{
    u128 r0 = static_cast<u128>(a.limbs[0]) * small;
    u128 r1 = static_cast<u128>(a.limbs[1]) * small;
    u128 r2 = static_cast<u128>(a.limbs[2]) * small;
    u128 r3 = static_cast<u128>(a.limbs[3]) * small;
    u128 r4 = static_cast<u128>(a.limbs[4]) * small;
    return carry_products(r0, r1, r2, r3, r4);
}

inline FieldElement square_times(FieldElement a, int times)
{
    for (int i = 0; i < times; ++i)
        a = square(a);
    return a;
}

// a^(2^250 - 1), and a^11, the common stem of inverting and of taking square roots.
void power_stem(FieldElement const& a, FieldElement& power_2_250_minus_1, FieldElement& power_11)
{
    FieldElement a2 = square(a);
    FieldElement a9 = multiply(square_times(a2, 2), a);
    power_11 = multiply(a9, a2);
    FieldElement x5 = multiply(square(power_11), a9); // a^(2^5 - 1)
    FieldElement x10 = multiply(square_times(x5, 5), x5);
    FieldElement x20 = multiply(square_times(x10, 10), x10);
    FieldElement x40 = multiply(square_times(x20, 20), x20);
    FieldElement x50 = multiply(square_times(x40, 10), x10);
    FieldElement x100 = multiply(square_times(x50, 50), x50);
    FieldElement x200 = multiply(square_times(x100, 100), x100);
    power_2_250_minus_1 = multiply(square_times(x200, 50), x50);
}

FieldElement invert(FieldElement const& a)
{
    FieldElement stem, power_11;
    power_stem(a, stem, power_11);
    return multiply(square_times(stem, 5), power_11);
}

// a^((p - 5) / 8) = a^(2^252 - 3).
FieldElement power_p58(FieldElement const& a)
{
    FieldElement stem, power_11;
    power_stem(a, stem, power_11);
    return multiply(square_times(stem, 2), a);
}

inline u64 load_le64(u8 const* bytes)
{
    u64 value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | bytes[i];
    return value;
}

FieldElement field_from_bytes(u8 const* bytes)
{
    FieldElement r;
    r.limbs[0] = load_le64(bytes) & mask51;
    r.limbs[1] = (load_le64(bytes + 6) >> 3) & mask51;
    r.limbs[2] = (load_le64(bytes + 12) >> 6) & mask51;
    r.limbs[3] = (load_le64(bytes + 19) >> 1) & mask51;
    r.limbs[4] = (load_le64(bytes + 24) >> 12) & mask51;
    return r;
}

void field_to_bytes(FieldElement a, u8* output)
{
    // Twice around, then subtract the modulus if it fits.
    for (int round = 0; round < 2; ++round) {
        u64 carry = 0;
        for (int i = 0; i < 5; ++i) {
            a.limbs[i] += carry;
            carry = a.limbs[i] >> 51;
            a.limbs[i] &= mask51;
        }
        a.limbs[0] += carry * 19;
    }
    u64 q = (a.limbs[0] + 19) >> 51;
    for (int i = 1; i < 5; ++i)
        q = (a.limbs[i] + q) >> 51;
    a.limbs[0] += 19 * q;
    u64 carry = 0;
    for (int i = 0; i < 5; ++i) {
        a.limbs[i] += carry;
        carry = a.limbs[i] >> 51;
        a.limbs[i] &= mask51;
    }
    u64 words[4] = {
        a.limbs[0] | (a.limbs[1] << 51),
        (a.limbs[1] >> 13) | (a.limbs[2] << 38),
        (a.limbs[2] >> 26) | (a.limbs[3] << 25),
        (a.limbs[3] >> 39) | (a.limbs[4] << 12),
    };
    for (int word = 0; word < 4; ++word) {
        for (int i = 0; i < 8; ++i)
            output[word * 8 + i] = static_cast<u8>(words[word] >> (i * 8));
    }
}

inline void conditional_swap(FieldElement& a, FieldElement& b, u64 swap)
{
    u64 mask = 0 - swap;
    for (int i = 0; i < 5; ++i) {
        u64 x = mask & (a.limbs[i] ^ b.limbs[i]);
        a.limbs[i] ^= x;
        b.limbs[i] ^= x;
    }
}

bool field_is_zero(FieldElement const& a)
{
    u8 bytes[32];
    field_to_bytes(a, bytes);
    u8 all = 0;
    for (u8 byte : bytes)
        all |= byte;
    return all == 0;
}

bool field_is_negative(FieldElement const& a)
{
    u8 bytes[32];
    field_to_bytes(a, bytes);
    return bytes[0] & 1;
}

// ---- Edwards curve points, in extended coordinates (X:Y:Z:T), with x = X/Z, y = Y/Z and
// xy = T/Z. The addition law below is complete: it has no exceptions.

struct EdwardsPoint {
    FieldElement x, y, z, t;
};

// d = -121665/121666, and sqrt(-1), each as limbs (values of RFC 8032).
constexpr FieldElement curve_d { { 0x34dca135978a3ull, 0x1a8283b156ebdull, 0x5e7a26001c029ull, 0x739c663a03cbbull, 0x52036cee2b6ffull } };
constexpr FieldElement curve_d2 { { 0x69b9426b2f159ull, 0x35050762add7aull, 0x3cf44c0038052ull, 0x6738cc7407977ull, 0x2406d9dc56dffull } };
constexpr FieldElement root_of_minus_one { { 0x61b274a0ea0b0ull, 0x0d5a5fc8f189dull, 0x7ef5e9cbd0c60ull, 0x78595a6804c9eull, 0x2b8324804fc1dull } };

constexpr EdwardsPoint base_point {
    { { 0x62d608f25d51aull, 0x412a4b4f6592aull, 0x75b7171a4b31dull, 0x1ff60527118feull, 0x216936d3cd6e5ull } },
    { { 0x6666666666658ull, 0x4ccccccccccccull, 0x1999999999999ull, 0x3333333333333ull, 0x6666666666666ull } },
    { { 1, 0, 0, 0, 0 } },
    { { 0x68ab3a5b7dda3ull, 0x00eea2a5eadbbull, 0x2af8df483c27eull, 0x332b375274732ull, 0x67875f0fd78b7ull } },
};

constexpr EdwardsPoint identity_point { field_zero, field_one, field_one, field_zero };

EdwardsPoint point_add(EdwardsPoint const& p, EdwardsPoint const& q)
{
    FieldElement a = multiply(subtract(p.y, p.x), subtract(q.y, q.x));
    FieldElement b = multiply(add(p.y, p.x), add(q.y, q.x));
    FieldElement c = multiply(multiply(p.t, q.t), curve_d2);
    FieldElement d = multiply_small(multiply(p.z, q.z), 2);
    FieldElement e = subtract(b, a);
    FieldElement f = subtract(d, c);
    FieldElement g = add(d, c);
    FieldElement h = add(b, a);
    return { multiply(e, f), multiply(g, h), multiply(f, g), multiply(e, h) };
}

EdwardsPoint point_double(EdwardsPoint const& p)
{
    return point_add(p, p);
}

void point_conditional_swap(EdwardsPoint& a, EdwardsPoint& b, u64 swap)
{
    conditional_swap(a.x, b.x, swap);
    conditional_swap(a.y, b.y, swap);
    conditional_swap(a.z, b.z, swap);
    conditional_swap(a.t, b.t, swap);
}

// `scalar` is 32 bytes, little-endian; every bit takes the same steps.
EdwardsPoint point_multiply(u8 const* scalar, EdwardsPoint const& p)
{
    EdwardsPoint r0 = identity_point;
    EdwardsPoint r1 = p;
    for (int bit = 255; bit >= 0; --bit) {
        u64 b = (scalar[bit / 8] >> (bit % 8)) & 1;
        point_conditional_swap(r0, r1, b);
        r1 = point_add(r0, r1);
        r0 = point_double(r0);
        point_conditional_swap(r0, r1, b);
    }
    return r0;
}

void point_to_bytes(EdwardsPoint const& p, u8* output)
{
    FieldElement inverse = invert(p.z);
    FieldElement x = multiply(p.x, inverse);
    FieldElement y = multiply(p.y, inverse);
    field_to_bytes(y, output);
    output[31] = static_cast<u8>((output[31] & 0x7f) | (field_is_negative(x) ? 0x80 : 0));
}

// False if these bytes are no point on the curve.
bool point_from_bytes(u8 const* bytes, EdwardsPoint& p)
{
    // Refuse a y that is not reduced.
    static constexpr u8 modulus[32] = { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
    bool below = false;
    for (int i = 31; i >= 0; --i) {
        u8 byte = i == 31 ? (bytes[31] & 0x7f) : bytes[i];
        if (byte != modulus[i]) {
            below = byte < modulus[i];
            break;
        }
    }
    if (!below)
        return false;

    FieldElement y = field_from_bytes(bytes);
    bool sign = bytes[31] >> 7;
    // x^2 = (y^2 - 1) / (d y^2 + 1).
    FieldElement y2 = square(y);
    FieldElement u = subtract(y2, field_one);
    FieldElement v = add(multiply(y2, curve_d), field_one);
    FieldElement v3 = multiply(square(v), v);
    FieldElement v7 = multiply(square(v3), v);
    FieldElement x = multiply(multiply(u, v3), power_p58(multiply(u, v7)));
    FieldElement check = multiply(v, square(x));
    if (!field_is_zero(subtract(check, u))) {
        if (!field_is_zero(add(check, u)))
            return false;
        x = multiply(x, root_of_minus_one);
    }
    if (field_is_zero(x) && sign)
        return false;
    if (field_is_negative(x) != sign)
        x = subtract(field_zero, x);
    p.x = x;
    p.y = y;
    p.z = field_one;
    p.t = multiply(x, y);
    return true;
}

// ---- Numbers modulo the group's order, L = 2^252 + 27742317777372353535851937790883648493.

constexpr u64 group_order[4] = { 0x5812631a5cf5d3edull, 0x14def9dea2f79cd6ull, 0, 0x1000000000000000ull };

// A little-endian number of `length` bytes (at most 64), reduced modulo L, as 32 bytes.
void reduce_modulo_order(u8 const* bytes, size_t length, u8* output)
{
    // Schoolbook: bit by bit, from the top: shift in, subtract if it fits. Not fast, but
    // only ever a few hundred steps of simple words, and its branches are on values derived
    // from a hash; so it uses masks, to treat secret nonces right too.
    u64 r[5] = {};
    for (int bit = static_cast<int>(length * 8) - 1; bit >= 0; --bit) {
        u64 in = (bytes[bit / 8] >> (bit % 8)) & 1;
        for (int i = 4; i > 0; --i)
            r[i] = (r[i] << 1) | (r[i - 1] >> 63);
        r[0] = (r[0] << 1) | in;
        u64 t[5];
        u64 borrow = 0;
        for (int i = 0; i < 5; ++i) {
            u64 order = i < 4 ? group_order[i] : 0;
            u128 difference = static_cast<u128>(r[i]) - order - borrow;
            t[i] = static_cast<u64>(difference);
            borrow = static_cast<u64>(difference >> 64) & 1;
        }
        u64 keep_t = borrow - 1;
        for (int i = 0; i < 5; ++i)
            r[i] = (t[i] & keep_t) | (r[i] & ~keep_t);
    }
    for (int word = 0; word < 4; ++word) {
        for (int i = 0; i < 8; ++i)
            output[word * 8 + i] = static_cast<u8>(r[word] >> (i * 8));
    }
}

// a * b + c modulo L, all 32 little-endian bytes.
void multiply_add_modulo_order(u8 const* a, u8 const* b, u8 const* c, u8* output)
{
    u8 wide[64] = {};
    u32 accumulator[64] = {};
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j)
            accumulator[i + j] += static_cast<u32>(a[i]) * b[j];
        accumulator[i] += c[i];
    }
    u32 carry = 0;
    for (int i = 0; i < 64; ++i) {
        u32 value = accumulator[i] + carry;
        wide[i] = static_cast<u8>(value);
        carry = value >> 8;
    }
    reduce_modulo_order(wide, 64, output);
}

bool is_below_order(u8 const* s)
{
    for (int word = 3; word >= 0; --word) {
        u64 value = load_le64(s + word * 8);
        if (value != group_order[word])
            return value < group_order[word];
    }
    return false;
}

void expand_seed(u8 const* seed, u8* scalar, u8* prefix)
{
    auto hashed = SHA512::hash({ seed, ed25519_seed_size });
    __builtin_memcpy(scalar, hashed.data(), 32);
    __builtin_memcpy(prefix, hashed.data() + 32, 32);
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
}

}

void x25519(u8 const* scalar, u8 const* point, u8* output)
{
    u8 clamped[32];
    __builtin_memcpy(clamped, scalar, 32);
    clamped[0] &= 248;
    clamped[31] &= 127;
    clamped[31] |= 64;

    // RFC 7748's ladder. The top bit of the u-coordinate is ignored, and the input is
    // taken modulo the prime even if it was not reduced.
    FieldElement x1 = field_from_bytes(point);
    FieldElement x2 = field_one;
    FieldElement z2 = field_zero;
    FieldElement x3 = x1;
    FieldElement z3 = field_one;
    u64 swap = 0;
    for (int bit = 254; bit >= 0; --bit) {
        u64 b = (clamped[bit / 8] >> (bit % 8)) & 1;
        swap ^= b;
        conditional_swap(x2, x3, swap);
        conditional_swap(z2, z3, swap);
        swap = b;
        FieldElement a = add(x2, z2);
        FieldElement aa = square(a);
        FieldElement c = subtract(x2, z2);
        FieldElement bb = square(c);
        FieldElement e = subtract(aa, bb);
        FieldElement d = add(x3, z3);
        FieldElement f = subtract(x3, z3);
        FieldElement da = multiply(f, a);
        FieldElement cb = multiply(d, c);
        x3 = square(add(da, cb));
        z3 = multiply(x1, square(subtract(da, cb)));
        x2 = multiply(aa, bb);
        z2 = multiply(e, add(aa, multiply_small(e, 121665)));
    }
    conditional_swap(x2, x3, swap);
    conditional_swap(z2, z3, swap);
    field_to_bytes(multiply(x2, invert(z2)), output);
    __builtin_memset(clamped, 0, sizeof(clamped));
}

void x25519_base(u8 const* scalar, u8* output)
{
    u8 base[32] = { 9 };
    x25519(scalar, base, output);
}

void ed25519_public_key(u8 const* seed, u8* public_key)
{
    u8 scalar[32];
    u8 prefix[32];
    expand_seed(seed, scalar, prefix);
    point_to_bytes(point_multiply(scalar, base_point), public_key);
    __builtin_memset(scalar, 0, sizeof(scalar));
    __builtin_memset(prefix, 0, sizeof(prefix));
}

void ed25519_sign(u8 const* seed, ReadonlyBytes message, u8* signature)
{
    u8 scalar[32];
    u8 prefix[32];
    expand_seed(seed, scalar, prefix);
    u8 public_key[32];
    point_to_bytes(point_multiply(scalar, base_point), public_key);

    SHA512 hasher;
    hasher.update({ prefix, 32 });
    hasher.update(message);
    auto r_hash = hasher.digest();
    u8 r[32];
    reduce_modulo_order(r_hash.data(), 64, r);
    point_to_bytes(point_multiply(r, base_point), signature);

    hasher.reset();
    hasher.update({ signature, 32 });
    hasher.update({ public_key, 32 });
    hasher.update(message);
    auto k_hash = hasher.digest();
    u8 k[32];
    reduce_modulo_order(k_hash.data(), 64, k);
    multiply_add_modulo_order(k, scalar, r, signature + 32);

    __builtin_memset(scalar, 0, sizeof(scalar));
    __builtin_memset(prefix, 0, sizeof(prefix));
    __builtin_memset(r, 0, sizeof(r));
}

bool ed25519_verify(u8 const* public_key, ReadonlyBytes message, u8 const* signature)
{
    if (!is_below_order(signature + 32))
        return false;
    EdwardsPoint a;
    if (!point_from_bytes(public_key, a))
        return false;
    SHA512 hasher;
    hasher.update({ signature, 32 });
    hasher.update({ public_key, 32 });
    hasher.update(message);
    auto k_hash = hasher.digest();
    u8 k[32];
    reduce_modulo_order(k_hash.data(), 64, k);

    // S B = R + k A, so R should come out of S B - k A.
    EdwardsPoint minus_a = a;
    minus_a.x = subtract(field_zero, a.x);
    minus_a.t = subtract(field_zero, a.t);
    EdwardsPoint result = point_add(point_multiply(signature + 32, base_point), point_multiply(k, minus_a));
    u8 expected[32];
    point_to_bytes(result, expected);
    u8 difference = 0;
    for (int i = 0; i < 32; ++i)
        difference |= expected[i] ^ signature[i];
    return difference == 0;
}

}
