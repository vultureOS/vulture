/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibCrypto/EllipticCurve.h>
#include <LibCrypto/RSA.h>
#include <LibTLS/ASN1.h>

// X.509 certificates (RFC 5280), and whether a chain of them says what it should.

namespace TLS {

enum class KeyKind : u8 {
    RSA,
    ECDSA,
    Ed25519,
};

enum class SignatureKind : u8 {
    RSA_PKCS1,
    RSA_PSS,
    ECDSA,
    Ed25519,
    Unsupported,
};

struct PublicKey {
    KeyKind kind { KeyKind::RSA };
    Crypto::RSAPublicKey rsa;
    Crypto::NamedCurve curve { Crypto::NamedCurve::P256 };
    ReadonlyBytes point; // ECDSA: 0x04, x, y. Ed25519: the 32 bytes.
};

struct Certificate {
    // Everything is a view of this: the DER of the whole certificate.
    Vector<u8> der;

    ReadonlyBytes to_be_signed;
    ReadonlyBytes issuer;
    ReadonlyBytes subject;
    i64 not_before { 0 };
    i64 not_after { 0 };
    PublicKey key;

    SignatureKind signature_kind { SignatureKind::Unsupported };
    Crypto::SignatureHash signature_hash { Crypto::SignatureHash::SHA256 };
    ReadonlyBytes signature;

    bool is_authority { false };
    int path_length { -1 };
    bool has_key_usage { false };
    bool may_sign_certificates { false };
    bool may_sign_handshakes { false };
    bool has_extended_usage { false };
    bool for_server_authentication { false };
    bool has_unknown_critical_extension { false };
    Vector<String> dns_names;
    Vector<u32> ip_addresses; // Host order.

    Certificate() = default;
    Certificate(Certificate const&) = delete;
    Certificate& operator=(Certificate const&) = delete;

    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Certificate>> parse(ReadonlyBytes bytes);

    // A name in the usual form, for showing: the common name, or else the first attribute.
    [[nodiscard]] String subject_summary() const;
    [[nodiscard]] String issuer_summary() const;
};

// Does `signature` over `message` check out with `key`, by the given algorithm?
[[nodiscard]] bool verify_signature(PublicKey const&, SignatureKind, Crypto::SignatureHash, ReadonlyBytes message, ReadonlyBytes signature);

class TrustStore {
public:
    // A file of "-----BEGIN CERTIFICATE-----" blocks. Any that can't be understood are skipped.
    void add_pem(ReadonlyBytes);
    ErrorOr<void> add_der(ReadonlyBytes);
    [[nodiscard]] size_t size() const { return m_roots.size(); }
    [[nodiscard]] Vector<NonnullOwnPtr<Certificate>> const& roots() const { return m_roots; }

private:
    Vector<NonnullOwnPtr<Certificate>> m_roots;
};

// Whether the first of `chain` (as a server sent them: its own first) is good for `host`
// (a name or a dotted address) at time `now`, by a path to one of the store's roots.
ErrorOr<void> verify_chain(Vector<NonnullOwnPtr<Certificate>> const& chain, TrustStore const&, StringView host, i64 now);
[[nodiscard]] bool certificate_matches_host(Certificate const&, StringView host);

}
