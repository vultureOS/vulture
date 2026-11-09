/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Security/Sandbox.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/New.h>
#include <LibCrypto/HMAC.h>
#include <LibCrypto/SHA2.h>

namespace Kernel {

namespace {

// Is `path` this, or inside it? ("/tmp/a" isn't inside "/tmp/ab".)
bool is_at_or_under(StringView path, StringView rule)
{
    if (rule == "/"_sv)
        return true;
    return path.starts_with(rule) && (path.length() == rule.length() || path[rule.length()] == '/');
}

bool matches_any(Vector<String> const& rules, StringView path)
{
    for (auto const& rule : rules) {
        if (is_at_or_under(path, rule.view()))
            return true;
    }
    return false;
}

// On the way to something allowed: a folder that holds it.
bool is_ancestor_of_any(Vector<String> const& rules, StringView path)
{
    for (auto const& rule : rules) {
        if (path == "/"_sv || is_at_or_under(rule.view(), path))
            return true;
    }
    return false;
}

}

ErrorOr<NonnullRefPtr<SandboxProfile>> SandboxProfile::parse(StringView text, RefPtr<SandboxProfile> outer)
{
    auto* raw = new (Base::nothrow) SandboxProfile;
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto profile = adopt_ref(*raw);
    profile->m_outer = move(outer);
    Optional<Error> problem;
    auto path_rule = [&](Vector<String>& rules, StringView rule) {
        // Only a path as it would be seen: from the root, nothing to resolve in it.
        bool bad = rule.is_empty() || rule[0] != '/' || (rule.length() > 1 && rule.ends_with("/"_sv));
        rule.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView part) {
            if (part == "."_sv || part == ".."_sv)
                bad = true;
        });
        if (bad || rule.contains("//"_sv)) {
            problem = Error::from_errno(EINVAL);
            return;
        }
        // What it really is: those are links, and the check is of where a path really goes.
        auto const is_in = [&](StringView top) { return is_at_or_under(rule, top); };
        StringBuilder real;
        if (is_in("/tmp"_sv) || is_in("/etc"_sv) || is_in("/var"_sv))
            real.append("/private"_sv);
        real.append(rule);
        auto copy = real.to_string();
        if (copy.is_error() || rules.try_append(copy.release_value()).is_error())
            problem = Error::from_errno(ENOMEM);
    };
    text.for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
        if (problem.has_value() || line[0] == '#')
            return;
        auto const space = line.find(' ');
        if (!space.has_value()) {
            problem = Error::from_errno(EINVAL);
            return;
        }
        auto const word = line.substring_view(0, space.value());
        auto const rest = line.substring_view(space.value() + 1);
        if (word == "name"_sv) {
            auto copy = String::from_view(rest);
            if (copy.is_error())
                problem = Error::from_errno(ENOMEM);
            else
                profile->m_name = copy.release_value();
        } else if (word == "read"_sv) {
            path_rule(profile->m_read, rest);
        } else if (word == "write"_sv) {
            path_rule(profile->m_write, rest);
        } else if (word == "exec"_sv) {
            path_rule(profile->m_execute, rest);
        } else if (word == "metadata"_sv) {
            path_rule(profile->m_metadata, rest);
        } else if (word == "service"_sv) {
            auto copy = String::from_view(rest);
            if (copy.is_error() || profile->m_services.try_append(copy.release_value()).is_error())
                problem = Error::from_errno(ENOMEM);
        } else if (word == "network"_sv) {
            if (rest == "outbound"_sv)
                profile->m_outbound = true;
            else if (rest == "inbound"_sv)
                profile->m_inbound = true;
            else if (rest == "configure"_sv)
                profile->m_configure = true;
            else
                problem = Error::from_errno(EINVAL);
        } else {
            problem = Error::from_errno(EINVAL);
        }
    });
    if (problem.has_value())
        return problem.release_value();
    return profile;
}

bool SandboxProfile::allows_here(SandboxOperation operation, StringView what) const
{
    switch (operation) {
    case SandboxOperation::FileRead:
        return matches_any(m_read, what) || matches_any(m_write, what);
    case SandboxOperation::FileWrite:
        return matches_any(m_write, what);
    case SandboxOperation::FileMetadata:
        return matches_any(m_read, what) || matches_any(m_write, what) || matches_any(m_metadata, what)
            || is_ancestor_of_any(m_read, what) || is_ancestor_of_any(m_write, what) || is_ancestor_of_any(m_metadata, what);
    case SandboxOperation::Execute:
        return matches_any(m_execute, what);
    case SandboxOperation::NetworkOutbound:
        return m_outbound;
    case SandboxOperation::NetworkInbound:
        return m_inbound;
    case SandboxOperation::NetworkConfigure:
        return m_configure;
    case SandboxOperation::Service:
        for (auto const& service : m_services) {
            if (service.view() == what)
                return true;
        }
        return false;
    }
    return false;
}

bool SandboxProfile::allows(SandboxOperation operation, StringView what) const
{
    for (auto const* profile = this; profile; profile = profile->m_outer.ptr()) {
        if (!profile->allows_here(operation, what))
            return false;
    }
    return true;
}

namespace {

constexpr size_t grant_key_size = 32;

// Made the first time it's needed. Nothing outside the kernel ever sees it.
u8 const* grant_key()
{
    static u8 s_key[grant_key_size];
    static bool s_made = false;
    static SpinLock s_lock;
    SpinLockGuard guard(s_lock);
    if (!s_made) {
        get_random_bytes({ s_key, grant_key_size });
        s_made = true;
    }
    return s_key;
}

using GrantDigest = Crypto::HMAC<Crypto::SHA256>::Digest;

GrantDigest grant_signature(bool writable, StringView path)
{
    Crypto::HMAC<Crypto::SHA256> hmac({ grant_key(), grant_key_size });
    u8 const kind = writable ? 'w' : 'r';
    hmac.update({ &kind, 1 });
    hmac.update(path.bytes());
    return hmac.digest();
}

char hex_digit(u8 value)
{
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

int value_of_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool is_plain_absolute_path(StringView path)
{
    if (path.is_empty() || path[0] != '/' || (path.length() > 1 && path.ends_with('/')))
        return false;
    bool plain = true;
    path.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView part) {
        if (part == "."_sv || part == ".."_sv)
            plain = false;
    });
    return plain;
}

}

// w or r, the signature in hexadecimal, and the path.
ErrorOr<String> sandbox_issue_grant(StringView path, bool writable)
{
    if (!is_plain_absolute_path(path))
        return Error::from_errno(EINVAL);
    auto const signature = grant_signature(writable, path);
    StringBuilder token;
    token.append(writable ? 'w' : 'r');
    for (size_t i = 0; i < signature.size(); ++i) {
        token.append(hex_digit(signature[i] >> 4));
        token.append(hex_digit(signature[i] & 15));
    }
    token.append(path);
    return token.to_string();
}

ErrorOr<SandboxGrant> sandbox_redeem_grant(StringView token)
{
    size_t const digits = GrantDigest {}.size() * 2;
    if (token.length() <= 1 + digits || (token[0] != 'w' && token[0] != 'r'))
        return Error::from_errno(EINVAL);
    bool const writable = token[0] == 'w';
    auto path = token.substring_view(1 + digits);
    if (!is_plain_absolute_path(path))
        return Error::from_errno(EINVAL);
    auto const signature = grant_signature(writable, path);
    u8 difference = 0;
    for (size_t i = 0; i < signature.size(); ++i) {
        int const high = value_of_hex(token[1 + i * 2]);
        int const low = value_of_hex(token[2 + i * 2]);
        if (high < 0 || low < 0)
            return Error::from_errno(EINVAL);
        difference |= static_cast<u8>((high << 4 | low) ^ signature[i]);
    }
    if (difference != 0)
        return Error::from_errno(EPERM);
    return SandboxGrant { TRY(String::from_view(path)), writable };
}

bool sandbox_grants_allow(Span<SandboxGrant const> grants, SandboxOperation operation, StringView what)
{
    for (auto const& grant : grants) {
        switch (operation) {
        case SandboxOperation::FileRead:
            if (is_at_or_under(what, grant.path.view()))
                return true;
            break;
        case SandboxOperation::FileWrite:
            if (grant.writable && is_at_or_under(what, grant.path.view()))
                return true;
            break;
        case SandboxOperation::FileMetadata:
            // And the folders on the way to it.
            if (is_at_or_under(what, grant.path.view()) || is_at_or_under(grant.path.view(), what))
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

bool sandbox_would_allow(SandboxOperation operation, StringView what)
{
    auto* process = Process::current();
    if (!process)
        return true;
    auto const* sandbox = process->sandbox();
    return !sandbox || sandbox->allows(operation, what) || process->sandbox_grants_allow(operation, what);
}

ErrorOr<void> sandbox_check(SandboxOperation operation, StringView what)
{
    auto* process = Process::current();
    if (!process)
        return {};
    auto const* sandbox = process->sandbox();
    if (!sandbox || sandbox->allows(operation, what) || process->sandbox_grants_allow(operation, what))
        return {};
    StringView description;
    switch (operation) {
    case SandboxOperation::FileRead:
        description = "file-read"_sv;
        break;
    case SandboxOperation::FileWrite:
        description = "file-write"_sv;
        break;
    case SandboxOperation::FileMetadata:
        description = "file-read-metadata"_sv;
        break;
    case SandboxOperation::Execute:
        description = "process-exec"_sv;
        break;
    case SandboxOperation::NetworkOutbound:
        description = "network-outbound"_sv;
        break;
    case SandboxOperation::NetworkInbound:
        description = "network-inbound"_sv;
        break;
    case SandboxOperation::NetworkConfigure:
        description = "network-configure"_sv;
        break;
    case SandboxOperation::Service:
        description = "service-lookup"_sv;
        break;
    }
    dmesgln("Sandbox: {}[{}], in {}, denied {} {}", process->name(), process->pid(), sandbox->name(), description, what);
    return Error::from_errno(EPERM);
}

}
