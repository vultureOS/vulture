/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibCrypto/EllipticCurve.h>

namespace Crypto {

namespace {

using u128 = unsigned __int128;

constexpr size_t largest_limbs = 6;

// Little end first. As many limbs count as the curve has; the rest stay zero.
struct Number {
    u64 limbs[largest_limbs] {};
};

// A prime and what Montgomery multiplication modulo it needs.
struct Modulus {
    size_t limbs;
    Number value;
    u64 n0;        // -1/value modulo 2^64.
    Number one;    // 2^(64 limbs) modulo value: one, in Montgomery form.
    Number square; // 2^(128 limbs) modulo value: what brings a number into it.

    void add(Number const& a, Number const& b, Number& r) const
    {
        u64 carry = 0;
        Number sum;
        for (size_t i = 0; i < limbs; ++i) {
            u128 t = static_cast<u128>(a.limbs[i]) + b.limbs[i] + carry;
            sum.limbs[i] = static_cast<u64>(t);
            carry = static_cast<u64>(t >> 64);
        }
        reduce_once(sum, carry, r);
    }

    void subtract(Number const& a, Number const& b, Number& r) const
    {
        u64 borrow = 0;
        Number difference;
        for (size_t i = 0; i < limbs; ++i) {
            u128 t = static_cast<u128>(a.limbs[i]) - b.limbs[i] - borrow;
            difference.limbs[i] = static_cast<u64>(t);
            borrow = static_cast<u64>(t >> 64) & 1;
        }
        // Went below zero: add the modulus back.
        u64 mask = 0 - borrow;
        u64 carry = 0;
        for (size_t i = 0; i < limbs; ++i) {
            u128 t = static_cast<u128>(difference.limbs[i]) + (value.limbs[i] & mask) + carry;
            r.limbs[i] = static_cast<u64>(t);
            carry = static_cast<u64>(t >> 64);
        }
    }

    // a * b / 2^(64 limbs), by word-by-word Montgomery.
    void multiply(Number const& a, Number const& b, Number& r) const
    {
        u64 t[largest_limbs + 2] = {};
        for (size_t i = 0; i < limbs; ++i) {
            u64 carry = 0;
            for (size_t j = 0; j < limbs; ++j) {
                u128 product = static_cast<u128>(a.limbs[j]) * b.limbs[i] + t[j] + carry;
                t[j] = static_cast<u64>(product);
                carry = static_cast<u64>(product >> 64);
            }
            u128 sum = static_cast<u128>(t[limbs]) + carry;
            t[limbs] = static_cast<u64>(sum);
            t[limbs + 1] = static_cast<u64>(sum >> 64);

            u64 q = t[0] * n0;
            u128 product = static_cast<u128>(q) * value.limbs[0] + t[0];
            carry = static_cast<u64>(product >> 64);
            for (size_t j = 1; j < limbs; ++j) {
                product = static_cast<u128>(q) * value.limbs[j] + t[j] + carry;
                t[j - 1] = static_cast<u64>(product);
                carry = static_cast<u64>(product >> 64);
            }
            sum = static_cast<u128>(t[limbs]) + carry;
            t[limbs - 1] = static_cast<u64>(sum);
            t[limbs] = t[limbs + 1] + static_cast<u64>(sum >> 64);
        }
        Number result;
        for (size_t i = 0; i < limbs; ++i)
            result.limbs[i] = t[i];
        reduce_once(result, t[limbs], r);
    }

    void reduce_once(Number const& a, u64 top, Number& r) const
    {
        u64 borrow = 0;
        Number difference;
        for (size_t i = 0; i < limbs; ++i) {
            u128 t = static_cast<u128>(a.limbs[i]) - value.limbs[i] - borrow;
            difference.limbs[i] = static_cast<u64>(t);
            borrow = static_cast<u64>(t >> 64) & 1;
        }
        // Keep the difference if it didn't go below zero, counting the top word.
        u64 keep_a = (0 - borrow) & ~(0 - static_cast<u64>(top != 0));
        for (size_t i = 0; i < limbs; ++i)
            r.limbs[i] = (a.limbs[i] & keep_a) | (difference.limbs[i] & ~keep_a);
    }

    // a^(value - 2): the inverse, as value is prime. The exponent is public.
    void invert(Number const& a, Number& r) const
    {
        Number exponent = value;
        exponent.limbs[0] -= 2; // Every modulus here is odd and well above two.
        Number result = one;
        for (size_t bit = limbs * 64; bit-- > 0;) {
            multiply(result, result, result);
            Number with_a;
            multiply(result, a, with_a);
            u64 take = (exponent.limbs[bit / 64] >> (bit % 64)) & 1;
            u64 mask = 0 - take;
            for (size_t i = 0; i < limbs; ++i)
                result.limbs[i] = (with_a.limbs[i] & mask) | (result.limbs[i] & ~mask);
        }
        r = result;
    }
};

constexpr u8 hex_value(char c)
{
    return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
}

constexpr Number number_from_hex(char const* hex, size_t limbs)
{
    Number result;
    for (size_t i = 0; i < limbs * 16; ++i) {
        size_t from_end = limbs * 16 - 1 - i;
        result.limbs[from_end / 16] |= static_cast<u64>(hex_value(hex[i])) << ((from_end % 16) * 4);
    }
    return result;
}

Modulus make_modulus(Number const& value, size_t limbs)
{
    Modulus m;
    m.limbs = limbs;
    m.value = value;
    u64 inverse = 1;
    for (int i = 0; i < 6; ++i)
        inverse *= 2 - value.limbs[0] * inverse;
    m.n0 = 0 - inverse;

    // 1, doubled 64 limbs times, and doubled as many again.
    Number power;
    power.limbs[0] = 1;
    m.square = Number {};
    for (size_t i = 0; i < limbs * 128; ++i) {
        if (i == limbs * 64)
            m.one = power;
        m.add(power, power, power);
    }
    m.square = power;
    return m;
}

struct Curve {
    size_t limbs;
    Modulus field;
    Modulus order;
    Number b; // In Montgomery form.
    Number base_x;
    Number base_y;
    Number curve_order;
};

Curve make_curve(NamedCurve which)
{
    Curve curve;
    Number b;
    if (which == NamedCurve::P256) {
        curve.limbs = 4;
        curve.field = make_modulus(number_from_hex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", 4), 4);
        curve.order = make_modulus(number_from_hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", 4), 4);
        b = number_from_hex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b", 4);
        curve.base_x = number_from_hex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296", 4);
        curve.base_y = number_from_hex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5", 4);
    } else {
        curve.limbs = 6;
        curve.field = make_modulus(number_from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff", 6), 6);
        curve.order = make_modulus(number_from_hex("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973", 6), 6);
        b = number_from_hex("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef", 6);
        curve.base_x = number_from_hex("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7", 6);
        curve.base_y = number_from_hex("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f", 6);
    }
    curve.curve_order = curve.order.value;
    curve.field.multiply(b, curve.field.square, curve.b);
    curve.field.multiply(curve.base_x, curve.field.square, curve.base_x);
    curve.field.multiply(curve.base_y, curve.field.square, curve.base_y);
    return curve;
}

// Projective coordinates, in Montgomery form. (0 : 1 : 0) is the point at infinity.
struct Point {
    Number x, y, z;
};

// Algorithm 4 of Renes, Costello and Batina: complete for a = -3, so that it serves for
// doubling and for the point at infinity too.
void point_add(Curve const& curve, Point const& p, Point const& q, Point& r)
{
    Modulus const& f = curve.field;
    Number t0, t1, t2, t3, t4, x3, y3, z3;
    f.multiply(p.x, q.x, t0);
    f.multiply(p.y, q.y, t1);
    f.multiply(p.z, q.z, t2);
    f.add(p.x, p.y, t3);
    f.add(q.x, q.y, t4);
    f.multiply(t3, t4, t3);
    f.add(t0, t1, t4);
    f.subtract(t3, t4, t3);
    f.add(p.y, p.z, t4);
    f.add(q.y, q.z, x3);
    f.multiply(t4, x3, t4);
    f.add(t1, t2, x3);
    f.subtract(t4, x3, t4);
    f.add(p.x, p.z, x3);
    f.add(q.x, q.z, y3);
    f.multiply(x3, y3, x3);
    f.add(t0, t2, y3);
    f.subtract(x3, y3, y3);
    f.multiply(curve.b, t2, z3);
    f.subtract(y3, z3, x3);
    f.add(x3, x3, z3);
    f.add(x3, z3, x3);
    f.subtract(t1, x3, z3);
    f.add(t1, x3, x3);
    f.multiply(curve.b, y3, y3);
    f.add(t2, t2, t1);
    f.add(t1, t2, t2);
    f.subtract(y3, t2, y3);
    f.subtract(y3, t0, y3);
    f.add(y3, y3, t1);
    f.add(t1, y3, y3);
    f.add(t0, t0, t1);
    f.add(t1, t0, t0);
    f.subtract(t0, t2, t0);
    f.multiply(t4, y3, t1);
    f.multiply(t0, y3, t2);
    f.multiply(x3, z3, y3);
    f.add(y3, t2, y3);
    f.multiply(t3, x3, x3);
    f.subtract(x3, t1, x3);
    f.multiply(t4, z3, z3);
    f.multiply(t3, t0, t1);
    f.add(z3, t1, z3);
    r.x = x3;
    r.y = y3;
    r.z = z3;
}

void conditional_swap(Curve const& curve, Point& a, Point& b, u64 swap)
{
    u64 mask = 0 - swap;
    for (size_t i = 0; i < curve.limbs; ++i) {
        u64 x = mask & (a.x.limbs[i] ^ b.x.limbs[i]);
        a.x.limbs[i] ^= x;
        b.x.limbs[i] ^= x;
        u64 y = mask & (a.y.limbs[i] ^ b.y.limbs[i]);
        a.y.limbs[i] ^= y;
        b.y.limbs[i] ^= y;
        u64 z = mask & (a.z.limbs[i] ^ b.z.limbs[i]);
        a.z.limbs[i] ^= z;
        b.z.limbs[i] ^= z;
    }
}

Point infinity_of(Curve const& curve)
{
    Point p;
    p.y = curve.field.one;
    return p;
}

void point_multiply(Curve const& curve, Number const& scalar, Point const& p, Point& r)
{
    Point r0 = infinity_of(curve);
    Point r1 = p;
    for (size_t bit = curve.limbs * 64; bit-- > 0;) {
        u64 b = (scalar.limbs[bit / 64] >> (bit % 64)) & 1;
        conditional_swap(curve, r0, r1, b);
        point_add(curve, r0, r1, r1);
        point_add(curve, r0, r0, r0);
        conditional_swap(curve, r0, r1, b);
    }
    r = r0;
}

bool is_zero(Number const& a, size_t limbs)
{
    u64 all = 0;
    for (size_t i = 0; i < limbs; ++i)
        all |= a.limbs[i];
    return all == 0;
}

bool is_below(Number const& a, Number const& b, size_t limbs)
{
    for (size_t i = limbs; i-- > 0;) {
        if (a.limbs[i] != b.limbs[i])
            return a.limbs[i] < b.limbs[i];
    }
    return false;
}

// Big-endian bytes, as long as they fit once leading zeroes are ignored.
bool number_from_bytes(ReadonlyBytes bytes, Number& result, size_t limbs)
{
    result = Number {};
    size_t start = 0;
    while (start < bytes.size() && bytes[start] == 0)
        ++start;
    if (bytes.size() - start > limbs * 8)
        return false;
    for (size_t i = start; i < bytes.size(); ++i) {
        size_t from_end = bytes.size() - 1 - i;
        result.limbs[from_end / 8] |= static_cast<u64>(bytes[i]) << ((from_end % 8) * 8);
    }
    return true;
}

void number_to_bytes(Number const& a, size_t limbs, u8* output)
{
    size_t const size = limbs * 8;
    for (size_t i = 0; i < size; ++i) {
        size_t from_end = size - 1 - i;
        output[i] = static_cast<u8>(a.limbs[from_end / 8] >> ((from_end % 8) * 8));
    }
}

// From projective coordinates and Montgomery form to plain x and y.
bool to_affine(Curve const& curve, Point const& p, Number& x, Number& y)
{
    if (is_zero(p.z, curve.limbs))
        return false;
    Modulus const& f = curve.field;
    Number inverse;
    f.invert(p.z, inverse);
    Number plain_one;
    plain_one.limbs[0] = 1;
    f.multiply(p.x, inverse, x);
    f.multiply(p.y, inverse, y);
    f.multiply(x, plain_one, x);
    f.multiply(y, plain_one, y);
    return true;
}

// A public key's point, checked against the curve's equation.
bool point_from_public_key(Curve const& curve, ReadonlyBytes key, Point& p)
{
    size_t const size = curve.limbs * 8;
    if (key.size() != size * 2 + 1 || key[0] != 0x04)
        return false;
    Number x, y;
    if (!number_from_bytes({ key.data() + 1, size }, x, curve.limbs) || !number_from_bytes({ key.data() + 1 + size, size }, y, curve.limbs))
        return false;
    Modulus const& f = curve.field;
    if (!is_below(x, f.value, curve.limbs) || !is_below(y, f.value, curve.limbs))
        return false;
    f.multiply(x, f.square, p.x);
    f.multiply(y, f.square, p.y);
    p.z = f.one;
    // y^2 = x^3 - 3x + b.
    Number left, right, three_x;
    f.multiply(p.y, p.y, left);
    f.multiply(p.x, p.x, right);
    f.multiply(right, p.x, right);
    f.add(p.x, p.x, three_x);
    f.add(three_x, p.x, three_x);
    f.subtract(right, three_x, right);
    f.add(right, curve.b, right);
    for (size_t i = 0; i < curve.limbs; ++i) {
        if (left.limbs[i] != right.limbs[i])
            return false;
    }
    return true;
}

}

size_t curve_coordinate_size(NamedCurve curve)
{
    return curve == NamedCurve::P256 ? 32 : 48;
}

bool ec_public_key(NamedCurve which, ReadonlyBytes scalar_bytes, Bytes public_key)
{
    auto curve = make_curve(which);
    size_t const size = curve.limbs * 8;
    if (scalar_bytes.size() != size || public_key.size() != size * 2 + 1)
        return false;
    Number scalar;
    if (!number_from_bytes(scalar_bytes, scalar, curve.limbs) || is_zero(scalar, curve.limbs) || !is_below(scalar, curve.curve_order, curve.limbs))
        return false;
    Point base { curve.base_x, curve.base_y, curve.field.one };
    Point result;
    point_multiply(curve, scalar, base, result);
    Number x, y;
    if (!to_affine(curve, result, x, y))
        return false;
    public_key[0] = 0x04;
    number_to_bytes(x, curve.limbs, public_key.data() + 1);
    number_to_bytes(y, curve.limbs, public_key.data() + 1 + size);
    return true;
}

bool ecdh(NamedCurve which, ReadonlyBytes scalar_bytes, ReadonlyBytes peer_public_key, Bytes shared_secret)
{
    auto curve = make_curve(which);
    size_t const size = curve.limbs * 8;
    if (scalar_bytes.size() != size || shared_secret.size() != size)
        return false;
    Number scalar;
    if (!number_from_bytes(scalar_bytes, scalar, curve.limbs) || is_zero(scalar, curve.limbs) || !is_below(scalar, curve.curve_order, curve.limbs))
        return false;
    Point peer;
    if (!point_from_public_key(curve, peer_public_key, peer))
        return false;
    Point result;
    point_multiply(curve, scalar, peer, result);
    Number x, y;
    if (!to_affine(curve, result, x, y))
        return false;
    number_to_bytes(x, curve.limbs, shared_secret.data());
    return true;
}

bool ecdsa_verify(NamedCurve which, ReadonlyBytes public_key, ReadonlyBytes digest, ReadonlyBytes r_bytes, ReadonlyBytes s_bytes)
{
    auto curve = make_curve(which);
    size_t const size = curve.limbs * 8;
    Modulus const& n = curve.order;
    Number r, s;
    if (!number_from_bytes(r_bytes, r, curve.limbs) || !number_from_bytes(s_bytes, s, curve.limbs))
        return false;
    if (is_zero(r, curve.limbs) || is_zero(s, curve.limbs) || !is_below(r, n.value, curve.limbs) || !is_below(s, n.value, curve.limbs))
        return false;
    Point q;
    if (!point_from_public_key(curve, public_key, q))
        return false;

    // The digest's leftmost bits, as many as the order has: for these curves, whole bytes.
    size_t const digest_size = digest.size() < size ? digest.size() : size;
    Number z;
    if (!number_from_bytes({ digest.data(), digest_size }, z, curve.limbs))
        return false;
    // Below twice the order, for these curves, so once is enough.
    n.reduce_once(z, 0, z);

    Number s_montgomery, inverse, z_montgomery, r_montgomery;
    n.multiply(s, n.square, s_montgomery);
    n.invert(s_montgomery, inverse);
    n.multiply(z, n.square, z_montgomery);
    n.multiply(r, n.square, r_montgomery);
    Number u1, u2, plain_one;
    plain_one.limbs[0] = 1;
    n.multiply(z_montgomery, inverse, u1);
    n.multiply(u1, plain_one, u1);
    n.multiply(r_montgomery, inverse, u2);
    n.multiply(u2, plain_one, u2);

    Point base { curve.base_x, curve.base_y, curve.field.one };
    Point first, second, sum;
    point_multiply(curve, u1, base, first);
    point_multiply(curve, u2, q, second);
    point_add(curve, first, second, sum);
    Number x, y;
    if (!to_affine(curve, sum, x, y))
        return false;
    // x modulo the order, again a single subtraction at most.
    n.reduce_once(x, 0, x);
    for (size_t i = 0; i < curve.limbs; ++i) {
        if (x.limbs[i] != r.limbs[i])
            return false;
    }
    return true;
}

}
