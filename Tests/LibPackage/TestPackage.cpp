/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/String.h>
#include <LibCrypto/Curve25519.h>
#include <LibPackage/Catalog.h>
#include <LibTest/TestCase.h>

using namespace Package;

namespace {

ReadonlyBytes bytes_of(StringView text)
{
    return { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() };
}

constexpr StringView good_index = R"({
    "serial" : 7,
    "packages" : [
        { "name" : "hello", "version" : "1.0", "summary" : "A greeting", "file" : "hello-1.0.vpkg", "size" : 1234,
          "sha256" : "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" }
    ]
})"_sv;

}

TEST_CASE(an_index)
{
    u64 serial = 0;
    auto packages = MUST(parse_index(bytes_of(good_index), "https://repo.example/stable"_sv, "main"_sv, &serial));
    EXPECT_EQ(serial, static_cast<u64>(7));
    EXPECT_EQ(packages.size(), static_cast<size_t>(1));
    EXPECT(packages[0].name.view() == "hello"_sv);
    EXPECT(packages[0].url.view() == "https://repo.example/stable/hello-1.0.vpkg"_sv);
    EXPECT_EQ(packages[0].size, static_cast<u64>(1234));
    EXPECT(packages[0].repository.view() == "main"_sv);
}

TEST_CASE(bad_indexes)
{
    auto refused = [](StringView json) { return parse_index(bytes_of(json), "https://r"_sv, "r"_sv).is_error(); };
    EXPECT(refused("not json"_sv));
    EXPECT(refused("[]"_sv));
    EXPECT(refused("{}"_sv));
    EXPECT(refused(R"({"packages":{}})"_sv));
    EXPECT(refused(R"({"packages":[{"name":"x"}]})"_sv));
    // A file name that leaves the repository's folder, by any name.
    EXPECT(refused(R"({"packages":[{"name":"x","version":"1","file":"../../etc/passwd","size":1,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}]})"_sv));
    EXPECT(refused(R"({"packages":[{"name":"../x","version":"1","file":"x.vpkg","size":1,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}]})"_sv));
    EXPECT(refused(R"({"packages":[{"name":"x","version":"1","file":"x.vpkg","size":1,"sha256":"short"}]})"_sv));
}

TEST_CASE(signatures)
{
    u8 seed[32] = { 1, 2, 3, 4, 5 };
    u8 key[32];
    Crypto::ed25519_public_key(seed, key);
    u8 signature[64];
    auto data = bytes_of(good_index);
    Crypto::ed25519_sign(seed, data, signature);
    auto key_hex = hex_of({ key, 32 });
    auto signature_hex = hex_of({ signature, 64 });

    EXPECT(verify_signature(data, signature_hex.view(), key_hex.view()));
    // Anything changed, and it fails.
    auto changed = MUST(String::from_view(good_index));
    Vector<u8> tampered;
    for (size_t i = 0; i < data.size(); ++i)
        tampered.append(data[i]);
    tampered[20] ^= 1;
    EXPECT(!verify_signature(tampered.span(), signature_hex.view(), key_hex.view()));
    // Another's key.
    u8 other_seed[32] = { 9 };
    u8 other_key[32];
    Crypto::ed25519_public_key(other_seed, other_key);
    EXPECT(!verify_signature(data, signature_hex.view(), hex_of({ other_key, 32 }).view()));
    // Nonsense doesn't get far.
    EXPECT(!verify_signature(data, "abc"_sv, key_hex.view()));
    EXPECT(!verify_signature(data, signature_hex.view(), "abc"_sv));
    (void)changed;
}

TEST_CASE(where_packages_may_go)
{
    EXPECT(is_allowed_install_path("Applications/Hello.app/Contents/Info.plist"_sv));
    EXPECT(is_allowed_install_path("usr/local/bin/hello"_sv));
    EXPECT(!is_allowed_install_path("etc/passwd"_sv));
    EXPECT(!is_allowed_install_path("/Applications/Hello.app"_sv));
    EXPECT(!is_allowed_install_path("Applications/../etc/passwd"_sv));
    EXPECT(!is_allowed_install_path("usr/local/../../etc/passwd"_sv));
    EXPECT(!is_allowed_install_path("Applications//x"_sv));
    EXPECT(!is_allowed_install_path("Applications/./x"_sv));
    EXPECT(!is_allowed_install_path("System/Library/Kernels/vulture"_sv));
    EXPECT(!is_allowed_install_path("usr/bin/sudo"_sv));
    EXPECT(!is_allowed_install_path(""_sv));
}

TEST_CASE(names)
{
    EXPECT(is_valid_name("hello-world_2.0"_sv));
    EXPECT(!is_valid_name(""_sv));
    EXPECT(!is_valid_name("../x"_sv));
    EXPECT(!is_valid_name(".hidden"_sv));
    EXPECT(!is_valid_name("-rf"_sv));
    EXPECT(!is_valid_name("a b"_sv));
    EXPECT(!is_valid_name("a/b"_sv));
}
