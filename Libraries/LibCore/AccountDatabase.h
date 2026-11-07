/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <sys/types.h>

// The people who use this computer, as opposed to the system's own accounts: those from 501 up, as on a Mac. Making one,
// taking one away, and who may administer the computer, by changing /etc/master.passwd, /etc/passwd and /etc/group
// together (only root can). One change at a time: /etc/ptmp is held while it's made, as BSD's tools hold it.
namespace Core::AccountDatabase {

constexpr uid_t first_person_uid = 501;
// Everyone's group, as on a Mac: "staff".
constexpr gid_t staff_gid = 20;

struct Person {
    String name;
    String full_name;
    String home;
    uid_t uid { 0 };
    bool administrator { false };
};

// Everyone, in the order they were made.
[[nodiscard]] Vector<Person> people();
[[nodiscard]] Optional<Person> person_named(StringView name);
[[nodiscard]] bool has_people();

// A short name an account can have: a lower-case letter, then lower-case letters, digits, '_', '-' and '.'; at most 31.
[[nodiscard]] bool is_valid_name(StringView);
// What a Mac suggests for a full name: its letters and digits, in lower case, run together ("Ada Lovelace": "adalovelace").
[[nodiscard]] String name_from_full_name(StringView full_name);

struct NewAccount {
    StringView name;
    StringView full_name;
    StringView password;
    StringView hint;
    bool administrator { false };
};

// The next user ID, a home with a Mac's folders, and a password hint if there's one. EEXIST if the name's taken,
// EINVAL if it can't be one.
ErrorOr<uid_t> create(NewAccount const&);

enum class HomeFolder {
    // Left where it is, renamed "<name> (Deleted)", as a Mac's "Don't change the home folder" does.
    Keep,
    Delete,
};
// EPERM for the last administrator.
ErrorOr<void> remove(StringView name, HomeFolder);
// EPERM for taking away the last administrator.
ErrorOr<void> set_administrator(StringView name, bool);

// What reminds someone of their password, shown after a few wrong ones. Only root can read them.
ErrorOr<void> set_hint(StringView name, StringView hint);
[[nodiscard]] Optional<String> hint(StringView name);

// Whether Setup has been through, or there's anyone at all (a disk from before there was Setup).
[[nodiscard]] bool setup_done();
ErrorOr<void> mark_setup_done();

}
