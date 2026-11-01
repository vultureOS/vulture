/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/HMAC.h>
#include <LibCrypto/PBKDF2.h>
#include <LibCrypto/SHA2.h>
#include <LibTest/TestCase.h>

// The test vectors of FIPS 180-4 (SHA-2), RFC 4231 (HMAC-SHA-2) and
// RFC 7914 (PBKDF2-HMAC-SHA256).

namespace {

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

}

// The bytes as lower-case hexadecimal, as the vectors are written.
static bool hex_matches(ReadonlyBytes bytes, char const* expected)
{
    static constexpr char digits[] = "0123456789abcdef";
    if (__builtin_strlen(expected) != bytes.size() * 2)
        return false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (expected[i * 2] != digits[bytes[i] >> 4] || expected[i * 2 + 1] != digits[bytes[i] & 15])
            return false;
    }
    return true;
}

template<typename Digest>
static bool matches(Digest const& digest, char const* expected)
{
    return hex_matches({ digest.data(), digest.size() }, expected);
}

TEST_CASE(sha256_vectors)
{
    EXPECT(matches(Crypto::SHA256::hash(text("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    EXPECT(matches(Crypto::SHA256::hash(text("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    EXPECT(matches(Crypto::SHA256::hash(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST_CASE(sha256_in_pieces)
{
    // A million "a"s, fed in uneven pieces.
    static u8 as[1000];
    __builtin_memset(as, 'a', sizeof(as));
    Crypto::SHA256 hasher;
    size_t fed = 0;
    for (size_t piece = 1; fed < 1'000'000; piece = piece % 997 + 1) {
        size_t take = piece < 1'000'000 - fed ? piece : 1'000'000 - fed;
        hasher.update({ as, take });
        fed += take;
    }
    EXPECT(matches(hasher.digest(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST_CASE(sha512_vectors)
{
    EXPECT(matches(Crypto::SHA512::hash(text("")),
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    EXPECT(matches(Crypto::SHA512::hash(text("abc")),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    EXPECT(matches(Crypto::SHA512::hash(text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
}

TEST_CASE(hmac_vectors)
{
    u8 key[20];
    __builtin_memset(key, 0x0b, sizeof(key));
    Crypto::HMAC<Crypto::SHA256> first({ key, sizeof(key) });
    first.update(text("Hi There"));
    EXPECT(matches(first.digest(), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    Crypto::HMAC<Crypto::SHA256> jefe(text("Jefe"));
    jefe.update(text("what do ya want for nothing?"));
    EXPECT(matches(jefe.digest(), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    Crypto::HMAC<Crypto::SHA512> jefe512(text("Jefe"));
    jefe512.update(text("what do ya want for nothing?"));
    EXPECT(matches(jefe512.digest(),
        "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"));

    // A key longer than a block is hashed first.
    u8 long_key[131];
    __builtin_memset(long_key, 0xaa, sizeof(long_key));
    Crypto::HMAC<Crypto::SHA256> long_one({ long_key, sizeof(long_key) });
    long_one.update(text("Test Using Larger Than Block-Size Key - Hash Key First"));
    EXPECT(matches(long_one.digest(), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST_CASE(pbkdf2_vectors)
{
    u8 key[64];
    Crypto::pbkdf2<Crypto::SHA256>(text("passwd"), text("salt"), 1, { key, sizeof(key) });
    EXPECT(hex_matches({ key, sizeof(key) },
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    Crypto::pbkdf2<Crypto::SHA256>(text("Password"), text("NaCl"), 80000, { key, sizeof(key) });
    EXPECT(hex_matches({ key, sizeof(key) },
        "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    Crypto::pbkdf2<Crypto::SHA512>(text("password"), text("salt"), 2, { key, sizeof(key) });
    EXPECT(hex_matches({ key, sizeof(key) },
        "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e"));
}

TEST_CASE(constant_time_equal)
{
    EXPECT(Crypto::constant_time_equal(text("same"), text("same")));
    EXPECT(!Crypto::constant_time_equal(text("same"), text("sane")));
    EXPECT(!Crypto::constant_time_equal(text("short"), text("shorter")));
}
