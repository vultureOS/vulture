/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/AES.h>
#include <LibCrypto/ChaCha20Poly1305.h>
#include <LibCrypto/Curve25519.h>
#include <LibCrypto/EllipticCurve.h>
#include <LibCrypto/GCM.h>
#include <LibCrypto/HKDF.h>
#include <LibCrypto/HMAC.h>
#include <LibCrypto/SHA2.h>
#include <LibTLS/Connection.h>

namespace TLS {

namespace {

constexpr u8 record_change_cipher_spec = 20;
constexpr u8 record_alert = 21;
constexpr u8 record_handshake = 22;
constexpr u8 record_application_data = 23;

constexpr u8 handshake_client_hello = 1;
constexpr u8 handshake_server_hello = 2;
constexpr u8 handshake_new_session_ticket = 4;
constexpr u8 handshake_encrypted_extensions = 8;
constexpr u8 handshake_certificate = 11;
constexpr u8 handshake_certificate_request = 13;
constexpr u8 handshake_certificate_verify = 15;
constexpr u8 handshake_finished = 20;
constexpr u8 handshake_key_update = 24;
constexpr u8 handshake_message_hash = 254;

constexpr u16 suite_aes_128_gcm_sha256 = 0x1301;
constexpr u16 suite_aes_256_gcm_sha384 = 0x1302;
constexpr u16 suite_chacha20_poly1305_sha256 = 0x1303;

constexpr u16 group_secp256r1 = 0x0017;
constexpr u16 group_secp384r1 = 0x0018;
constexpr u16 group_x25519 = 0x001d;

constexpr u16 extension_server_name = 0;
constexpr u16 extension_supported_groups = 10;
constexpr u16 extension_signature_algorithms = 13;
constexpr u16 extension_alpn = 16;
constexpr u16 extension_supported_versions = 43;
constexpr u16 extension_cookie = 44;
constexpr u16 extension_key_share = 51;

constexpr u16 scheme_rsa_pkcs1_sha256 = 0x0401;
constexpr u16 scheme_rsa_pkcs1_sha384 = 0x0501;
constexpr u16 scheme_rsa_pkcs1_sha512 = 0x0601;
constexpr u16 scheme_ecdsa_secp256r1_sha256 = 0x0403;
constexpr u16 scheme_ecdsa_secp384r1_sha384 = 0x0503;
constexpr u16 scheme_rsa_pss_rsae_sha256 = 0x0804;
constexpr u16 scheme_rsa_pss_rsae_sha384 = 0x0805;
constexpr u16 scheme_rsa_pss_rsae_sha512 = 0x0806;
constexpr u16 scheme_ed25519 = 0x0807;
constexpr u16 scheme_rsa_pss_pss_sha256 = 0x0809;
constexpr u16 scheme_rsa_pss_pss_sha384 = 0x080a;
constexpr u16 scheme_rsa_pss_pss_sha512 = 0x080b;

constexpr u8 alert_close_notify = 0;
constexpr u8 alert_unexpected_message = 10;
constexpr u8 alert_bad_record_mac = 20;
constexpr u8 alert_handshake_failure = 40;
constexpr u8 alert_bad_certificate = 42;
constexpr u8 alert_decode_error = 50;
constexpr u8 alert_decrypt_error = 51;
constexpr u8 alert_protocol_version = 70;

constexpr size_t largest_plaintext = 16384;
constexpr size_t largest_ciphertext = largest_plaintext + 256;

// A ServerHello with this random is a HelloRetryRequest. (It is SHA-256 of that name.)
constexpr u8 retry_random[32] = { 0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

class Writer {
public:
    explicit Writer(Vector<u8>& out)
        : m_out(out)
    {
    }
    void u8_value(u8 value) { m_out.append(value); }
    void u16_value(u16 value)
    {
        m_out.append(static_cast<u8>(value >> 8));
        m_out.append(static_cast<u8>(value));
    }
    void u24_value(u32 value)
    {
        m_out.append(static_cast<u8>(value >> 16));
        m_out.append(static_cast<u8>(value >> 8));
        m_out.append(static_cast<u8>(value));
    }
    void bytes(ReadonlyBytes data)
    {
        for (size_t i = 0; i < data.size(); ++i)
            m_out.append(data[i]);
    }
    // Space for a length of this many bytes, filled in by end_length().
    size_t begin_length(size_t size)
    {
        size_t at = m_out.size();
        for (size_t i = 0; i < size; ++i)
            m_out.append(0);
        return at;
    }
    void end_length(size_t at, size_t size)
    {
        size_t const length = m_out.size() - at - size;
        for (size_t i = 0; i < size; ++i)
            m_out[at + i] = static_cast<u8>(length >> ((size - 1 - i) * 8));
    }

private:
    Vector<u8>& m_out;
};

class Parser {
public:
    explicit Parser(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }
    [[nodiscard]] bool at_end() const { return m_at == m_bytes.size(); }
    [[nodiscard]] size_t left() const { return m_bytes.size() - m_at; }
    ErrorOr<u8> u8_value()
    {
        if (left() < 1)
            return Error::from_string_literal("TLS: a message too short");
        return m_bytes[m_at++];
    }
    ErrorOr<u16> u16_value()
    {
        if (left() < 2)
            return Error::from_string_literal("TLS: a message too short");
        u16 value = static_cast<u16>(m_bytes[m_at] << 8 | m_bytes[m_at + 1]);
        m_at += 2;
        return value;
    }
    ErrorOr<u32> u24_value()
    {
        if (left() < 3)
            return Error::from_string_literal("TLS: a message too short");
        u32 value = static_cast<u32>(m_bytes[m_at]) << 16 | static_cast<u32>(m_bytes[m_at + 1]) << 8 | m_bytes[m_at + 2];
        m_at += 3;
        return value;
    }
    ErrorOr<ReadonlyBytes> bytes(size_t size)
    {
        if (left() < size)
            return Error::from_string_literal("TLS: a message too short");
        ReadonlyBytes out { m_bytes.data() + m_at, size };
        m_at += size;
        return out;
    }

private:
    ReadonlyBytes m_bytes;
    size_t m_at { 0 };
};

}

Connection::Connection(Transport& transport, Options options)
    : m_transport(transport)
    , m_options(move(options))
{
}

Connection::~Connection()
{
    __builtin_memset(m_x25519_private, 0, sizeof(m_x25519_private));
    __builtin_memset(m_ec_private, 0, sizeof(m_ec_private));
    __builtin_memset(m_handshake_secret, 0, sizeof(m_handshake_secret));
    __builtin_memset(m_master_secret, 0, sizeof(m_master_secret));
    __builtin_memset(m_client_handshake_secret, 0, sizeof(m_client_handshake_secret));
    __builtin_memset(m_server_handshake_secret, 0, sizeof(m_server_handshake_secret));
    __builtin_memset(m_read.secret, 0, sizeof(m_read.secret));
    __builtin_memset(m_write.secret, 0, sizeof(m_write.secret));
}

void Connection::progress(StringView text)
{
    if (m_options.on_progress)
        m_options.on_progress(text);
}

StringView Connection::cipher_suite_name() const
{
    switch (m_cipher_suite) {
    case suite_aes_128_gcm_sha256:
        return "AEAD-AES128-GCM-SHA256"_sv;
    case suite_aes_256_gcm_sha384:
        return "AEAD-AES256-GCM-SHA384"_sv;
    case suite_chacha20_poly1305_sha256:
        return "AEAD-CHACHA20-POLY1305-SHA256"_sv;
    default:
        return "?"_sv;
    }
}

StringView Connection::group_name() const
{
    switch (m_group) {
    case group_x25519:
        return "X25519"_sv;
    case group_secp256r1:
        return "P-256"_sv;
    case group_secp384r1:
        return "P-384"_sv;
    default:
        return "?"_sv;
    }
}

ErrorOr<NonnullOwnPtr<Connection>> Connection::handshake(Transport& transport, Options options)
{
    if (!options.random || (options.verify_certificate && !options.trust_store))
        return Error::from_string_literal("TLS: incomplete options");
    auto connection = adopt_own(*new (Base::nothrow) Connection(transport, move(options)));
    TRY(connection->run_handshake());
    return connection;
}

// ---- Hashing and keys

void Connection::add_to_transcript(ReadonlyBytes bytes)
{
    for (size_t i = 0; i < bytes.size(); ++i)
        m_transcript.append(bytes[i]);
}

void Connection::transcript_hash(u8* output) const
{
    if (m_hash == HashKind::SHA256) {
        auto digest = Crypto::SHA256::hash(m_transcript.span());
        __builtin_memcpy(output, digest.data(), digest.size());
    } else {
        auto digest = Crypto::SHA384::hash(m_transcript.span());
        __builtin_memcpy(output, digest.data(), digest.size());
    }
}

void Connection::hkdf_extract(ReadonlyBytes salt, ReadonlyBytes key, u8* output) const
{
    if (m_hash == HashKind::SHA256) {
        auto result = Crypto::hkdf_extract<Crypto::SHA256>(salt, key);
        __builtin_memcpy(output, result.data(), result.size());
    } else {
        auto result = Crypto::hkdf_extract<Crypto::SHA384>(salt, key);
        __builtin_memcpy(output, result.data(), result.size());
    }
}

void Connection::hkdf_expand_label(ReadonlyBytes secret, StringView label, ReadonlyBytes context, Bytes output) const
{
    Vector<u8> info;
    Writer writer(info);
    writer.u16_value(static_cast<u16>(output.size()));
    constexpr StringView prefix = "tls13 "_sv;
    writer.u8_value(static_cast<u8>(prefix.length() + label.length()));
    writer.bytes({ reinterpret_cast<u8 const*>(prefix.characters_without_null_termination()), prefix.length() });
    writer.bytes({ reinterpret_cast<u8 const*>(label.characters_without_null_termination()), label.length() });
    writer.u8_value(static_cast<u8>(context.size()));
    writer.bytes(context);
    if (m_hash == HashKind::SHA256)
        Crypto::hkdf_expand<Crypto::SHA256>(secret, info.span(), output);
    else
        Crypto::hkdf_expand<Crypto::SHA384>(secret, info.span(), output);
}

void Connection::derive_secret(ReadonlyBytes secret, StringView label, u8* output) const
{
    u8 hash[48];
    transcript_hash(hash);
    hkdf_expand_label(secret, label, { hash, hash_size() }, { output, hash_size() });
}

void Connection::set_traffic_keys(Direction& direction, ReadonlyBytes secret)
{
    __builtin_memcpy(direction.secret, secret.data(), secret.size());
    size_t const key_size = m_cipher_suite == suite_aes_128_gcm_sha256 ? 16 : 32;
    u8 key[32];
    hkdf_expand_label(secret, "key"_sv, {}, { key, key_size });
    hkdf_expand_label(secret, "iv"_sv, {}, { direction.iv, Crypto::aead_nonce_size });
    if (m_cipher_suite == suite_chacha20_poly1305_sha256)
        direction.cipher = adopt_own(*static_cast<Crypto::AEAD*>(new Crypto::ChaCha20Poly1305({ key, key_size })));
    else
        direction.cipher = adopt_own(*static_cast<Crypto::AEAD*>(new Crypto::AESGCM({ key, key_size })));
    direction.sequence = 0;
    __builtin_memset(key, 0, sizeof(key));
}

void Connection::compute_finished(ReadonlyBytes base_key, u8* output) const
{
    u8 finished_key[48];
    hkdf_expand_label(base_key, "finished"_sv, {}, { finished_key, hash_size() });
    u8 hash[48];
    transcript_hash(hash);
    if (m_hash == HashKind::SHA256) {
        Crypto::HMAC<Crypto::SHA256> hmac({ finished_key, 32 });
        hmac.update({ hash, 32 });
        auto digest = hmac.digest();
        __builtin_memcpy(output, digest.data(), 32);
    } else {
        Crypto::HMAC<Crypto::SHA384> hmac({ finished_key, 48 });
        hmac.update({ hash, 48 });
        auto digest = hmac.digest();
        __builtin_memcpy(output, digest.data(), 48);
    }
}

// ---- Records

ErrorOr<void> Connection::fill(size_t count)
{
    while (m_incoming.size() - m_incoming_start < count) {
        u8 chunk[16384 + 512];
        size_t got = TRY(m_transport.read({ chunk, sizeof(chunk) }));
        if (got == 0)
            return Error::from_string_literal("TLS: the connection ended without a word");
        if (m_incoming_start > 0 && m_incoming_start == m_incoming.size()) {
            m_incoming.clear();
            m_incoming_start = 0;
        }
        for (size_t i = 0; i < got; ++i)
            m_incoming.append(chunk[i]);
    }
    return {};
}

ErrorOr<void> Connection::read_record(u8& type, Vector<u8>& contents)
{
    TRY(fill(5));
    u8 const* header = m_incoming.data() + m_incoming_start;
    type = header[0];
    size_t const length = static_cast<size_t>(header[3]) << 8 | header[4];
    if (length > largest_ciphertext) {
        (void)send_alert(22);
        return Error::from_string_literal("TLS: a record too large");
    }
    u8 const version_major = header[1];
    if (version_major != 3)
        return Error::from_string_literal("TLS: this is not TLS");
    TRY(fill(5 + length));
    header = m_incoming.data() + m_incoming_start;
    ReadonlyBytes body { header + 5, length };
    u8 record_header[5];
    __builtin_memcpy(record_header, header, 5);

    contents.clear();
    bool const encrypted = m_read.cipher && type == record_application_data;
    if (encrypted) {
        if (length < Crypto::aead_tag_size + 1) {
            m_incoming_start += 5 + length;
            (void)send_alert(alert_bad_record_mac);
            return Error::from_string_literal("TLS: a record too short");
        }
        TRY(contents.try_resize(length - Crypto::aead_tag_size));
        u8 nonce[Crypto::aead_nonce_size];
        __builtin_memcpy(nonce, m_read.iv, sizeof(nonce));
        for (int i = 0; i < 8; ++i)
            nonce[sizeof(nonce) - 1 - static_cast<size_t>(i)] ^= static_cast<u8>(m_read.sequence >> (i * 8));
        size_t const ciphertext_size = length - Crypto::aead_tag_size;
        bool const opened = m_read.cipher->open(nonce, { record_header, 5 }, body.slice(0, ciphertext_size), body.data() + ciphertext_size, contents.data());
        m_incoming_start += 5 + length;
        if (!opened) {
            (void)send_alert(alert_bad_record_mac);
            return Error::from_string_literal("TLS: a record failed its integrity check");
        }
        ++m_read.sequence;
        // The real type is the last byte that isn't padding.
        size_t size = contents.size();
        while (size > 0 && contents[size - 1] == 0)
            --size;
        if (size == 0) {
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: a record of nothing but padding");
        }
        type = contents[size - 1];
        TRY(contents.try_resize(size - 1));
    } else {
        TRY(contents.try_resize(length));
        if (length > 0)
            __builtin_memcpy(contents.data(), body.data(), length);
        m_incoming_start += 5 + length;
    }
    if (m_incoming_start > 256 * 1024) {
        // What's been read needn't stay.
        Vector<u8> rest;
        TRY(rest.try_resize(m_incoming.size() - m_incoming_start));
        if (!rest.is_empty())
            __builtin_memcpy(rest.data(), m_incoming.data() + m_incoming_start, rest.size());
        m_incoming = move(rest);
        m_incoming_start = 0;
    }

    if (type == record_alert) {
        if (contents.size() < 2)
            return Error::from_string_literal("TLS: a bad alert");
        if (contents[1] == alert_close_notify) {
            m_closed_by_peer = true;
            contents.clear();
            return {};
        }
        switch (contents[1]) {
        case alert_handshake_failure:
            return Error::from_string_literal("TLS: the server's alert: handshake failure");
        case alert_protocol_version:
            return Error::from_string_literal("TLS: the server won't speak TLS 1.3");
        case alert_bad_certificate:
            return Error::from_string_literal("TLS: the server's alert: bad certificate");
        case alert_decrypt_error:
            return Error::from_string_literal("TLS: the server's alert: decrypt error");
        default:
            return Error::from_string_literal("TLS: the server sent an alert");
        }
    }
    return {};
}

ErrorOr<void> Connection::send_record(u8 type, ReadonlyBytes data)
{
    Vector<u8> record;
    Writer writer(record);
    writer.u8_value(type);
    writer.u16_value(type == record_handshake ? 0x0301 : 0x0303);
    writer.u16_value(static_cast<u16>(data.size()));
    writer.bytes(data);
    return m_transport.write(record.span());
}

ErrorOr<void> Connection::send_encrypted(u8 inner_type, ReadonlyBytes data)
{
    size_t at = 0;
    do {
        size_t const piece = min(data.size() - at, largest_plaintext);
        size_t const inner_size = piece + 1;
        size_t const total = inner_size + Crypto::aead_tag_size;
        Vector<u8> record;
        TRY(record.try_resize(5 + total));
        record[0] = record_application_data;
        record[1] = 3;
        record[2] = 3;
        record[3] = static_cast<u8>(total >> 8);
        record[4] = static_cast<u8>(total);
        if (piece > 0)
            __builtin_memcpy(record.data() + 5, data.data() + at, piece);
        record[5 + piece] = inner_type;
        u8 nonce[Crypto::aead_nonce_size];
        __builtin_memcpy(nonce, m_write.iv, sizeof(nonce));
        for (int i = 0; i < 8; ++i)
            nonce[sizeof(nonce) - 1 - static_cast<size_t>(i)] ^= static_cast<u8>(m_write.sequence >> (i * 8));
        m_write.cipher->seal(nonce, { record.data(), 5 }, { record.data() + 5, inner_size }, record.data() + 5, record.data() + 5 + inner_size);
        ++m_write.sequence;
        TRY(m_transport.write(record.span()));
        at += piece;
    } while (at < data.size());
    return {};
}

ErrorOr<void> Connection::send_alert(u8 description)
{
    u8 const body[2] = { static_cast<u8>(description == alert_close_notify ? 1 : 2), description };
    if (m_write.cipher)
        return send_encrypted(record_alert, { body, 2 });
    return send_record(record_alert, { body, 2 });
}

ErrorOr<void> Connection::read_handshake_message(u8& type, Vector<u8>& body)
{
    for (;;) {
        size_t const available = m_handshake_bytes.size() - m_handshake_start;
        if (available >= 4) {
            u8 const* header = m_handshake_bytes.data() + m_handshake_start;
            size_t const length = static_cast<size_t>(header[1]) << 16 | static_cast<size_t>(header[2]) << 8 | header[3];
            // A certificate chain is large; anything near this is not one.
            if (length > 512 * 1024) {
                (void)send_alert(alert_decode_error);
                return Error::from_string_literal("TLS: a handshake message too large");
            }
            if (available >= 4 + length) {
                type = header[0];
                TRY(body.try_resize(length));
                if (length > 0)
                    __builtin_memcpy(body.data(), header + 4, length);
                m_handshake_start += 4 + length;
                if (m_handshake_start == m_handshake_bytes.size()) {
                    m_handshake_bytes.clear();
                    m_handshake_start = 0;
                }
                return {};
            }
        }
        u8 record_type;
        Vector<u8> contents;
        TRY(read_record(record_type, contents));
        if (m_closed_by_peer)
            return Error::from_string_literal("TLS: the server closed during the handshake");
        if (record_type == record_change_cipher_spec)
            continue;
        if (record_type != record_handshake) {
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: something else in the middle of the handshake");
        }
        for (size_t i = 0; i < contents.size(); ++i)
            m_handshake_bytes.append(contents[i]);
    }
}

// ---- The handshake

ErrorOr<void> Connection::send_client_hello(bool retry)
{
    Vector<u8> hello;
    Writer writer(hello);
    writer.u8_value(handshake_client_hello);
    size_t const message_length = writer.begin_length(3);
    writer.u16_value(0x0303);
    writer.bytes({ m_client_random, 32 });
    writer.u8_value(32);
    writer.bytes({ m_session_id, 32 });

    // The fastest first: AES only where the processor does it, as it is slow and leaky
    // without; ChaCha20 is good everywhere.
    size_t const suites = writer.begin_length(2);
    if (Crypto::AES::has_hardware()) {
        writer.u16_value(suite_aes_128_gcm_sha256);
        writer.u16_value(suite_aes_256_gcm_sha384);
        writer.u16_value(suite_chacha20_poly1305_sha256);
    } else {
        writer.u16_value(suite_chacha20_poly1305_sha256);
        writer.u16_value(suite_aes_128_gcm_sha256);
        writer.u16_value(suite_aes_256_gcm_sha384);
    }
    writer.end_length(suites, 2);
    writer.u8_value(1);
    writer.u8_value(0); // No compression.

    size_t const extensions = writer.begin_length(2);

    if (!m_options.host.is_empty()) {
        // Only names: an address doesn't go here.
        bool is_address = true;
        for (size_t i = 0; i < m_options.host.length(); ++i) {
            char const c = m_options.host[i];
            if ((c < '0' || c > '9') && c != '.')
                is_address = false;
        }
        if (!is_address) {
            writer.u16_value(extension_server_name);
            size_t const whole = writer.begin_length(2);
            size_t const list = writer.begin_length(2);
            writer.u8_value(0);
            writer.u16_value(static_cast<u16>(m_options.host.length()));
            writer.bytes({ reinterpret_cast<u8 const*>(m_options.host.characters_without_null_termination()), m_options.host.length() });
            writer.end_length(list, 2);
            writer.end_length(whole, 2);
        }
    }

    writer.u16_value(extension_supported_versions);
    writer.u16_value(3);
    writer.u8_value(2);
    writer.u16_value(0x0304);

    writer.u16_value(extension_supported_groups);
    writer.u16_value(8);
    writer.u16_value(6);
    writer.u16_value(group_x25519);
    writer.u16_value(group_secp256r1);
    writer.u16_value(group_secp384r1);

    writer.u16_value(extension_signature_algorithms);
    {
        static constexpr u16 schemes[] = { scheme_ecdsa_secp256r1_sha256, scheme_ecdsa_secp384r1_sha384, scheme_ed25519, scheme_rsa_pss_rsae_sha256,
            scheme_rsa_pss_rsae_sha384, scheme_rsa_pss_rsae_sha512, scheme_rsa_pss_pss_sha256, scheme_rsa_pss_pss_sha384, scheme_rsa_pss_pss_sha512,
            scheme_rsa_pkcs1_sha256, scheme_rsa_pkcs1_sha384, scheme_rsa_pkcs1_sha512 };
        size_t const whole = writer.begin_length(2);
        size_t const list = writer.begin_length(2);
        for (u16 scheme : schemes)
            writer.u16_value(scheme);
        writer.end_length(list, 2);
        writer.end_length(whole, 2);
    }

    writer.u16_value(extension_key_share);
    {
        size_t const whole = writer.begin_length(2);
        size_t const list = writer.begin_length(2);
        u16 const group = retry ? m_retry_group : static_cast<u16>(group_x25519);
        m_group = group;
        writer.u16_value(group);
        if (group == group_x25519) {
            writer.u16_value(Crypto::x25519_key_size);
            m_options.random({ m_x25519_private, sizeof(m_x25519_private) });
            u8 public_key[32];
            Crypto::x25519_base(m_x25519_private, public_key);
            writer.bytes({ public_key, 32 });
        } else {
            auto const curve = group == group_secp256r1 ? Crypto::NamedCurve::P256 : Crypto::NamedCurve::P384;
            size_t const size = Crypto::curve_coordinate_size(curve);
            u8 public_key[97];
            // A number below the order: nearly all of them are, so try until one is.
            for (;;) {
                m_options.random({ m_ec_private, size });
                if (Crypto::ec_public_key(curve, { m_ec_private, size }, { public_key, size * 2 + 1 }))
                    break;
            }
            writer.u16_value(static_cast<u16>(size * 2 + 1));
            writer.bytes({ public_key, size * 2 + 1 });
        }
        writer.end_length(list, 2);
        writer.end_length(whole, 2);
    }

    if (retry && !m_retry_cookie.is_empty()) {
        writer.u16_value(extension_cookie);
        size_t const whole = writer.begin_length(2);
        size_t const list = writer.begin_length(2);
        writer.bytes(m_retry_cookie.span());
        writer.end_length(list, 2);
        writer.end_length(whole, 2);
    }

    if (!m_options.application_protocol.is_empty()) {
        writer.u16_value(extension_alpn);
        size_t const whole = writer.begin_length(2);
        size_t const list = writer.begin_length(2);
        writer.u8_value(static_cast<u8>(m_options.application_protocol.length()));
        writer.bytes({ reinterpret_cast<u8 const*>(m_options.application_protocol.characters_without_null_termination()), m_options.application_protocol.length() });
        writer.end_length(list, 2);
        writer.end_length(whole, 2);
    }

    writer.end_length(extensions, 2);
    writer.end_length(message_length, 3);

    add_to_transcript(hello.span());
    return send_record(record_handshake, hello.span());
}

ErrorOr<void> Connection::process_server_hello(ReadonlyBytes body, bool& is_retry)
{
    Parser parser(body);
    u16 const legacy_version = TRY(parser.u16_value());
    auto const random = TRY(parser.bytes(32));
    u8 const session_length = TRY(parser.u8_value());
    auto const session = TRY(parser.bytes(session_length));
    u16 const suite = TRY(parser.u16_value());
    u8 const compression = TRY(parser.u8_value());
    u16 const extensions_length = TRY(parser.u16_value());
    Parser extensions(TRY(parser.bytes(extensions_length)));

    is_retry = __builtin_memcmp(random.data(), retry_random, 32) == 0;

    if (legacy_version != 0x0303 || compression != 0) {
        (void)send_alert(alert_protocol_version);
        return Error::from_string_literal("TLS: the server won't speak TLS 1.3");
    }
    if (session.size() != 32 || __builtin_memcmp(session.data(), m_session_id, 32) != 0) {
        (void)send_alert(47);
        return Error::from_string_literal("TLS: the server didn't echo the session id");
    }
    if (suite != suite_aes_128_gcm_sha256 && suite != suite_aes_256_gcm_sha384 && suite != suite_chacha20_poly1305_sha256) {
        (void)send_alert(47);
        return Error::from_string_literal("TLS: the server chose a cipher nobody offered");
    }
    m_cipher_suite = suite;
    m_hash = suite == suite_aes_256_gcm_sha384 ? HashKind::SHA384 : HashKind::SHA256;

    bool saw_version = false;
    ReadonlyBytes key_exchange;
    u16 share_group = 0;
    while (!extensions.at_end()) {
        u16 const type = TRY(extensions.u16_value());
        u16 const length = TRY(extensions.u16_value());
        Parser extension(TRY(extensions.bytes(length)));
        switch (type) {
        case extension_supported_versions:
            if (TRY(extension.u16_value()) == 0x0304)
                saw_version = true;
            break;
        case extension_key_share:
            share_group = TRY(extension.u16_value());
            if (!is_retry) {
                u16 const size = TRY(extension.u16_value());
                key_exchange = TRY(extension.bytes(size));
            }
            break;
        case extension_cookie: {
            u16 const size = TRY(extension.u16_value());
            auto const cookie = TRY(extension.bytes(size));
            m_retry_cookie.clear();
            for (size_t i = 0; i < cookie.size(); ++i)
                m_retry_cookie.append(cookie[i]);
            break;
        }
        default:
            break;
        }
    }
    if (!saw_version) {
        (void)send_alert(alert_protocol_version);
        return Error::from_string_literal("TLS: the server won't speak TLS 1.3");
    }

    if (is_retry) {
        if (share_group == 0 && m_retry_cookie.is_empty()) {
            (void)send_alert(47);
            return Error::from_string_literal("TLS: a retry request that changes nothing");
        }
        if (share_group != 0) {
            if (share_group != group_x25519 && share_group != group_secp256r1 && share_group != group_secp384r1) {
                (void)send_alert(47);
                return Error::from_string_literal("TLS: a retry request for a group nobody offered");
            }
            m_retry_group = share_group;
        } else {
            m_retry_group = m_group;
        }
        return {};
    }

    if (share_group != m_group) {
        (void)send_alert(47);
        return Error::from_string_literal("TLS: the server's key share is for the wrong group");
    }
    if (share_group == group_x25519) {
        if (key_exchange.size() != 32) {
            (void)send_alert(alert_decode_error);
            return Error::from_string_literal("TLS: a bad key share");
        }
        TRY(m_shared_secret.try_resize(32));
        Crypto::x25519(m_x25519_private, key_exchange.data(), m_shared_secret.data());
        // All zeroes means a point of small order: no secret at all.
        u8 any = 0;
        for (size_t i = 0; i < 32; ++i)
            any |= m_shared_secret[i];
        if (any == 0) {
            (void)send_alert(47);
            return Error::from_string_literal("TLS: a bad key share");
        }
    } else {
        auto const curve = share_group == group_secp256r1 ? Crypto::NamedCurve::P256 : Crypto::NamedCurve::P384;
        size_t const size = Crypto::curve_coordinate_size(curve);
        TRY(m_shared_secret.try_resize(size));
        if (!Crypto::ecdh(curve, { m_ec_private, size }, key_exchange, m_shared_secret.span())) {
            (void)send_alert(47);
            return Error::from_string_literal("TLS: a bad key share");
        }
    }
    return {};
}

ErrorOr<void> Connection::process_certificate(ReadonlyBytes body)
{
    Parser parser(body);
    u8 const context_size = TRY(parser.u8_value());
    (void)TRY(parser.bytes(context_size));
    u32 const list_size = TRY(parser.u24_value());
    Parser list(TRY(parser.bytes(list_size)));
    while (!list.at_end()) {
        u32 const size = TRY(list.u24_value());
        auto const der = TRY(list.bytes(size));
        u16 const extensions = TRY(list.u16_value());
        (void)TRY(list.bytes(extensions));
        auto parsed = Certificate::parse(der);
        if (parsed.is_error()) {
            (void)send_alert(alert_bad_certificate);
            return parsed.release_error();
        }
        TRY(m_chain.try_append(parsed.release_value()));
    }
    if (m_chain.is_empty()) {
        (void)send_alert(alert_decode_error);
        return Error::from_string_literal("TLS: the server sent no certificate");
    }
    m_peer_subject = m_chain.first()->subject_summary();
    m_peer_issuer = m_chain.first()->issuer_summary();
    if (m_options.verify_certificate) {
        auto verified = verify_chain(m_chain, *m_options.trust_store, m_options.host, m_options.now);
        if (verified.is_error()) {
            (void)send_alert(alert_bad_certificate);
            return verified.release_error();
        }
    }
    return {};
}

ErrorOr<void> Connection::process_certificate_verify(ReadonlyBytes body)
{
    Parser parser(body);
    u16 const scheme = TRY(parser.u16_value());
    u16 const size = TRY(parser.u16_value());
    auto const signature = TRY(parser.bytes(size));

    SignatureKind kind;
    Crypto::SignatureHash hash = Crypto::SignatureHash::SHA256;
    switch (scheme) {
    case scheme_ecdsa_secp256r1_sha256:
        kind = SignatureKind::ECDSA;
        hash = Crypto::SignatureHash::SHA256;
        break;
    case scheme_ecdsa_secp384r1_sha384:
        kind = SignatureKind::ECDSA;
        hash = Crypto::SignatureHash::SHA384;
        break;
    case scheme_ed25519:
        kind = SignatureKind::Ed25519;
        break;
    case scheme_rsa_pss_rsae_sha256:
    case scheme_rsa_pss_pss_sha256:
        kind = SignatureKind::RSA_PSS;
        hash = Crypto::SignatureHash::SHA256;
        break;
    case scheme_rsa_pss_rsae_sha384:
    case scheme_rsa_pss_pss_sha384:
        kind = SignatureKind::RSA_PSS;
        hash = Crypto::SignatureHash::SHA384;
        break;
    case scheme_rsa_pss_rsae_sha512:
    case scheme_rsa_pss_pss_sha512:
        kind = SignatureKind::RSA_PSS;
        hash = Crypto::SignatureHash::SHA512;
        break;
    default:
        // PKCS#1 v1.5 is for certificates: never, in TLS 1.3, for this.
        (void)send_alert(47);
        return Error::from_string_literal("TLS: the server signed the handshake with something it shouldn't");
    }

    // 64 spaces, the context, a zero, and the transcript's hash up to the certificate.
    Vector<u8> signed_content;
    for (int i = 0; i < 64; ++i)
        signed_content.append(0x20);
    constexpr StringView context = "TLS 1.3, server CertificateVerify"_sv;
    for (size_t i = 0; i < context.length(); ++i)
        signed_content.append(static_cast<u8>(context[i]));
    signed_content.append(0);
    u8 transcript[48];
    transcript_hash(transcript);
    for (size_t i = 0; i < hash_size(); ++i)
        signed_content.append(transcript[i]);

    if (!verify_signature(m_chain.first()->key, kind, hash, signed_content.span(), signature)) {
        (void)send_alert(alert_decrypt_error);
        return Error::from_string_literal("TLS: the server's signature of the handshake doesn't check out");
    }
    return {};
}

ErrorOr<void> Connection::process_finished(ReadonlyBytes body)
{
    u8 expected[48];
    compute_finished({ m_server_handshake_secret, hash_size() }, expected);
    if (!Crypto::constant_time_equal({ expected, hash_size() }, body)) {
        (void)send_alert(alert_decrypt_error);
        return Error::from_string_literal("TLS: the server's Finished doesn't check out");
    }
    return {};
}

ErrorOr<void> Connection::process_encrypted_handshake()
{
    enum class Expect {
        EncryptedExtensions,
        CertificateOrRequest,
        Certificate,
        CertificateVerify,
        Finished,
    } expect = Expect::EncryptedExtensions;
    bool asked_for_certificate = false;

    for (;;) {
        u8 type;
        Vector<u8> body;
        TRY(read_handshake_message(type, body));

        // CertificateVerify and Finished cover everything before them, so these are checked
        // before they go in the transcript.
        switch (expect) {
        case Expect::EncryptedExtensions: {
            if (type != handshake_encrypted_extensions)
                goto unexpected;
            Parser parser(body.span());
            u16 const length = TRY(parser.u16_value());
            Parser extensions(TRY(parser.bytes(length)));
            while (!extensions.at_end()) {
                u16 const extension_type = TRY(extensions.u16_value());
                u16 const extension_length = TRY(extensions.u16_value());
                Parser extension(TRY(extensions.bytes(extension_length)));
                if (extension_type == extension_alpn) {
                    (void)TRY(extension.u16_value());
                    u8 const name_length = TRY(extension.u8_value());
                    auto const name = TRY(extension.bytes(name_length));
                    m_alpn = TRY(String::from_view({ reinterpret_cast<char const*>(name.data()), name.size() }));
                }
            }
            expect = Expect::CertificateOrRequest;
            break;
        }
        case Expect::CertificateOrRequest:
            if (type == handshake_certificate_request) {
                asked_for_certificate = true;
                expect = Expect::Certificate;
                break;
            }
            [[fallthrough]];
        case Expect::Certificate:
            if (type != handshake_certificate)
                goto unexpected;
            TRY(process_certificate(body.span()));
            expect = Expect::CertificateVerify;
            break;
        case Expect::CertificateVerify:
            if (type != handshake_certificate_verify)
                goto unexpected;
            TRY(process_certificate_verify(body.span()));
            expect = Expect::Finished;
            break;
        case Expect::Finished: {
            if (type != handshake_finished)
                goto unexpected;
            TRY(process_finished(body.span()));
            // Add the message itself, and it's over from their side.
            u8 header[4] = { type, static_cast<u8>(body.size() >> 16), static_cast<u8>(body.size() >> 8), static_cast<u8>(body.size()) };
            add_to_transcript({ header, 4 });
            add_to_transcript(body.span());

            // Keys for the traffic to come are derived from everything up to here.
            u8 derived[48];
            u8 empty_hash[48];
            if (m_hash == HashKind::SHA256) {
                auto digest = Crypto::SHA256::hash({});
                __builtin_memcpy(empty_hash, digest.data(), 32);
            } else {
                auto digest = Crypto::SHA384::hash({});
                __builtin_memcpy(empty_hash, digest.data(), 48);
            }
            hkdf_expand_label({ m_handshake_secret, hash_size() }, "derived"_sv, { empty_hash, hash_size() }, { derived, hash_size() });
            u8 zeroes[48] = {};
            hkdf_extract({ derived, hash_size() }, { zeroes, hash_size() }, m_master_secret);
            u8 client_secret[48];
            u8 server_secret[48];
            derive_secret({ m_master_secret, hash_size() }, "c ap traffic"_sv, client_secret);
            derive_secret({ m_master_secret, hash_size() }, "s ap traffic"_sv, server_secret);

            // Our side: asked for a certificate, we have none. Then our Finished, under the
            // handshake's keys.
            if (asked_for_certificate) {
                u8 const empty_certificate[8] = { handshake_certificate, 0, 0, 4, 0, 0, 0, 0 };
                add_to_transcript({ empty_certificate, 8 });
                TRY(send_encrypted(record_handshake, { empty_certificate, 8 }));
            }
            u8 finished[48];
            compute_finished({ m_client_handshake_secret, hash_size() }, finished);
            Vector<u8> message;
            Writer writer(message);
            writer.u8_value(handshake_finished);
            writer.u24_value(static_cast<u32>(hash_size()));
            writer.bytes({ finished, hash_size() });
            add_to_transcript(message.span());
            TRY(send_encrypted(record_handshake, message.span()));

            set_traffic_keys(m_read, { server_secret, hash_size() });
            set_traffic_keys(m_write, { client_secret, hash_size() });
            return {};
        }
        }
        {
            u8 header[4] = { type, static_cast<u8>(body.size() >> 16), static_cast<u8>(body.size() >> 8), static_cast<u8>(body.size()) };
            add_to_transcript({ header, 4 });
            add_to_transcript(body.span());
        }
        continue;
    unexpected:
        (void)send_alert(alert_unexpected_message);
        return Error::from_string_literal("TLS: the handshake's messages came in the wrong order");
    }
}

ErrorOr<void> Connection::run_handshake()
{
    m_options.random({ m_client_random, sizeof(m_client_random) });
    m_options.random({ m_session_id, sizeof(m_session_id) });

    TRY(send_client_hello(false));
    progress("TLSv1.3 (OUT), TLS handshake, Client hello (1):"_sv);

    for (bool first = true;; first = false) {
        u8 type;
        Vector<u8> body;
        TRY(read_handshake_message(type, body));
        if (type != handshake_server_hello) {
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: expected the server's hello");
        }
        bool is_retry = false;
        TRY(process_server_hello(body.span(), is_retry));
        progress("TLSv1.3 (IN), TLS handshake, Server hello (2):"_sv);

        u8 header[4] = { type, static_cast<u8>(body.size() >> 16), static_cast<u8>(body.size() >> 8), static_cast<u8>(body.size()) };
        if (!is_retry) {
            add_to_transcript({ header, 4 });
            add_to_transcript(body.span());
            break;
        }
        if (!first) {
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: a second retry request");
        }
        // The first hello is replaced by its hash, in a message of its own.
        u8 first_hash[48];
        transcript_hash(first_hash);
        m_transcript.clear();
        u8 const synthetic[4] = { handshake_message_hash, 0, 0, static_cast<u8>(hash_size()) };
        add_to_transcript({ synthetic, 4 });
        add_to_transcript({ first_hash, hash_size() });
        add_to_transcript({ header, 4 });
        add_to_transcript(body.span());

        u8 const change_cipher[1] = { 1 };
        TRY(send_record(record_change_cipher_spec, { change_cipher, 1 }));
        m_sent_dummy_change_cipher = true;
        TRY(send_client_hello(true));
        progress("TLSv1.3 (OUT), TLS handshake, Client hello (1):"_sv);
    }

    // Early secret, from no pre-shared key, then the handshake secret.
    u8 zeroes[48] = {};
    u8 early[48];
    hkdf_extract({}, { zeroes, hash_size() }, early);
    u8 empty_hash[48];
    if (m_hash == HashKind::SHA256) {
        auto digest = Crypto::SHA256::hash({});
        __builtin_memcpy(empty_hash, digest.data(), 32);
    } else {
        auto digest = Crypto::SHA384::hash({});
        __builtin_memcpy(empty_hash, digest.data(), 48);
    }
    u8 derived[48];
    hkdf_expand_label({ early, hash_size() }, "derived"_sv, { empty_hash, hash_size() }, { derived, hash_size() });
    hkdf_extract({ derived, hash_size() }, m_shared_secret.span(), m_handshake_secret);
    derive_secret({ m_handshake_secret, hash_size() }, "c hs traffic"_sv, m_client_handshake_secret);
    derive_secret({ m_handshake_secret, hash_size() }, "s hs traffic"_sv, m_server_handshake_secret);
    set_traffic_keys(m_read, { m_server_handshake_secret, hash_size() });
    set_traffic_keys(m_write, { m_client_handshake_secret, hash_size() });

    // What middleboxes expect to see of TLS 1.2, and means nothing (RFC 8446, D.4).
    if (!m_sent_dummy_change_cipher) {
        u8 const change_cipher[1] = { 1 };
        TRY(send_record(record_change_cipher_spec, { change_cipher, 1 }));
    }

    TRY(process_encrypted_handshake());
    progress("TLSv1.3 (OUT), TLS handshake, Finished (20):"_sv);
    __builtin_memset(m_shared_secret.data(), 0, m_shared_secret.size());
    m_transcript.clear();
    return {};
}

// ---- After the handshake

ErrorOr<void> Connection::process_post_handshake(ReadonlyBytes contents)
{
    for (size_t i = 0; i < contents.size(); ++i)
        m_handshake_bytes.append(contents[i]);
    for (;;) {
        size_t const available = m_handshake_bytes.size() - m_handshake_start;
        if (available < 4)
            break;
        u8 const* header = m_handshake_bytes.data() + m_handshake_start;
        size_t const length = static_cast<size_t>(header[1]) << 16 | static_cast<size_t>(header[2]) << 8 | header[3];
        if (available < 4 + length)
            break;
        u8 const type = header[0];
        u8 const* body = header + 4;
        switch (type) {
        case handshake_new_session_ticket:
            // Nothing here resumes, so nothing keeps these.
            break;
        case handshake_key_update: {
            if (length != 1 || body[0] > 1) {
                (void)send_alert(alert_decode_error);
                return Error::from_string_literal("TLS: a bad key update");
            }
            bool const update_requested = body[0] == 1;
            u8 next[48];
            hkdf_expand_label({ m_read.secret, hash_size() }, "traffic upd"_sv, {}, { next, hash_size() });
            set_traffic_keys(m_read, { next, hash_size() });
            if (update_requested) {
                u8 const message[5] = { handshake_key_update, 0, 0, 1, 0 };
                TRY(send_encrypted(record_handshake, { message, 5 }));
                hkdf_expand_label({ m_write.secret, hash_size() }, "traffic upd"_sv, {}, { next, hash_size() });
                set_traffic_keys(m_write, { next, hash_size() });
            }
            break;
        }
        default:
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: an unexpected message after the handshake");
        }
        m_handshake_start += 4 + length;
    }
    if (m_handshake_start == m_handshake_bytes.size()) {
        m_handshake_bytes.clear();
        m_handshake_start = 0;
    }
    return {};
}

ErrorOr<size_t> Connection::read(Bytes output)
{
    for (;;) {
        size_t const available = m_application_data.size() - m_application_start;
        if (available > 0) {
            size_t const size = min(available, output.size());
            __builtin_memcpy(output.data(), m_application_data.data() + m_application_start, size);
            m_application_start += size;
            if (m_application_start == m_application_data.size()) {
                m_application_data.clear();
                m_application_start = 0;
            }
            return size;
        }
        if (m_closed_by_peer)
            return static_cast<size_t>(0);
        u8 type;
        Vector<u8> contents;
        TRY(read_record(type, contents));
        if (m_closed_by_peer)
            return static_cast<size_t>(0);
        if (type == record_application_data) {
            m_application_data = move(contents);
            m_application_start = 0;
        } else if (type == record_handshake) {
            TRY(process_post_handshake(contents.span()));
        } else if (type != record_change_cipher_spec) {
            (void)send_alert(alert_unexpected_message);
            return Error::from_string_literal("TLS: an unexpected kind of record");
        }
    }
}

ErrorOr<void> Connection::write(ReadonlyBytes data)
{
    if (m_closed_by_us)
        return Error::from_string_literal("TLS: writing after closing");
    if (data.is_empty())
        return {};
    return send_encrypted(record_application_data, data);
}

ErrorOr<void> Connection::close()
{
    if (m_closed_by_us)
        return {};
    m_closed_by_us = true;
    return send_alert(alert_close_notify);
}

}
