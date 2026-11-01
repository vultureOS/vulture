/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <pwd.h>
#include <sys/types.h>

namespace Core {

// A user account from /etc/passwd, /etc/master.passwd (whose password
// hashes only root can read) and /etc/group: who someone is, checking
// their password, changing it, and becoming them.
//
// Passwords are stored as "$pbkdf2-sha512$<rounds>$<salt>$<key>" (salt
// and key in hexadecimal): PBKDF2 with HMAC-SHA512 over a random salt, as
// macOS's ShadowHash does. "" is no password; "*" (or any hash starting
// with "*" or "!") cannot log in with a password.
class Account {
public:
    static ErrorOr<Account> from_name(StringView name);
    static ErrorOr<Account> from_uid(uid_t);

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] StringView full_name() const { return m_full_name.view(); }
    [[nodiscard]] StringView home() const { return m_home.view(); }
    [[nodiscard]] StringView shell() const { return m_shell.view(); }
    [[nodiscard]] uid_t uid() const { return m_uid; }
    [[nodiscard]] gid_t gid() const { return m_gid; }
    [[nodiscard]] Vector<gid_t> const& groups() const { return m_groups; }
    [[nodiscard]] bool is_in_group(StringView group_name) const;

    // Whether the hash was readable (we are root).
    [[nodiscard]] bool knows_password() const { return m_knows_password; }
    [[nodiscard]] bool has_password() const { return !m_password_hash.is_empty(); }
    [[nodiscard]] bool is_locked() const;
    [[nodiscard]] bool authenticate(StringView password) const;

    // Rewrites /etc/master.passwd with the new password's hash (root only).
    ErrorOr<void> set_password(StringView password);

    // The groups, then the group and user IDs (real, effective and saved).
    ErrorOr<void> become() const;

    // /Users/<name> with the folders macOS makes (Desktop, Documents,
    // Downloads, Library, Movies, Music, Pictures, Public), owned by the
    // user, if it is not there yet.
    ErrorOr<void> create_home_directory() const;

    [[nodiscard]] static ErrorOr<String> hash_password(StringView password);
    [[nodiscard]] static bool password_matches(StringView password, StringView hash);

private:
    static ErrorOr<Account> from_entry(struct passwd const&);

    String m_name;
    String m_full_name;
    String m_home;
    String m_shell;
    String m_password_hash;
    uid_t m_uid { 0 };
    gid_t m_gid { 0 };
    Vector<gid_t> m_groups;
    bool m_knows_password { false };
};

}
