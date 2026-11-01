/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/Account.h>
#include <LibCrypto/PBKDF2.h>
#include <LibCrypto/SHA2.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Core {

namespace {

constexpr u32 default_rounds = 45000;
constexpr size_t salt_size = 16;
constexpr size_t key_size = 64;

ReadonlyBytes bytes_of(StringView text)
{
    return { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() };
}

void append_hex(StringBuilder& builder, ReadonlyBytes bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (u8 byte : bytes) {
        builder.append(digits[byte >> 4]);
        builder.append(digits[byte & 15]);
    }
}

bool parse_hex(StringView text, u8* out, size_t size)
{
    if (text.length() != size * 2)
        return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < size; ++i) {
        int high = value(text[i * 2]), low = value(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        out[i] = static_cast<u8>(high * 16 + low);
    }
    return true;
}

}

ErrorOr<Account> Account::from_entry(struct passwd const& entry)
{
    Account account;
    account.m_name = TRY(String::from_view(StringView(entry.pw_name)));
    // The GECOS field's first part is the full name.
    StringView gecos(entry.pw_gecos);
    if (auto comma = gecos.find(','); comma.has_value())
        gecos = gecos.substring_view(0, comma.value());
    account.m_full_name = TRY(String::from_view(gecos));
    account.m_home = TRY(String::from_view(StringView(entry.pw_dir)));
    account.m_shell = TRY(String::from_view(StringView(entry.pw_shell)));
    account.m_uid = entry.pw_uid;
    account.m_gid = entry.pw_gid;
    // /etc/passwd says "*" for everyone; only /etc/master.passwd (read
    // when we are root) has the real field.
    account.m_knows_password = access("/etc/master.passwd", R_OK) == 0;
    if (account.m_knows_password)
        account.m_password_hash = TRY(String::from_view(StringView(entry.pw_passwd)));
    gid_t groups[64];
    int count = 64;
    getgrouplist(entry.pw_name, entry.pw_gid, groups, &count);
    for (int i = 0; i < count && i < 64; ++i)
        TRY(account.m_groups.try_append(groups[i]));
    return account;
}

ErrorOr<Account> Account::from_name(StringView name)
{
    auto copy = TRY(String::from_view(name));
    struct passwd* entry = getpwnam(copy.characters());
    if (!entry)
        return Error::from_errno(ENOENT);
    return from_entry(*entry);
}

ErrorOr<Account> Account::from_uid(uid_t uid)
{
    struct passwd* entry = getpwuid(uid);
    if (!entry)
        return Error::from_errno(ENOENT);
    return from_entry(*entry);
}

bool Account::is_in_group(StringView group_name) const
{
    auto copy = String::from_view(group_name);
    if (copy.is_error())
        return false;
    struct group* entry = getgrnam(copy.value().characters());
    if (!entry)
        return false;
    for (gid_t id : m_groups) {
        if (id == entry->gr_gid)
            return true;
    }
    return false;
}

bool Account::is_locked() const
{
    return m_password_hash.view().starts_with('*') || m_password_hash.view().starts_with('!');
}

bool Account::authenticate(StringView password) const
{
    if (!m_knows_password || is_locked())
        return false;
    if (!has_password())
        return true;
    return password_matches(password, m_password_hash.view());
}

ErrorOr<String> Account::hash_password(StringView password)
{
    u8 salt[salt_size];
    if (getentropy(salt, sizeof(salt)) < 0)
        return Error::from_errno(errno);
    u8 key[key_size];
    Crypto::pbkdf2<Crypto::SHA512>(bytes_of(password), { salt, sizeof(salt) }, default_rounds, { key, sizeof(key) });
    StringBuilder builder;
    builder.appendff("$pbkdf2-sha512${}$", default_rounds);
    append_hex(builder, { salt, sizeof(salt) });
    builder.append('$');
    append_hex(builder, { key, sizeof(key) });
    __builtin_memset(key, 0, sizeof(key));
    return builder.to_string();
}

bool Account::password_matches(StringView password, StringView hash)
{
    // "$pbkdf2-sha512$<rounds>$<salt>$<key>"
    StringView prefix = "$pbkdf2-sha512$"_sv;
    if (!hash.starts_with(prefix))
        return false;
    StringView rest = hash.substring_view(prefix.length());
    auto first = rest.find('$');
    if (!first.has_value())
        return false;
    StringView rounds_text = rest.substring_view(0, first.value());
    rest = rest.substring_view(first.value() + 1);
    auto second = rest.find('$');
    if (!second.has_value())
        return false;
    StringView salt_text = rest.substring_view(0, second.value());
    StringView key_text = rest.substring_view(second.value() + 1);
    u32 rounds = 0;
    for (char c : rounds_text) {
        if (c < '0' || c > '9' || rounds > 10'000'000)
            return false;
        rounds = rounds * 10 + static_cast<u32>(c - '0');
    }
    u8 salt[64];
    size_t salt_length = salt_text.length() / 2;
    u8 expected[key_size];
    if (rounds == 0 || salt_length > sizeof(salt) || !parse_hex(salt_text, salt, salt_length) || !parse_hex(key_text, expected, key_size))
        return false;
    u8 key[key_size];
    Crypto::pbkdf2<Crypto::SHA512>(bytes_of(password), { salt, salt_length }, rounds, { key, sizeof(key) });
    bool same = Crypto::constant_time_equal({ key, sizeof(key) }, { expected, sizeof(expected) });
    __builtin_memset(key, 0, sizeof(key));
    return same;
}

ErrorOr<void> Account::set_password(StringView password)
{
    auto hash = TRY(hash_password(password));
    FILE* in = fopen("/etc/master.passwd", "r");
    if (!in)
        return Error::from_errno(errno);
    // The new file beside the old, then put in its place in one step.
    int fd = open("/etc/master.passwd.new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        int error = errno;
        fclose(in);
        return Error::from_errno(error);
    }
    FILE* out = fdopen(fd, "w");
    char* line = nullptr;
    size_t size = 0;
    bool found = false;
    while (getline(&line, &size, in) >= 0) {
        char* colon = strchr(line, ':');
        if (colon && static_cast<size_t>(colon - line) == m_name.length() && strncmp(line, m_name.characters(), m_name.length()) == 0) {
            char* after = strchr(colon + 1, ':');
            if (after) {
                fprintf(out, "%s:%s%s", m_name.characters(), hash.characters(), after);
                found = true;
                continue;
            }
        }
        fputs(line, out);
    }
    free(line);
    fclose(in);
    bool written = fflush(out) == 0 && fsync(fd) == 0;
    fclose(out);
    if (!found || !written) {
        unlink("/etc/master.passwd.new");
        return Error::from_errno(found ? EIO : ENOENT);
    }
    if (rename("/etc/master.passwd.new", "/etc/master.passwd") < 0)
        return Error::from_errno(errno);
    m_password_hash = move(hash);
    return {};
}

ErrorOr<void> Account::become() const
{
    if (setgroups(m_groups.size(), m_groups.begin()) < 0)
        return Error::from_errno(errno);
    if (setgid(m_gid) < 0)
        return Error::from_errno(errno);
    if (setuid(m_uid) < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> Account::create_home_directory() const
{
    struct stat information;
    if (stat(m_home.characters(), &information) == 0)
        return {};
    if (mkdir(m_home.characters(), 0755) < 0)
        return Error::from_errno(errno);
    if (chown(m_home.characters(), m_uid, m_gid) < 0)
        return Error::from_errno(errno);
    // macOS's folders; Library, and the private ones, closed to others.
    static struct {
        char const* name;
        mode_t mode;
    } const folders[] = { { "Desktop", 0700 }, { "Documents", 0700 }, { "Downloads", 0700 }, { "Library", 0700 }, { "Movies", 0700 },
        { "Music", 0700 }, { "Pictures", 0700 }, { "Public", 0755 } };
    for (auto const& folder : folders) {
        StringBuilder path;
        path.append(m_home.view());
        path.append('/');
        path.append(StringView(folder.name));
        auto text = TRY(path.to_string());
        if (mkdir(text.characters(), folder.mode) < 0 && errno != EEXIST)
            return Error::from_errno(errno);
        (void)chmod(text.characters(), folder.mode);
        if (chown(text.characters(), m_uid, m_gid) < 0)
            return Error::from_errno(errno);
    }
    return {};
}

}
