/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibCore/Account.h>
#include <LibCore/GetPassword.h>
#include <unistd.h>

// passwd [user]: changes a password, as macOS's passwd asks:
//
//   Changing password for vulture.
//   Old Password:
//   New Password:
//   Retype New Password:
//
// Root may change anyone's without the old one; others only their own.
int main(int argc, char** argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: passwd [user]\n");
        return 1;
    }
    auto account = argc > 1 ? Core::Account::from_name(StringView(argv[1])) : Core::Account::from_uid(getuid());
    if (account.is_error()) {
        fprintf(stderr, "passwd: unknown user %s\n", argc > 1 ? argv[1] : "");
        return 1;
    }
    bool privileged = getuid() == 0;
    if (!privileged && account.value().uid() != getuid()) {
        fprintf(stderr, "passwd: permission denied\n");
        return 1;
    }
    if (!account.value().knows_password()) {
        fprintf(stderr, "passwd: not installed set-user-ID root\n");
        return 1;
    }
    printf("Changing password for %.*s.\n", static_cast<int>(account.value().name().length()), account.value().name().characters_without_null_termination());
    fflush(stdout);
    if (!privileged && account.value().has_password()) {
        auto old_password = Core::get_password("Old Password:"_sv);
        if (old_password.is_error())
            return 1;
        if (!account.value().authenticate(old_password.value().view())) {
            fprintf(stderr, "passwd: authentication error\n");
            return 1;
        }
    }
    for (;;) {
        auto first = Core::get_password("New Password:"_sv);
        if (first.is_error())
            return 1;
        auto second = Core::get_password("Retype New Password:"_sv);
        if (second.is_error())
            return 1;
        if (first.value().view() != second.value().view()) {
            printf("Mismatch; try again, EOF to quit.\n");
            fflush(stdout);
            continue;
        }
        // Changing it takes the right to write /etc/master.passwd: ours.
        if (setuid(0) < 0) {
            fprintf(stderr, "passwd: %s\n", strerror(errno));
            return 1;
        }
        if (auto result = account.value().set_password(first.value().view()); result.is_error()) {
            fprintf(stderr, "passwd: %s\n", strerror(result.error().code()));
            return 1;
        }
        return 0;
    }
}
