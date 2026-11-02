/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Vector.h>
#include <LibTLS/Certificate.h>
#include <LibTest/TestCase.h>

// Certificates OpenSSL made, as a real authority would, for a chain that is good and for each
// way one can be bad. (Their private keys are not kept anywhere.)

using namespace TLS;

namespace {

u8 nibble(char c)
{
    return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
}

Vector<u8> hex(char const* text)
{
    Vector<u8> bytes;
    size_t length = __builtin_strlen(text);
    for (size_t i = 0; i + 1 < length; i += 2)
        bytes.append(static_cast<u8>(nibble(text[i]) << 4 | nibble(text[i + 1])));
    return bytes;
}

constexpr char const* root_der = "308201ce30820173a00302010202145eb3111b2a497c65837c6b75bfc7d834338d33d6300a06082a8648ce3d040302303331153013060355040a0c0c56756c747572652054657374311a301806035504030c1156756c74757265205465737420526f6f743020170d3230303130313030303030305a180f32303930303130313030303030305a303331153013060355040a0c0c56756c747572652054657374311a301806035504030c1156756c74757265205465737420526f6f743059301306072a8648ce3d020106082a8648ce3d03010703420004e3d13134ba030eb5be8a8178c0a34677013b2a35924d6a34d27e84fa2942a05e6246021a52b8300a66cd7d34a2b0ccfd2362a53d66f23342e24dea3c71e98666a3633061301d0603551d0e04160414158b229c19ba1edafa3963be40f65da851592ec6301f0603551d23041830168014158b229c19ba1edafa3963be40f65da851592ec6300f0603551d130101ff040530030101ff300e0603551d0f0101ff040403020106300a06082a8648ce3d04030203490030460221009484e54a9395741135d6fbcd5b6966a38242aed47a5064284981dff8731bad9b022100dd0e1650f06a66fb7a8b687f4b33f3ef433796b7aaaa178efac20ca796960113";
constexpr char const* intermediate_der = "308202a330820249a00302010202146d8730a6196844831021f11e4dc9590e797df6bd300a06082a8648ce3d040303303331153013060355040a0c0c56756c747572652054657374311a301806035504030c1156756c74757265205465737420526f6f743020170d3230303130313030303030305a180f32303830303130313030303030305a303b31153013060355040a0c0c56756c7475726520546573743122302006035504030c1956756c74757265205465737420496e7465726d65646961746530820122300d06092a864886f70d01010105000382010f003082010a0282010100ba227d34dcb19cac99f15df7fc3180f92de323695103aec5d4d9b725ddaacdc3f0bcaa8c0b0535d840512f894463fd74abbb61ded58b33ec780154949175ede132766e68d10757156b556a6674d3093bbaa3d61b312c9fdef646ad9e6fa3d4a8d79ef3313ecd68a8ea76ec6791aed79bf96ed00496c351b92c4b2bea2a48af2eb3c27dd1837fe489e9c5d0683023e51fbcd015e0156dda77d22028d85db18992b538b7c01bcb22f6f1d068bf6586fafc332950af692417ccd00d93b9fe47673fa51e1b79b69207c8767eeb7b39e7c80c274a557160c64cfd868dd62e0eeb5b103380c6b02b73b36f3906aecf4c94890553cfdf37cd0ac82ae21ef2d17b7b6b7d0203010001a366306430120603551d130101ff040830060101ff020100300e0603551d0f0101ff040403020106301d0603551d0e041604146eab572b8ade6217612fcd7076d15732ba9d3878301f0603551d23041830168014158b229c19ba1edafa3963be40f65da851592ec6300a06082a8648ce3d040303034800304502202a5f83fcba8c39de31ec1d56059a7476afaeeb5fef640c7911b970047651bea9022100e4755e4d94ce73f3881dca860f0b3daf9ee4a3c4d20ae9c3b10b99e6bb84e1a6";
constexpr char const* leaf_der = "308202e4308201cca003020102021430eaa2d2ee3dd973251af1a4ec56ec65e001e9d0300d06092a864886f70d01010b0500303b31153013060355040a0c0c56756c7475726520546573743122302006035504030c1956756c74757265205465737420496e7465726d6564696174653020170d3230303130313030303030305a180f32303730303130313030303030305a301b3119301706035504030c107777772e76756c747572652e746573743076301006072a8648ce3d020106052b810400220362000493e83123c0ed18f3dc19757ab65851754b0494bf0697b5949e1eb52e31a6349b3d534c88b5dcc9a82a2ac6a8c4594d08e2b8611f4aee241340da51bdde319e7775fcd23dd3e46d09b2cfeaa024305da9c3aa48af1f3a20b1d6e6bc59dd654921a381ab3081a830090603551d1304023000300e0603551d0f0101ff04040302078030130603551d25040c300a06082b0601050507030130360603551d11042f302d82107777772e76756c747572652e7465737482132a2e617070732e76756c747572652e7465737487040a000202301d0603551d0e04160414dafa2b543a9758d8607ce88492a7c457fd6c5dca301f0603551d230418301680146eab572b8ade6217612fcd7076d15732ba9d3878300d06092a864886f70d01010b05000382010100348124199052cf23a45c41697889473836e6382675254f8eb925495cfc2ee41c02088458c346487131340dcc44fcc7b84f141eb27e5970bb71c129631823ae1d2560729ee80fca4ed3a370155d4002f0eb449c0f0d34fbb6d3cbb6b1f6a8e4c81a6f61450e681e4be3fcd4f26a9c81560e2644486181b372708babb6128c9620e126b3f7b32636a9a4452a096efcd6563969b18ba7e79d40b3eaf83f8f74ea309320b53303df648f9215d9623c305c7b9a5015b040838635a862c184d120ce5bbe33dfe67c9e1c54d8bcbf1d573ca9c4272f2fa42a3970c8c8a380ce12e0efa0f417e3d7011ca73d8fdc523b53c97aeedb7d6118227cc9d8274f60ea8199c656";
constexpr char const* expired_der = "308202e2308201caa003020102021430eaa2d2ee3dd973251af1a4ec56ec65e001e9d1300d06092a864886f70d01010b0500303b31153013060355040a0c0c56756c7475726520546573743122302006035504030c1956756c74757265205465737420496e7465726d656469617465301e170d3030303130313030303030305a170d3130303130313030303030305a301b3119301706035504030c107777772e76756c747572652e746573743076301006072a8648ce3d020106052b810400220362000493e83123c0ed18f3dc19757ab65851754b0494bf0697b5949e1eb52e31a6349b3d534c88b5dcc9a82a2ac6a8c4594d08e2b8611f4aee241340da51bdde319e7775fcd23dd3e46d09b2cfeaa024305da9c3aa48af1f3a20b1d6e6bc59dd654921a381ab3081a830090603551d1304023000300e0603551d0f0101ff04040302078030130603551d25040c300a06082b0601050507030130360603551d11042f302d82107777772e76756c747572652e7465737482132a2e617070732e76756c747572652e7465737487040a000202301d0603551d0e04160414dafa2b543a9758d8607ce88492a7c457fd6c5dca301f0603551d230418301680146eab572b8ade6217612fcd7076d15732ba9d3878300d06092a864886f70d01010b05000382010100725c360c2b5b73c025d9fdd1e6ac14eb8270eb08db00682c7f2686caf498f6ccb14c8cead41078445fc765d6eafe83a333ddf15457e9ff324ab36f45bb321613347b1e0602e8a1453dec138c9430466faf0b05d5ecd028d3a48ac81024a512db4a31debde865da6ce71b401660c5f36e3d3cf69323ed936e67f04f9816d5a17c07e6f43d4c52767114667b8ab797f735d96011801efa52a46f0ac55c1e45b876bcad88e99b1daee12a777748d8d6d4094dbba2edd92862d5da23a9af455714874814157da7f95f9fd69018e51d077b48fbaa08c95af46490a81b7377ef85b281e66fb1ca73b00af6a18eaa41107aef0cab4e234c348567d5c8810011c4786211";
constexpr char const* signed_by_leaf_der = "308202f230820277a00302010202142e9deb3f24f13f4e1aed9cb66b26625cd0307dea300a06082a8648ce3d040302301b3119301706035504030c107777772e76756c747572652e746573743020170d3230303130313030303030305a180f32303630303130313030303030305a303b31153013060355040a0c0c56756c7475726520546573743122302006035504030c1956756c74757265205465737420496e7465726d65646961746530820122300d06092a864886f70d01010105000382010f003082010a0282010100ba227d34dcb19cac99f15df7fc3180f92de323695103aec5d4d9b725ddaacdc3f0bcaa8c0b0535d840512f894463fd74abbb61ded58b33ec780154949175ede132766e68d10757156b556a6674d3093bbaa3d61b312c9fdef646ad9e6fa3d4a8d79ef3313ecd68a8ea76ec6791aed79bf96ed00496c351b92c4b2bea2a48af2eb3c27dd1837fe489e9c5d0683023e51fbcd015e0156dda77d22028d85db18992b538b7c01bcb22f6f1d068bf6586fafc332950af692417ccd00d93b9fe47673fa51e1b79b69207c8767eeb7b39e7c80c274a557160c64cfd868dd62e0eeb5b103380c6b02b73b36f3906aecf4c94890553cfdf37cd0ac82ae21ef2d17b7b6b7d0203010001a381ab3081a830090603551d1304023000300e0603551d0f0101ff04040302078030130603551d25040c300a06082b0601050507030130360603551d11042f302d82107777772e76756c747572652e7465737482132a2e617070732e76756c747572652e7465737487040a000202301d0603551d0e041604146eab572b8ade6217612fcd7076d15732ba9d3878301f0603551d23041830168014dafa2b543a9758d8607ce88492a7c457fd6c5dca300a06082a8648ce3d0403020369003066023100b0aba69a788838f21076804b667475c3b99c4c3746db5c8ed76ca405cb1021617bce2479b0236fd2ad2643b0ee8812ff02310082477704d6ac9ead463aaa1e6addd8575029c17147f1c0ee0f51416d1ad00d4003476cbf92127832063eec2b273705dc";
constexpr char const* ed25519_der = "3082013a3081eda003020102021465a8c24a5a8c0e31dfae9f071d7d8056132e8efc300506032b657030123110300e06035504030c07456420526f6f743020170d3230303130313030303030305a180f32303930303130313030303030305a30123110300e06035504030c07456420526f6f74302a300506032b65700321001e4a68a727cdd9366a7eb75e3575a7f5bea01dbe61c12940eae140b5818ca93ca3533051301d0603551d0e04160414de0597d553a9c92f5e1636df4691efd15524d2f1301f0603551d23041830168014de0597d553a9c92f5e1636df4691efd15524d2f1300f0603551d130101ff040530030101ff300506032b6570034100a02dadd383269c01dd2843ea632b514e5d36c166bed1db2e39d703b123dd300f85cf0aea48bab5e2b24981508f54851adc74c7e630ef82941ff6531c74fc8e03";
constexpr char const* pss_der = "3082037130820225a00302010202146617817cc9b4b0d00aa0b7b64564e694763b4261304106092a864886f70d01010a3034a00f300d06096086480165030402010500a11c301a06092a864886f70d010108300d06096086480165030402010500a20302012030133111300f06035504030c0850535320526f6f743020170d3230303130313030303030305a180f32303930303130313030303030305a30133111300f06035504030c0850535320526f6f7430820122300d06092a864886f70d01010105000382010f003082010a0282010100ba227d34dcb19cac99f15df7fc3180f92de323695103aec5d4d9b725ddaacdc3f0bcaa8c0b0535d840512f894463fd74abbb61ded58b33ec780154949175ede132766e68d10757156b556a6674d3093bbaa3d61b312c9fdef646ad9e6fa3d4a8d79ef3313ecd68a8ea76ec6791aed79bf96ed00496c351b92c4b2bea2a48af2eb3c27dd1837fe489e9c5d0683023e51fbcd015e0156dda77d22028d85db18992b538b7c01bcb22f6f1d068bf6586fafc332950af692417ccd00d93b9fe47673fa51e1b79b69207c8767eeb7b39e7c80c274a557160c64cfd868dd62e0eeb5b103380c6b02b73b36f3906aecf4c94890553cfdf37cd0ac82ae21ef2d17b7b6b7d0203010001a3533051301d0603551d0e041604146eab572b8ade6217612fcd7076d15732ba9d3878301f0603551d230418301680146eab572b8ade6217612fcd7076d15732ba9d3878300f0603551d130101ff040530030101ff304106092a864886f70d01010a3034a00f300d06096086480165030402010500a11c301a06092a864886f70d010108300d06096086480165030402010500a2030201200382010100b53847d39557d8cce913da2aa92edf68902c96ac469e20bc9d73d841ab4c347e321710519ff014d472b2cf8720ae7a3e8b8881dd2ca07e00e13bbc5c8454e9e2d57a9dccfa33ec247db3a14e56b493fd435c1381f080450276e2169002b8596e65b5d39fb4684c88f80b71cb28375b221c90d2aa6fd26e4d586aedfa1b9a051d281f20dddd7b855502d70f966322524cab5e87779c56efa09c1db7bae89accbe6abc4505c7d9b798ce802723f105b16ac8be2cb57384b9c3d23a508be79f7ed8405db2f125d31707e79e5034d4f1f6e75b0082a9d1b9a3639ffe1aff7546e5b6f93c6b2b24a4bdbcc9e014b0d8bb123ec420ff3f2c6402c795caa0ebf8562513";
constexpr char const* root_pem = R"(-----BEGIN CERTIFICATE-----
MIIBzjCCAXOgAwIBAgIUXrMRGypJfGWDfGt1v8fYNDONM9YwCgYIKoZIzj0EAwIw
MzEVMBMGA1UECgwMVnVsdHVyZSBUZXN0MRowGAYDVQQDDBFWdWx0dXJlIFRlc3Qg
Um9vdDAgFw0yMDAxMDEwMDAwMDBaGA8yMDkwMDEwMTAwMDAwMFowMzEVMBMGA1UE
CgwMVnVsdHVyZSBUZXN0MRowGAYDVQQDDBFWdWx0dXJlIFRlc3QgUm9vdDBZMBMG
ByqGSM49AgEGCCqGSM49AwEHA0IABOPRMTS6Aw61voqBeMCjRncBOyo1kk1qNNJ+
hPopQqBeYkYCGlK4MApmzX00orDM/SNipT1m8jNC4k3qPHHphmajYzBhMB0GA1Ud
DgQWBBQViyKcGboe2vo5Y75A9l2oUVkuxjAfBgNVHSMEGDAWgBQViyKcGboe2vo5
Y75A9l2oUVkuxjAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAKBggq
hkjOPQQDAgNJADBGAiEAlITlSpOVdBE11vvNW2lmo4JCrtR6UGQoSYHf+HMbrZsC
IQDdDhZQ8Gpm+3qLaH9LM/PvQzeWt6qqF476wgynlpYBEw==
-----END CERTIFICATE-----
)";

// 2030, well inside everything but the expired one.
constexpr i64 now = 1893456000;

NonnullOwnPtr<Certificate> parsed(char const* der)
{
    auto bytes = hex(der);
    return MUST(Certificate::parse(bytes.span()));
}

TrustStore store_with_root()
{
    TrustStore store;
    auto bytes = hex(root_der);
    MUST(store.add_der(bytes.span()));
    return store;
}

Vector<NonnullOwnPtr<Certificate>> good_chain()
{
    Vector<NonnullOwnPtr<Certificate>> chain;
    chain.append(parsed(leaf_der));
    chain.append(parsed(intermediate_der));
    return chain;
}

bool fails_with(ErrorOr<void> result, StringView what)
{
    return result.is_error() && result.error().string_literal().contains(what);
}

}

TEST_CASE(what_a_certificate_says)
{
    auto leaf = parsed(leaf_der);
    EXPECT(leaf->key.kind == KeyKind::ECDSA);
    EXPECT(leaf->key.curve == Crypto::NamedCurve::P384);
    EXPECT(leaf->signature_kind == SignatureKind::RSA_PKCS1);
    EXPECT(leaf->signature_hash == Crypto::SignatureHash::SHA256);
    EXPECT(!leaf->is_authority);
    EXPECT(leaf->has_key_usage && leaf->may_sign_handshakes && !leaf->may_sign_certificates);
    EXPECT(leaf->has_extended_usage && leaf->for_server_authentication);
    EXPECT_EQ(leaf->dns_names.size(), static_cast<size_t>(2));
    EXPECT_EQ(leaf->ip_addresses.size(), static_cast<size_t>(1));
    EXPECT_EQ(leaf->ip_addresses[0], 0x0a000202u);
    EXPECT(leaf->subject_summary().view() == "www.vulture.test"_sv);
    EXPECT(leaf->issuer_summary().view() == "Vulture Test Intermediate"_sv);
    // 2020-01-01 and 2070-01-01.
    EXPECT_EQ(leaf->not_before, static_cast<i64>(1577836800));
    EXPECT_EQ(leaf->not_after, static_cast<i64>(3155760000));

    auto intermediate = parsed(intermediate_der);
    EXPECT(intermediate->is_authority);
    EXPECT_EQ(intermediate->path_length, 0);
    EXPECT(intermediate->key.kind == KeyKind::RSA);
    EXPECT(intermediate->signature_kind == SignatureKind::ECDSA);
    EXPECT(intermediate->signature_hash == Crypto::SignatureHash::SHA384);
}

TEST_CASE(a_good_chain)
{
    auto store = store_with_root();
    auto chain = good_chain();
    EXPECT(!verify_chain(chain, store, "www.vulture.test"_sv, now).is_error());
    // Names are not case-sensitive, and a final dot means nothing.
    EXPECT(!verify_chain(chain, store, "WWW.Vulture.TEST"_sv, now).is_error());
    EXPECT(!verify_chain(chain, store, "www.vulture.test."_sv, now).is_error());
    // As an address too.
    EXPECT(!verify_chain(chain, store, "10.0.2.2"_sv, now).is_error());
    // A wildcard, for one label exactly.
    EXPECT(!verify_chain(chain, store, "foo.apps.vulture.test"_sv, now).is_error());
}

TEST_CASE(the_wrong_host)
{
    auto store = store_with_root();
    auto chain = good_chain();
    EXPECT(fails_with(verify_chain(chain, store, "www.vulture.example"_sv, now), "host name"_sv));
    EXPECT(fails_with(verify_chain(chain, store, "vulture.test"_sv, now), "host name"_sv));
    EXPECT(fails_with(verify_chain(chain, store, "a.b.apps.vulture.test"_sv, now), "host name"_sv));
    EXPECT(fails_with(verify_chain(chain, store, "apps.vulture.test"_sv, now), "host name"_sv));
    EXPECT(fails_with(verify_chain(chain, store, "10.0.2.3"_sv, now), "host name"_sv));
    EXPECT(fails_with(verify_chain(chain, store, "www.vulture.test.evil.example"_sv, now), "host name"_sv));
}

TEST_CASE(time_matters)
{
    auto store = store_with_root();
    auto chain = good_chain();
    // Before anything was valid.
    EXPECT(fails_with(verify_chain(chain, store, "www.vulture.test"_sv, 1000), "not yet valid"_sv));
    // After the leaf has gone.
    EXPECT(fails_with(verify_chain(chain, store, "www.vulture.test"_sv, 3155760001), "expired"_sv));

    Vector<NonnullOwnPtr<Certificate>> expired;
    expired.append(parsed(expired_der));
    expired.append(parsed(intermediate_der));
    EXPECT(fails_with(verify_chain(expired, store, "www.vulture.test"_sv, now), "expired"_sv));
}

TEST_CASE(an_unknown_authority)
{
    TrustStore empty;
    auto chain = good_chain();
    EXPECT(fails_with(verify_chain(chain, empty, "www.vulture.test"_sv, now), "local issuer"_sv));

    // No intermediate sent: nothing connects the leaf to the root.
    auto store = store_with_root();
    Vector<NonnullOwnPtr<Certificate>> alone;
    alone.append(parsed(leaf_der));
    EXPECT(fails_with(verify_chain(alone, store, "www.vulture.test"_sv, now), "local issuer"_sv));
}

TEST_CASE(a_tampered_certificate)
{
    auto store = store_with_root();
    // One bit of the name it claims, then one bit of the signature.
    auto bytes = hex(leaf_der);
    bytes[bytes.size() / 3] ^= 0x10;
    auto changed = Certificate::parse(bytes.span());
    if (!changed.is_error()) {
        Vector<NonnullOwnPtr<Certificate>> chain;
        chain.append(changed.release_value());
        chain.append(parsed(intermediate_der));
        EXPECT(verify_chain(chain, store, "www.vulture.test"_sv, now).is_error());
    }
    auto signature_changed = hex(leaf_der);
    signature_changed[signature_changed.size() - 5] ^= 0x01;
    Vector<NonnullOwnPtr<Certificate>> chain;
    chain.append(MUST(Certificate::parse(signature_changed.span())));
    chain.append(parsed(intermediate_der));
    EXPECT(fails_with(verify_chain(chain, store, "www.vulture.test"_sv, now), "signature"_sv));
}

TEST_CASE(not_an_authority)
{
    // A chain through a certificate that may not sign: the leaf, signing a look-alike of
    // the intermediate, which then signs nothing good.
    auto store = store_with_root();
    Vector<NonnullOwnPtr<Certificate>> chain;
    chain.append(parsed(signed_by_leaf_der));
    chain.append(parsed(leaf_der));
    chain.append(parsed(intermediate_der));
    auto result = verify_chain(chain, store, "www.vulture.test"_sv, now);
    EXPECT(fails_with(move(result), "not a certificate authority"_sv));
}

TEST_CASE(garbage_is_not_a_certificate)
{
    auto bytes = hex(leaf_der);
    // Cut short anywhere, it doesn't parse, and nothing reads past the end. (The sanitizers
    // would say.)
    for (size_t size = 0; size < bytes.size(); size += 7)
        EXPECT(Certificate::parse({ bytes.data(), size }).is_error());
    // Each byte wrong in turn: it may parse or not, but never goes wrong.
    for (size_t i = 0; i < bytes.size(); i += 3) {
        u8 const before = bytes[i];
        bytes[i] = static_cast<u8>(before ^ 0xff);
        (void)Certificate::parse(bytes.span());
        bytes[i] = before;
    }
}

TEST_CASE(signature_algorithms)
{
    // Self-signed, each by a different algorithm.
    auto ed = parsed(ed25519_der);
    EXPECT(ed->signature_kind == SignatureKind::Ed25519);
    EXPECT(verify_signature(ed->key, ed->signature_kind, ed->signature_hash, ed->to_be_signed, ed->signature));
    auto pss = parsed(pss_der);
    EXPECT(pss->signature_kind == SignatureKind::RSA_PSS);
    EXPECT(pss->signature_hash == Crypto::SignatureHash::SHA256);
    EXPECT(verify_signature(pss->key, pss->signature_kind, pss->signature_hash, pss->to_be_signed, pss->signature));
    auto root = parsed(root_der);
    EXPECT(root->signature_kind == SignatureKind::ECDSA);
    EXPECT(verify_signature(root->key, root->signature_kind, root->signature_hash, root->to_be_signed, root->signature));
}

TEST_CASE(pem_files)
{
    TrustStore store;
    StringBuilder text;
    text.append("# a comment\n"_sv);
    text.append(StringView { root_pem, __builtin_strlen(root_pem) });
    text.append("\nsome text in between\n"_sv);
    text.append(StringView { root_pem, __builtin_strlen(root_pem) });
    auto view = text.view();
    store.add_pem({ reinterpret_cast<u8 const*>(view.characters_without_null_termination()), view.length() });
    EXPECT_EQ(store.size(), static_cast<size_t>(2));
    auto chain = good_chain();
    EXPECT(!verify_chain(chain, store, "www.vulture.test"_sv, now).is_error());
}
