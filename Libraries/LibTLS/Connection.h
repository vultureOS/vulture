/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibCrypto/AEAD.h>
#include <LibTLS/Certificate.h>

// A TLS 1.3 client (RFC 8446). TLS 1.2 isn't here: a server that won't speak 1.3 gets a clear
// "no" rather than an older protocol. There's no client certificate, and no resuming of old
// sessions: each connection does the whole handshake.

namespace TLS {

// Whatever the bytes travel over.
class Transport {
public:
    virtual ~Transport() = default;
    // Up to `output`'s size; 0 at the end.
    virtual ErrorOr<size_t> read(Bytes output) = 0;
    virtual ErrorOr<void> write(ReadonlyBytes) = 0;
};

struct Options {
    // Who we mean to talk to: sent in the handshake, and what the certificate must be good for.
    StringView host;
    TrustStore const* trust_store { nullptr };
    // Seconds since 1970, for the certificates' dates.
    i64 now { 0 };
    // Fills a buffer with what can't be guessed.
    void (*random)(Bytes) { nullptr };
    // Off for "curl -k": the channel is still encrypted, but to whoever answers.
    bool verify_certificate { true };
    // Offered by ALPN, if any.
    StringView application_protocol;
    // What it is doing, for "curl -v".
    Function<void(StringView)> on_progress;
};

class Connection {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Connection>> handshake(Transport&, Options);
    ~Connection();

    // Application data, up to `output`'s size. 0 once the other side has said it is done.
    ErrorOr<size_t> read(Bytes output);
    ErrorOr<void> write(ReadonlyBytes);
    // Says we are done. The transport is the caller's to close.
    ErrorOr<void> close();

    [[nodiscard]] StringView cipher_suite_name() const;
    [[nodiscard]] StringView group_name() const;
    [[nodiscard]] StringView negotiated_protocol() const { return m_alpn.view(); }
    [[nodiscard]] String peer_subject() const { return m_peer_subject; }
    [[nodiscard]] String peer_issuer() const { return m_peer_issuer; }

private:
    enum class HashKind : u8 {
        SHA256,
        SHA384,
    };

    struct Direction {
        OwnPtr<Crypto::AEAD> cipher;
        u8 iv[12];
        u64 sequence { 0 };
        u8 secret[48];
    };

    explicit Connection(Transport&, Options);

    ErrorOr<void> run_handshake();
    ErrorOr<void> send_client_hello(bool retry);
    ErrorOr<void> process_server_hello(ReadonlyBytes, bool& is_retry);
    ErrorOr<void> process_encrypted_handshake();
    ErrorOr<void> process_certificate(ReadonlyBytes);
    ErrorOr<void> process_certificate_verify(ReadonlyBytes);
    ErrorOr<void> process_finished(ReadonlyBytes);
    ErrorOr<void> process_post_handshake(ReadonlyBytes);

    ErrorOr<void> send_record(u8 type, ReadonlyBytes);
    ErrorOr<void> send_alert(u8 description);
    ErrorOr<void> send_encrypted(u8 inner_type, ReadonlyBytes);
    // One record, opened if keys are in place: its type and contents.
    ErrorOr<void> read_record(u8& type, Vector<u8>& contents);
    // The next handshake message whole, however records cut it up.
    ErrorOr<void> read_handshake_message(u8& type, Vector<u8>& body);
    ErrorOr<void> fill(size_t count);

    void add_to_transcript(ReadonlyBytes);
    [[nodiscard]] size_t hash_size() const { return m_hash == HashKind::SHA256 ? 32 : 48; }
    void transcript_hash(u8* output) const;
    void hkdf_extract(ReadonlyBytes salt, ReadonlyBytes key, u8* output) const;
    void hkdf_expand_label(ReadonlyBytes secret, StringView label, ReadonlyBytes context, Bytes output) const;
    void derive_secret(ReadonlyBytes secret, StringView label, u8* output) const;
    void set_traffic_keys(Direction&, ReadonlyBytes secret);
    void compute_finished(ReadonlyBytes base_key, u8* output) const;

    void progress(StringView);

    Transport& m_transport;
    Options m_options;

    Vector<u8> m_transcript;
    HashKind m_hash { HashKind::SHA256 };
    u16 m_cipher_suite { 0 };
    u16 m_group { 0 };
    u8 m_session_id[32];
    u8 m_client_random[32];
    u8 m_x25519_private[32];
    u8 m_ec_private[48];
    Vector<u8> m_shared_secret;
    u16 m_retry_group { 0 };
    Vector<u8> m_retry_cookie;
    bool m_sent_dummy_change_cipher { false };

    u8 m_handshake_secret[48];
    u8 m_master_secret[48];
    u8 m_client_handshake_secret[48];
    u8 m_server_handshake_secret[48];
    Direction m_read;
    Direction m_write;

    Vector<NonnullOwnPtr<Certificate>> m_chain;
    String m_alpn;
    String m_peer_subject;
    String m_peer_issuer;

    Vector<u8> m_incoming;
    size_t m_incoming_start { 0 };
    Vector<u8> m_handshake_bytes;
    size_t m_handshake_start { 0 };
    Vector<u8> m_application_data;
    size_t m_application_start { 0 };
    bool m_closed_by_peer { false };
    bool m_closed_by_us { false };
};

}
