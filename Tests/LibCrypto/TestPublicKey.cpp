/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Vector.h>
#include <LibCrypto/AES.h>
#include <LibCrypto/ChaCha20Poly1305.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/EllipticCurve.h>
#include <LibCrypto/GCM.h>
#include <LibCrypto/HKDF.h>
#include <LibCrypto/RSA.h>
#include <LibCrypto/SHA2.h>
#include <LibTest/TestCase.h>

// Known answers: FIPS 197 (AES), the GCM specification's test cases (and OpenSSL's answers
// for them), RFC 8439 (ChaCha20-Poly1305), RFC 5869 (HKDF), RFC 7748 (X25519), RFC 8032
// (Ed25519), RFC 5903 and 6979 (P-256 and P-384), and signatures OpenSSL made (RSA).

namespace {

u8 nibble(char c)
{
    return static_cast<u8>(c <= '9' ? c - '0' : (c >= 'a' ? c - 'a' + 10 : c - 'A' + 10));
}

Vector<u8> hex(char const* text)
{
    Vector<u8> bytes;
    size_t length = __builtin_strlen(text);
    for (size_t i = 0; i + 1 < length; i += 2)
        bytes.append(static_cast<u8>(nibble(text[i]) << 4 | nibble(text[i + 1])));
    return bytes;
}

bool same(u8 const* bytes, size_t size, char const* expected)
{
    auto wanted = hex(expected);
    if (wanted.size() != size)
        return false;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != wanted[i])
            return false;
    }
    return true;
}

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

}

TEST_CASE(sha384_vectors)
{
    EXPECT(same(Crypto::SHA384::hash(text("abc")).data(), 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
}

TEST_CASE(aes_block)
{
    auto plain = hex("00112233445566778899aabbccddeeff");
    u8 out[16];
    auto key128 = hex("000102030405060708090a0b0c0d0e0f");
    Crypto::AES aes128(key128.span());
    aes128.encrypt_block(plain.data(), out);
    EXPECT(same(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    auto key256 = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    Crypto::AES aes256(key256.span());
    aes256.encrypt_block(plain.data(), out);
    EXPECT(same(out, 16, "8ea2b7ca516745bfeafc49904b496089"));
}

TEST_CASE(aes_gcm)
{
    u8 tag[16];
    {
        // Test case 2: a zero key, a block of zeroes.
        auto key = hex("00000000000000000000000000000000");
        auto nonce = hex("000000000000000000000000");
        auto plain = hex("00000000000000000000000000000000");
        u8 out[16];
        Crypto::AESGCM gcm(key.span());
        gcm.seal(nonce.data(), {}, plain.span(), out, tag);
        EXPECT(same(out, 16, "0388dace60b6a392f328c2b971b2fe78"));
        EXPECT(same(tag, 16, "ab6e47d42cec13bdf53a67b21257bddf"));
    }
    auto nonce = hex("cafebabefacedbaddecaf888");
    auto plain = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    auto associated = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    Vector<u8> out;
    MUST(out.try_resize(plain.size()));
    {
        // Test case 4.
        auto key = hex("feffe9928665731c6d6a8f9467308308");
        Crypto::AESGCM gcm(key.span());
        gcm.seal(nonce.data(), associated.span(), plain.span(), out.data(), tag);
        EXPECT(same(out.data(), out.size(), "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"));
        EXPECT(same(tag, 16, "5bc94fbc3221a5db94fae95ae7121a47"));
        Vector<u8> back;
        MUST(back.try_resize(plain.size()));
        EXPECT(gcm.open(nonce.data(), associated.span(), out.span(), tag, back.data()));
        EXPECT(__builtin_memcmp(back.data(), plain.data(), plain.size()) == 0);
        // One bit wrong anywhere, and nothing comes out.
        tag[3] ^= 1;
        EXPECT(!gcm.open(nonce.data(), associated.span(), out.span(), tag, back.data()));
        EXPECT(back[0] == 0);
        tag[3] ^= 1;
        out[0] ^= 1;
        EXPECT(!gcm.open(nonce.data(), associated.span(), out.span(), tag, back.data()));
        out[0] ^= 1;
    }
    {
        // Test case 16: a key of 256 bits.
        auto key = hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        Crypto::AESGCM gcm(key.span());
        gcm.seal(nonce.data(), associated.span(), plain.span(), out.data(), tag);
        EXPECT(same(out.data(), out.size(), "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"));
        EXPECT(same(tag, 16, "76fc6ece0f4e1768cddf8853bb2d551b"));
    }
}

TEST_CASE(chacha20_block)
{
    auto key = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    auto nonce = hex("000000090000004a00000000");
    u8 block[64];
    Crypto::chacha20_block(key.data(), 1, nonce.data(), block);
    EXPECT(same(block, 64, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"));
}

TEST_CASE(poly1305)
{
    auto key = hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    u8 tag[16];
    Crypto::poly1305(key.data(), text("Cryptographic Forum Research Group"), tag);
    EXPECT(same(tag, 16, "a8061dc1305136c6c22b8baf0c0127a9"));
}

TEST_CASE(poly1305_carries)
{
    // RFC 8439, A.3, test vectors 8 to 11: numbers built to make the carries go all the way.
    u8 tag[16];
    auto key_a = hex("02000000000000000000000000000000ffffffffffffffffffffffffffffffff");
    auto message_a = hex("02000000000000000000000000000000");
    Crypto::poly1305(key_a.data(), message_a.span(), tag);
    EXPECT(same(tag, 16, "03000000000000000000000000000000"));
    auto key_b = hex("02000000000000000000000000000000");
    Vector<u8> key_full = key_b;
    for (int i = 0; i < 16; ++i)
        key_full.append(0);
    auto message_b = hex("ffffffffffffffffffffffffffffffff");
    Crypto::poly1305(key_full.data(), message_b.span(), tag);
    EXPECT(same(tag, 16, "03000000000000000000000000000000"));
    auto key_c = hex("01000000000000000000000000000000" "00000000000000000000000000000000");
    auto message_c = hex("fffffffffffffffffffffffffffffffff0ffffffffffffffffffffffffffffff11000000000000000000000000000000");
    Crypto::poly1305(key_c.data(), message_c.span(), tag);
    EXPECT(same(tag, 16, "05000000000000000000000000000000"));
}

TEST_CASE(chacha20_poly1305)
{
    auto key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    auto nonce = hex("070000004041424344454647");
    auto associated = hex("50515253c0c1c2c3c4c5c6c7");
    auto plain = text("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
    Vector<u8> out;
    MUST(out.try_resize(plain.size()));
    u8 tag[16];
    Crypto::ChaCha20Poly1305 aead(key.span());
    aead.seal(nonce.data(), associated.span(), plain, out.data(), tag);
    EXPECT(same(out.data(), 16, "d31a8d34648e60db7b86afbc53ef7ec2"));
    EXPECT(same(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"));
    Vector<u8> back;
    MUST(back.try_resize(plain.size()));
    EXPECT(aead.open(nonce.data(), associated.span(), out.span(), tag, back.data()));
    EXPECT(__builtin_memcmp(back.data(), plain.data(), plain.size()) == 0);
    tag[15] ^= 0x80;
    EXPECT(!aead.open(nonce.data(), associated.span(), out.span(), tag, back.data()));
}

TEST_CASE(hkdf)
{
    Vector<u8> input;
    for (int i = 0; i < 22; ++i)
        input.append(0x0b);
    auto salt = hex("000102030405060708090a0b0c");
    auto info = hex("f0f1f2f3f4f5f6f7f8f9");
    auto prk = Crypto::hkdf_extract<Crypto::SHA256>(salt.span(), input.span());
    EXPECT(same(prk.data(), 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
    u8 output[42];
    Crypto::hkdf_expand<Crypto::SHA256>(prk.span(), info.span(), { output, 42 });
    EXPECT(same(output, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
}

TEST_CASE(x25519)
{
    auto scalar = hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    auto point = hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    u8 out[32];
    Crypto::x25519(scalar.data(), point.data(), out);
    EXPECT(same(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

    auto alice = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    auto bob = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    u8 alice_public[32];
    u8 bob_public[32];
    Crypto::x25519_base(alice.data(), alice_public);
    Crypto::x25519_base(bob.data(), bob_public);
    EXPECT(same(alice_public, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    EXPECT(same(bob_public, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    Crypto::x25519(alice.data(), bob_public, out);
    EXPECT(same(out, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    Crypto::x25519(bob.data(), alice_public, out);
    EXPECT(same(out, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
}

TEST_CASE(ed25519)
{
    struct Vector1 {
        char const* seed;
        char const* public_key;
        char const* message;
        char const* signature;
    };
    static constexpr Vector1 const vectors[] = {
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    };
    for (auto const& vector : vectors) {
        auto seed = hex(vector.seed);
        auto message = hex(vector.message);
        u8 public_key[32];
        Crypto::ed25519_public_key(seed.data(), public_key);
        EXPECT(same(public_key, 32, vector.public_key));
        u8 signature[64];
        Crypto::ed25519_sign(seed.data(), message.span(), signature);
        EXPECT(same(signature, 64, vector.signature));
        EXPECT(Crypto::ed25519_verify(public_key, message.span(), signature));
        signature[10] ^= 1;
        EXPECT(!Crypto::ed25519_verify(public_key, message.span(), signature));
        signature[10] ^= 1;
        signature[40] ^= 1;
        EXPECT(!Crypto::ed25519_verify(public_key, message.span(), signature));
    }
}

TEST_CASE(ed25519_refuses_a_large_s)
{
    // S + L for a good signature would pass a check that does not look at S's size.
    auto seed = hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    u8 public_key[32];
    Crypto::ed25519_public_key(seed.data(), public_key);
    u8 signature[64];
    Crypto::ed25519_sign(seed.data(), {}, signature);
    static constexpr u8 order[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
    unsigned carry = 0;
    for (int i = 0; i < 32; ++i) {
        unsigned sum = signature[32 + i] + order[i] + carry;
        signature[32 + i] = static_cast<u8>(sum);
        carry = sum >> 8;
    }
    EXPECT(!Crypto::ed25519_verify(public_key, {}, signature));
}

TEST_CASE(p256_key_agreement)
{
    auto i = hex("c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433");
    auto r = hex("c6ef9c5d78ae012a011164acb397ce2088685d8f06bf9be0b283ab46476bee53");
    u8 public_i[65];
    EXPECT(Crypto::ec_public_key(Crypto::NamedCurve::P256, i.span(), { public_i, 65 }));
    EXPECT(same(public_i, 65, "04dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c37725811805271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3"));
    u8 public_r[65];
    EXPECT(Crypto::ec_public_key(Crypto::NamedCurve::P256, r.span(), { public_r, 65 }));
    EXPECT(same(public_r, 65, "04d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf6356fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab"));
    u8 shared[32];
    EXPECT(Crypto::ecdh(Crypto::NamedCurve::P256, i.span(), { public_r, 65 }, { shared, 32 }));
    EXPECT(same(shared, 32, "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de"));
    EXPECT(Crypto::ecdh(Crypto::NamedCurve::P256, r.span(), { public_i, 65 }, { shared, 32 }));
    EXPECT(same(shared, 32, "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de"));
    // A point off the curve gets no answer at all.
    public_r[40] ^= 1;
    EXPECT(!Crypto::ecdh(Crypto::NamedCurve::P256, i.span(), { public_r, 65 }, { shared, 32 }));
}

TEST_CASE(p384_key_agreement)
{
    auto i = hex("099f3c7034d4a2c699884d73a375a67f7624ef7c6b3c0f160647b67414dce655e35b538041e649ee3faef896783ab194");
    auto r = hex("41cb0779b4bdb85d47846725fbec3c9430fab46cc8dc5060855cc9bda0aa2942e0308312916b8ed2960e4bd55a7448fc");
    u8 public_i[97];
    EXPECT(Crypto::ec_public_key(Crypto::NamedCurve::P384, i.span(), { public_i, 97 }));
    EXPECT(same(public_i, 97, "04667842d7d180ac2cde6f74f37551f55755c7645c20ef73e31634fe72b4c55ee6de3ac808acb4bdb4c88732aee95f41aa9482ed1fc0eeb9cafc4984625ccfc23f65032149e0e144ada024181535a0f38eeb9fcff3c2c947dae69b4c634573a81c"));
    u8 public_r[97];
    EXPECT(Crypto::ec_public_key(Crypto::NamedCurve::P384, r.span(), { public_r, 97 }));
    u8 shared[48];
    EXPECT(Crypto::ecdh(Crypto::NamedCurve::P384, i.span(), { public_r, 97 }, { shared, 48 }));
    EXPECT(same(shared, 48, "11187331c279962d93d604243fd592cb9d0a926f422e47187521287e7156c5c4d603135569b9e9d09cf5d4a270f59746"));
}

TEST_CASE(ecdsa)
{
    {
        auto key = hex("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
        auto digest = Crypto::SHA256::hash(text("sample"));
        auto r = hex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716");
        auto s = hex("f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");
        EXPECT(Crypto::ecdsa_verify(Crypto::NamedCurve::P256, key.span(), digest.span(), r.span(), s.span()));
        auto other = Crypto::SHA256::hash(text("test"));
        EXPECT(!Crypto::ecdsa_verify(Crypto::NamedCurve::P256, key.span(), other.span(), r.span(), s.span()));
        s[5] ^= 1;
        EXPECT(!Crypto::ecdsa_verify(Crypto::NamedCurve::P256, key.span(), digest.span(), r.span(), s.span()));
        // Nothing of nothing.
        Vector<u8> zero;
        zero.append(0);
        EXPECT(!Crypto::ecdsa_verify(Crypto::NamedCurve::P256, key.span(), digest.span(), zero.span(), r.span()));
    }
    {
        auto key = hex("04ec3a4e415b4e19a4568618029f427fa5da9a8bc4ae92e02e06aae5286b300c64def8f0ea9055866064a254515480bc138015d9b72d7d57244ea8ef9ac0c621896708a59367f9dfb9f54ca84b3f1c9db1288b231c3ae0d4fe7344fd2533264720");
        auto digest = Crypto::SHA384::hash(text("sample"));
        auto r = hex("94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c81a648152e44acf96e36dd1e80fabe46");
        auto s = hex("99ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94fa329c145786e679e7b82c71a38628ac8");
        EXPECT(Crypto::ecdsa_verify(Crypto::NamedCurve::P384, key.span(), digest.span(), r.span(), s.span()));
        s[0] ^= 1;
        EXPECT(!Crypto::ecdsa_verify(Crypto::NamedCurve::P384, key.span(), digest.span(), r.span(), s.span()));
    }
}

TEST_CASE(rsa)
{
    auto modulus = hex("b87e577cbbf95bdc0854eebe57c81a36105b09bee4b6e2fb1e64869ffe53c887ebcc59387127129a331cc3ab7e521b0ea954a716ab5cd10e74f38873d66505bdac2108d813e0fe5c4793ac3dbecf4511aca9226921dfdb772e3667c5da29fe5454e84e309f9f565b5502a527896ce173fcf0216eeb6a449246639089fe8eeadb6aad9cced6b462e8122733986d555cece85389055377fdd6bdacbaaceec418ded51791c6c2a9822a117512e47e9ee06d54da14efa4bc1fe4c6dbf46c0a8fe872cc7baa172ceb7c99f6147ce4d41ada576e3c96f41a88112596013097e5da61d26b2de5953bff52cd15d3736374864f875ee806d0fa3eb23f11f91d67b12b7dbf");
    auto exponent = hex("010001");
    Crypto::RSAPublicKey key { Crypto::BigInteger::from_bytes(modulus.span()), Crypto::BigInteger::from_bytes(exponent.span()) };
    auto message = text("vultureOS checks signatures");
    auto wrong = text("vultureOS checks signature!");

    auto pkcs1_256 = hex("900c15708d145d830f699a72e2c79dcd43a1e7585a217eeecc117e90c8027b61cf74eafbc6835acb5edc755867291d08d9b5f02eb44082254ead42a35aee021197d7f7c6a2eacf2778d758e78e14ba228441b4c3d62309f62777e03a898a842a443f287a20884ebfa5c2c861d519cc7ee0bb8db098d083c058b5f38f0f2740a82a607793ee974971e2b2563d04d7d62d80b3e7ec39adb6e5214678af24770b0e303dde6869edf5f5f4009f7806a0aaeea5e5e43b72ea2622d2544c55561e7deeadd7dad250e6d6e35264315ba0e702d1d5f71998a47250f01aeec1a0b8bf26e8f6f26b82fdbcd385c5697f586cc2b58b8120d2eb4cd646192f6af7c803d53510");
    EXPECT(Crypto::rsa_verify_pkcs1(key, Crypto::SignatureHash::SHA256, message, pkcs1_256.span()));
    EXPECT(!Crypto::rsa_verify_pkcs1(key, Crypto::SignatureHash::SHA256, wrong, pkcs1_256.span()));
    EXPECT(!Crypto::rsa_verify_pkcs1(key, Crypto::SignatureHash::SHA384, message, pkcs1_256.span()));
    auto pkcs1_384 = hex("001ac7ae7b9dbb222fad806254869560456c1d62a15b4e2e87373641f2f0625bb06ef4f3fe5b0d40c2cce5c35f3b84b267f13d915765fcb3641320485b45b598e9625ee1f90c7223929ed131a1aa0f7a5e84bbc92bdc124e4e655fc5e202ef5cda514bd734bd4d73bb2c850d68006c269198fbf2197ecf3fb8fb1fce135a87e7d96101b17881f92022e89ac7bf592745973799dbfe4662389b4c1a700e499b5e481e07fbf9c61c680c2251e530f1d9fba1dea48b3397a3a558e382afb9dda6ff5c5fb269a14781da2d7f8cd5de48ecda9edab4c7ba1a2f192126c9089841ee601060e13e0b9d0643f84b12de8e8b0671aa1d34af86f6e222a1103fbb31621507");
    EXPECT(Crypto::rsa_verify_pkcs1(key, Crypto::SignatureHash::SHA384, message, pkcs1_384.span()));

    auto pss_256 = hex("8d18082cb1b2db244df5426703abfe89733a6659d7b32b784b7a870c06bc1a5aa0faa7ff59c20d3f604618ffd3b0367957d7e0fff23a109db1fe52326ae7c9d111028b02dbf28a8081343f199ec953ac4cedb3e92ba047fc0bc91e470b77b28047d103ad5e8056a4805b8d335cf51e7dc392f460b77187c848502db596c9b33ad8a0dc642587d09957d055cc4adb5bf590487a3dc4d387e9e4b43c5beaafdcdcb155f03702d2279addf4482461b5f88e35747c498282e2144973782c7e98b1bc3e87504b3243caa7def8cbd8ecb6baab88b76b5a4f6ecec1dce561aa97639c8d7018c04bbfae3af7a2c9e93cf465c6419ad00e87c1e6dc78df9ee7ad8a1539f9");
    EXPECT(Crypto::rsa_verify_pss(key, Crypto::SignatureHash::SHA256, message, pss_256.span()));
    EXPECT(!Crypto::rsa_verify_pss(key, Crypto::SignatureHash::SHA256, wrong, pss_256.span()));
    auto pss_512 = hex("aaf8a7362bade976f511f2391fad1ec852cea5a3d67a18ac71d4ae77cb07d0755197df555037d2491dfd1b231f85e2e1412dd4d5e4916e470b8293e9e4d5fd46955c1ebae46f38f896f04af1311b3b7ac6bdb6f852fcc7730a77f306176a3f1df6e221a66ed8d602e833913734c58a4ec6890a8de1cbdce82a3ef72c7b294ba19a8f26f93d451cc18ee42ec1bdf4cf9caf1be3bdd0cbfa0089af4af55eaf2db1fc86fc0e3a2282b52a0ddcd307f3652075b281f98afffaa9f183573b7ec1bcf59e981ebfae875d63f7cc5acc359151c5bf85e1d4f12e65fb0aa349be2dbf80d524ac684ec43ecaf466828a9de08e43d30d823f88837d877b9bcae489298500ed");
    EXPECT(Crypto::rsa_verify_pss(key, Crypto::SignatureHash::SHA512, message, pss_512.span()));
    pss_512[100] ^= 1;
    EXPECT(!Crypto::rsa_verify_pss(key, Crypto::SignatureHash::SHA512, message, pss_512.span()));
}
