/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/CharacterTypes.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/SHA2.h>
#include <LibTLS/Certificate.h>

namespace TLS {

namespace {

constexpr size_t largest_chain_depth = 8;

// The algorithm of a signature, from its AlgorithmIdentifier.
ErrorOr<void> parse_signature_algorithm(ASN1::Element const& identifier, SignatureKind& kind, Crypto::SignatureHash& hash)
{
    ASN1::Reader reader(identifier.contents);
    auto oid = TRY(reader.read_expecting(ASN1::tag_object));
    kind = SignatureKind::Unsupported;
    auto is = [&](StringView dotted) { return ASN1::object_is(oid.contents, dotted); };
    if (is("1.2.840.113549.1.1.11"_sv)) {
        kind = SignatureKind::RSA_PKCS1;
        hash = Crypto::SignatureHash::SHA256;
    } else if (is("1.2.840.113549.1.1.12"_sv)) {
        kind = SignatureKind::RSA_PKCS1;
        hash = Crypto::SignatureHash::SHA384;
    } else if (is("1.2.840.113549.1.1.13"_sv)) {
        kind = SignatureKind::RSA_PKCS1;
        hash = Crypto::SignatureHash::SHA512;
    } else if (is("1.2.840.10045.4.3.2"_sv)) {
        kind = SignatureKind::ECDSA;
        hash = Crypto::SignatureHash::SHA256;
    } else if (is("1.2.840.10045.4.3.3"_sv)) {
        kind = SignatureKind::ECDSA;
        hash = Crypto::SignatureHash::SHA384;
    } else if (is("1.2.840.10045.4.3.4"_sv)) {
        kind = SignatureKind::ECDSA;
        hash = Crypto::SignatureHash::SHA512;
    } else if (is("1.3.101.112"_sv)) {
        kind = SignatureKind::Ed25519;
    } else if (is("1.2.840.113549.1.1.10"_sv)) {
        // RSASSA-PSS: which hash is in its parameters.
        auto parameters = TRY(reader.read_expecting(ASN1::tag_sequence));
        ASN1::Reader inner(parameters.contents);
        if (inner.next_tag() == ASN1::context_tag(0, true)) {
            auto wrapped = TRY(inner.read());
            ASN1::Reader wrapped_reader(wrapped.contents);
            auto algorithm = TRY(wrapped_reader.read_expecting(ASN1::tag_sequence));
            ASN1::Reader algorithm_reader(algorithm.contents);
            auto hash_oid = TRY(algorithm_reader.read_expecting(ASN1::tag_object));
            kind = SignatureKind::RSA_PSS;
            if (ASN1::object_is(hash_oid.contents, "2.16.840.1.101.3.4.2.1"_sv))
                hash = Crypto::SignatureHash::SHA256;
            else if (ASN1::object_is(hash_oid.contents, "2.16.840.1.101.3.4.2.2"_sv))
                hash = Crypto::SignatureHash::SHA384;
            else if (ASN1::object_is(hash_oid.contents, "2.16.840.1.101.3.4.2.3"_sv))
                hash = Crypto::SignatureHash::SHA512;
            else
                kind = SignatureKind::Unsupported;
        }
    }
    return {};
}

ErrorOr<void> parse_public_key(ASN1::Element const& info, PublicKey& key)
{
    ASN1::Reader reader(info.contents);
    auto algorithm = TRY(reader.read_expecting(ASN1::tag_sequence));
    auto bits = TRY(reader.read_expecting(ASN1::tag_bit_string));
    if (bits.contents.is_empty() || bits.contents[0] != 0)
        return Error::from_string_literal("certificate: a key with stray bits");
    ReadonlyBytes const material = bits.contents.slice(1);

    ASN1::Reader algorithm_reader(algorithm.contents);
    auto oid = TRY(algorithm_reader.read_expecting(ASN1::tag_object));
    if (ASN1::object_is(oid.contents, "1.2.840.113549.1.1.1"_sv)) {
        key.kind = KeyKind::RSA;
        ASN1::Reader outer(material);
        auto sequence = TRY(outer.read_expecting(ASN1::tag_sequence));
        ASN1::Reader numbers(sequence.contents);
        auto modulus = TRY(numbers.read_expecting(ASN1::tag_integer));
        auto exponent = TRY(numbers.read_expecting(ASN1::tag_integer));
        key.rsa.modulus = Crypto::BigInteger::from_bytes(modulus.contents);
        key.rsa.exponent = Crypto::BigInteger::from_bytes(exponent.contents);
        return {};
    }
    if (ASN1::object_is(oid.contents, "1.2.840.10045.2.1"_sv)) {
        key.kind = KeyKind::ECDSA;
        auto curve = TRY(algorithm_reader.read_expecting(ASN1::tag_object));
        if (ASN1::object_is(curve.contents, "1.2.840.10045.3.1.7"_sv))
            key.curve = Crypto::NamedCurve::P256;
        else if (ASN1::object_is(curve.contents, "1.3.132.0.34"_sv))
            key.curve = Crypto::NamedCurve::P384;
        else
            return Error::from_string_literal("certificate: a key on a curve we don't know");
        key.point = material;
        return {};
    }
    if (ASN1::object_is(oid.contents, "1.3.101.112"_sv)) {
        if (material.size() != Crypto::ed25519_public_key_size)
            return Error::from_string_literal("certificate: bad Ed25519 key");
        key.kind = KeyKind::Ed25519;
        key.point = material;
        return {};
    }
    return Error::from_string_literal("certificate: a kind of key we don't know");
}

ErrorOr<void> parse_extensions(ReadonlyBytes bytes, Certificate& certificate)
{
    ASN1::Reader list(bytes);
    while (!list.at_end()) {
        auto extension = TRY(list.read_expecting(ASN1::tag_sequence));
        ASN1::Reader reader(extension.contents);
        auto oid = TRY(reader.read_expecting(ASN1::tag_object));
        bool critical = false;
        if (reader.next_tag() == ASN1::tag_boolean) {
            auto flag = TRY(reader.read());
            critical = !flag.contents.is_empty() && flag.contents[0] != 0;
        }
        auto value = TRY(reader.read_expecting(ASN1::tag_octet_string));
        ASN1::Reader inner(value.contents);

        if (ASN1::object_is(oid.contents, "2.5.29.19"_sv)) {
            // basicConstraints.
            auto sequence = TRY(inner.read_expecting(ASN1::tag_sequence));
            ASN1::Reader constraints(sequence.contents);
            if (constraints.next_tag() == ASN1::tag_boolean) {
                auto flag = TRY(constraints.read());
                certificate.is_authority = !flag.contents.is_empty() && flag.contents[0] != 0;
            }
            if (constraints.next_tag() == ASN1::tag_integer) {
                auto length = TRY(constraints.read());
                if (length.contents.size() == 1)
                    certificate.path_length = length.contents[0];
            }
        } else if (ASN1::object_is(oid.contents, "2.5.29.15"_sv)) {
            // keyUsage: bits from the top, 0 digitalSignature and 5 keyCertSign.
            auto usage = TRY(inner.read_expecting(ASN1::tag_bit_string));
            certificate.has_key_usage = true;
            if (usage.contents.size() >= 2) {
                certificate.may_sign_handshakes = usage.contents[1] & 0x80;
                certificate.may_sign_certificates = usage.contents[1] & 0x04;
            }
        } else if (ASN1::object_is(oid.contents, "2.5.29.37"_sv)) {
            // extKeyUsage.
            auto sequence = TRY(inner.read_expecting(ASN1::tag_sequence));
            ASN1::Reader usages(sequence.contents);
            certificate.has_extended_usage = true;
            while (!usages.at_end()) {
                auto usage = TRY(usages.read_expecting(ASN1::tag_object));
                if (ASN1::object_is(usage.contents, "1.3.6.1.5.5.7.3.1"_sv) || ASN1::object_is(usage.contents, "2.5.29.37.0"_sv))
                    certificate.for_server_authentication = true;
            }
        } else if (ASN1::object_is(oid.contents, "2.5.29.17"_sv)) {
            // subjectAltName.
            auto sequence = TRY(inner.read_expecting(ASN1::tag_sequence));
            ASN1::Reader names(sequence.contents);
            while (!names.at_end()) {
                auto name = TRY(names.read());
                if (name.tag == ASN1::context_tag(2, false)) {
                    StringView const text { reinterpret_cast<char const*>(name.contents.data()), name.contents.size() };
                    TRY(certificate.dns_names.try_append(TRY(String::from_view(text))));
                } else if (name.tag == ASN1::context_tag(7, false) && name.contents.size() == 4) {
                    u32 address = static_cast<u32>(name.contents[0]) << 24 | static_cast<u32>(name.contents[1]) << 16 | static_cast<u32>(name.contents[2]) << 8 | name.contents[3];
                    TRY(certificate.ip_addresses.try_append(address));
                }
            }
        } else if (ASN1::object_is(oid.contents, "2.5.29.35"_sv) || ASN1::object_is(oid.contents, "2.5.29.14"_sv)
            || ASN1::object_is(oid.contents, "2.5.29.32"_sv) || ASN1::object_is(oid.contents, "2.5.29.31"_sv)
            || ASN1::object_is(oid.contents, "1.3.6.1.5.5.7.1.1"_sv)) {
            // Key identifiers, policies, revocation lists and where to find more: no
            // decision here depends on them.
        } else if (critical) {
            certificate.has_unknown_critical_extension = true;
        }
    }
    return {};
}

// The common name in a Name (or else the first string in it there is).
String name_summary(ReadonlyBytes name)
{
    ASN1::Reader sets(name);
    String first;
    while (!sets.at_end()) {
        auto set = sets.read();
        if (set.is_error() || set.value().tag != ASN1::tag_set)
            break;
        ASN1::Reader attributes(set.value().contents);
        while (!attributes.at_end()) {
            auto attribute = attributes.read();
            if (attribute.is_error() || attribute.value().tag != ASN1::tag_sequence)
                break;
            ASN1::Reader parts(attribute.value().contents);
            auto oid = parts.read();
            auto value = parts.read();
            if (oid.is_error() || value.is_error())
                break;
            StringView const text { reinterpret_cast<char const*>(value.value().contents.data()), value.value().contents.size() };
            if (ASN1::object_is(oid.value().contents, "2.5.4.3"_sv))
                return MUST(String::from_view(text));
            if (first.is_empty())
                first = MUST(String::from_view(text));
        }
    }
    return first;
}

bool label_character_ok(char c)
{
    return is_ascii_alphanumeric(c) || c == '-' || c == '_' || c == '.' || c == '*';
}

bool equal_ignoring_case(StringView a, StringView b)
{
    if (a.length() != b.length())
        return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (to_ascii_lowercase(a[i]) != to_ascii_lowercase(b[i]))
            return false;
    }
    return true;
}

// As RFC 6125 has it: a wildcard only as the whole of the leftmost label, standing for exactly
// one label, and never with nothing to its right but one more label.
bool name_matches(StringView pattern, StringView host)
{
    for (size_t i = 0; i < pattern.length(); ++i) {
        if (!label_character_ok(pattern[i]))
            return false;
    }
    if (pattern.length() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        StringView const rest = pattern.substring_view(2);
        // Two labels at least on the right: "*.com" is nobody's.
        if (!rest.contains('.'))
            return false;
        auto const dot = host.find('.');
        if (!dot.has_value() || dot.value() == 0)
            return false;
        return equal_ignoring_case(rest, host.substring_view(dot.value() + 1));
    }
    if (pattern.contains('*'))
        return false;
    return equal_ignoring_case(pattern, host);
}

bool parse_dotted_address(StringView host, u32& address)
{
    u32 value = 0;
    int part = 0;
    u32 current = 0;
    int digits = 0;
    for (size_t i = 0; i <= host.length(); ++i) {
        if (i == host.length() || host[i] == '.') {
            if (digits == 0 || current > 255 || part > 3)
                return false;
            value = value << 8 | current;
            current = 0;
            digits = 0;
            ++part;
            continue;
        }
        if (host[i] < '0' || host[i] > '9' || ++digits > 3)
            return false;
        current = current * 10 + static_cast<u32>(host[i] - '0');
    }
    if (part != 4)
        return false;
    address = value;
    return true;
}

bool same_bytes(ReadonlyBytes a, ReadonlyBytes b)
{
    return a.size() == b.size() && __builtin_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool signed_by(Certificate const& certificate, Certificate const& issuer)
{
    if (!same_bytes(certificate.issuer, issuer.subject))
        return false;
    return verify_signature(issuer.key, certificate.signature_kind, certificate.signature_hash, certificate.to_be_signed, certificate.signature);
}

ErrorOr<void> check_authority(Certificate const& certificate, size_t below, i64 now)
{
    if (!certificate.is_authority)
        return Error::from_string_literal("a certificate in the chain is not a certificate authority's");
    if (certificate.has_key_usage && !certificate.may_sign_certificates)
        return Error::from_string_literal("a certificate in the chain may not sign certificates");
    if (certificate.path_length >= 0 && below > static_cast<size_t>(certificate.path_length) + 1)
        return Error::from_string_literal("the chain is longer than an authority in it allows");
    if (now < certificate.not_before)
        return Error::from_string_literal("a certificate in the chain is not valid yet");
    if (now > certificate.not_after)
        return Error::from_string_literal("a certificate in the chain has expired");
    if (certificate.has_unknown_critical_extension)
        return Error::from_string_literal("a certificate in the chain has a critical extension we don't understand");
    return {};
}

// From `current`, which sits `depth` steps above the server's certificate, up to a root.
ErrorOr<void> find_path(Certificate const& current, Vector<NonnullOwnPtr<Certificate>> const& chain, TrustStore const& store, size_t depth, i64 now)
{
    if (depth > largest_chain_depth)
        return Error::from_string_literal("the certificate chain is too long");
    // A root we trust, and that signed this: there it ends.
    bool any_name_match = false;
    Optional<Error> last_error;
    for (auto const& root : store.roots()) {
        if (!same_bytes(current.issuer, root->subject))
            continue;
        any_name_match = true;
        if (signed_by(current, *root)) {
            auto checked = check_authority(*root, depth + 1, now);
            if (!checked.is_error())
                return {};
            last_error = checked.release_error();
        }
    }
    // Or an intermediate the server sent along.
    for (auto const& candidate : chain) {
        if (candidate.ptr() == &current || !same_bytes(current.issuer, candidate->subject))
            continue;
        any_name_match = true;
        if (!signed_by(current, *candidate))
            continue;
        auto checked = check_authority(*candidate, depth + 1, now);
        if (checked.is_error()) {
            last_error = checked.release_error();
            continue;
        }
        auto above = find_path(*candidate, chain, store, depth + 1, now);
        if (!above.is_error())
            return {};
        last_error = above.release_error();
    }
    if (last_error.has_value())
        return last_error.release_value();
    if (any_name_match)
        return Error::from_string_literal("a signature in the certificate chain doesn't check out");
    return Error::from_string_literal("unable to get local issuer certificate");
}

}

ErrorOr<NonnullOwnPtr<Certificate>> Certificate::parse(ReadonlyBytes bytes)
{
    auto certificate = TRY(try_make<Certificate>());
    TRY(certificate->der.try_resize(bytes.size()));
    if (!bytes.is_empty())
        __builtin_memcpy(certificate->der.data(), bytes.data(), bytes.size());

    ASN1::Reader outer(certificate->der.span());
    auto whole = TRY(outer.read_expecting(ASN1::tag_sequence));
    if (!outer.at_end())
        return Error::from_string_literal("certificate: something after it");
    ASN1::Reader parts(whole.contents);
    auto tbs = TRY(parts.read_expecting(ASN1::tag_sequence));
    auto signature_algorithm = TRY(parts.read_expecting(ASN1::tag_sequence));
    auto signature_bits = TRY(parts.read_expecting(ASN1::tag_bit_string));
    if (signature_bits.contents.is_empty() || signature_bits.contents[0] != 0)
        return Error::from_string_literal("certificate: a signature with stray bits");

    certificate->to_be_signed = tbs.whole;
    certificate->signature = signature_bits.contents.slice(1);
    TRY(parse_signature_algorithm(signature_algorithm, certificate->signature_kind, certificate->signature_hash));

    ASN1::Reader fields(tbs.contents);
    // [0] version, if it isn't version 1.
    if (fields.next_tag() == ASN1::context_tag(0, true))
        (void)TRY(fields.read());
    (void)TRY(fields.read_expecting(ASN1::tag_integer)); // Serial number.
    auto inner_algorithm = TRY(fields.read_expecting(ASN1::tag_sequence));
    // The two must say the same, or the signature means something else than it seems.
    if (!same_bytes(inner_algorithm.whole, signature_algorithm.whole))
        return Error::from_string_literal("certificate: two different signature algorithms");
    auto issuer = TRY(fields.read_expecting(ASN1::tag_sequence));
    certificate->issuer = issuer.whole;
    auto validity = TRY(fields.read_expecting(ASN1::tag_sequence));
    ASN1::Reader validity_reader(validity.contents);
    certificate->not_before = TRY(ASN1::parse_time(TRY(validity_reader.read())));
    certificate->not_after = TRY(ASN1::parse_time(TRY(validity_reader.read())));
    auto subject = TRY(fields.read_expecting(ASN1::tag_sequence));
    certificate->subject = subject.whole;
    auto key_info = TRY(fields.read_expecting(ASN1::tag_sequence));
    TRY(parse_public_key(key_info, certificate->key));
    while (!fields.at_end()) {
        auto field = TRY(fields.read());
        if (field.tag == ASN1::context_tag(3, true)) {
            ASN1::Reader wrapper(field.contents);
            auto list = TRY(wrapper.read_expecting(ASN1::tag_sequence));
            TRY(parse_extensions(list.contents, *certificate));
        }
    }
    return certificate;
}

String Certificate::subject_summary() const
{
    return name_summary(subject.size() > 2 ? ASN1::Reader(subject).read().value().contents : ReadonlyBytes {});
}

String Certificate::issuer_summary() const
{
    return name_summary(issuer.size() > 2 ? ASN1::Reader(issuer).read().value().contents : ReadonlyBytes {});
}

bool verify_signature(PublicKey const& key, SignatureKind kind, Crypto::SignatureHash hash, ReadonlyBytes message, ReadonlyBytes signature)
{
    switch (kind) {
    case SignatureKind::RSA_PKCS1:
        return key.kind == KeyKind::RSA && Crypto::rsa_verify_pkcs1(key.rsa, hash, message, signature);
    case SignatureKind::RSA_PSS:
        return key.kind == KeyKind::RSA && Crypto::rsa_verify_pss(key.rsa, hash, message, signature);
    case SignatureKind::ECDSA: {
        if (key.kind != KeyKind::ECDSA)
            return false;
        // r and s, as two integers in a sequence.
        ASN1::Reader outer(signature);
        auto sequence = outer.read_expecting(ASN1::tag_sequence);
        if (sequence.is_error())
            return false;
        ASN1::Reader numbers(sequence.value().contents);
        auto r = numbers.read_expecting(ASN1::tag_integer);
        auto s = numbers.read_expecting(ASN1::tag_integer);
        if (r.is_error() || s.is_error())
            return false;
        u8 digest[64];
        size_t digest_size = 0;
        switch (hash) {
        case Crypto::SignatureHash::SHA256: {
            auto d = Crypto::SHA256::hash(message);
            digest_size = d.size();
            __builtin_memcpy(digest, d.data(), digest_size);
            break;
        }
        case Crypto::SignatureHash::SHA384: {
            auto d = Crypto::SHA384::hash(message);
            digest_size = d.size();
            __builtin_memcpy(digest, d.data(), digest_size);
            break;
        }
        case Crypto::SignatureHash::SHA512: {
            auto d = Crypto::SHA512::hash(message);
            digest_size = d.size();
            __builtin_memcpy(digest, d.data(), digest_size);
            break;
        }
        }
        return Crypto::ecdsa_verify(key.curve, key.point, { digest, digest_size }, r.value().contents, s.value().contents);
    }
    case SignatureKind::Ed25519:
        return key.kind == KeyKind::Ed25519 && signature.size() == Crypto::ed25519_signature_size
            && Crypto::ed25519_verify(key.point.data(), message, signature.data());
    case SignatureKind::Unsupported:
        return false;
    }
    return false;
}

void TrustStore::add_pem(ReadonlyBytes pem)
{
    constexpr StringView begin = "-----BEGIN CERTIFICATE-----"_sv;
    constexpr StringView end = "-----END CERTIFICATE-----"_sv;
    StringView const text { reinterpret_cast<char const*>(pem.data()), pem.size() };
    size_t at = 0;
    Vector<u8> der;
    for (;;) {
        auto const found = text.substring_view(at).find(begin);
        if (!found.has_value())
            return;
        size_t const body = at + found.value() + begin.length();
        auto const stop = text.substring_view(body).find(end);
        if (!stop.has_value())
            return;
        der.clear();
        u32 accumulator = 0;
        int bits = 0;
        for (size_t i = body; i < body + stop.value(); ++i) {
            char const c = text[i];
            int value;
            if (c >= 'A' && c <= 'Z')
                value = c - 'A';
            else if (c >= 'a' && c <= 'z')
                value = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                value = c - '0' + 52;
            else if (c == '+')
                value = 62;
            else if (c == '/')
                value = 63;
            else
                continue;
            accumulator = accumulator << 6 | static_cast<u32>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der.append(static_cast<u8>(accumulator >> bits));
            }
        }
        (void)add_der(der.span());
        at = body + stop.value() + end.length();
    }
}

ErrorOr<void> TrustStore::add_der(ReadonlyBytes der)
{
    auto certificate = TRY(Certificate::parse(der));
    TRY(m_roots.try_append(move(certificate)));
    return {};
}

bool certificate_matches_host(Certificate const& certificate, StringView host)
{
    u32 address;
    if (parse_dotted_address(host, address)) {
        for (auto known : certificate.ip_addresses) {
            if (known == address)
                return true;
        }
        return false;
    }
    // A name ending with a dot means the same without it.
    if (host.ends_with("."_sv))
        host = host.substring_view(0, host.length() - 1);
    for (auto const& name : certificate.dns_names) {
        if (name_matches(name.view(), host))
            return true;
    }
    return false;
}

ErrorOr<void> verify_chain(Vector<NonnullOwnPtr<Certificate>> const& chain, TrustStore const& store, StringView host, i64 now)
{
    if (chain.is_empty())
        return Error::from_string_literal("the server sent no certificate");
    auto const& leaf = *chain.first();
    if (now < leaf.not_before)
        return Error::from_string_literal("certificate is not yet valid");
    if (now > leaf.not_after)
        return Error::from_string_literal("certificate has expired");
    if (leaf.has_unknown_critical_extension)
        return Error::from_string_literal("the certificate has a critical extension we don't understand");
    if (leaf.has_key_usage && !leaf.may_sign_handshakes)
        return Error::from_string_literal("the certificate may not sign a handshake");
    if (leaf.has_extended_usage && !leaf.for_server_authentication)
        return Error::from_string_literal("the certificate is not for servers");
    if (!certificate_matches_host(leaf, host))
        return Error::from_string_literal("no alternative certificate subject name matches target host name");
    return find_path(leaf, chain, store, 0, now);
}

}
