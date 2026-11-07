/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <DirectoryEndpoint.h>
#include <LibCore/AccountDatabase.h>
#include <LibIPC/Bootstrap.h>
#include <string.h>
#include <unistd.h>

// sysadminctl: people's accounts, as macOS's makes and takes them away.
//
//   sysadminctl -addUser NAME [-fullName NAME] [-password PASSWORD] [-hint HINT] [-admin]
//               [-adminUser NAME -adminPassword PASSWORD]
//   sysadminctl -deleteUser NAME [-keepHome] [-adminUser NAME -adminPassword PASSWORD]
//
// Root changes the files itself; anyone else asks com.vulture.directory, with an administrator's name and password.
// What it says goes to standard error, as a Mac's does.
namespace {

int usage()
{
    fprintf(stderr, "usage: sysadminctl -addUser <user name> [-fullName <full name>] [-password <user password>] [-hint <user hint>] [-admin]\n"
                    "                   [-adminUser <administrator user name> -adminPassword <administrator password>]\n"
                    "       sysadminctl -deleteUser <user name> [-keepHome] [-adminUser <administrator user name> -adminPassword <administrator password>]\n");
    return 1;
}

int report(char const* doing, char const* name, int error)
{
    if (error == 0)
        return 0;
    char const* why = error == EACCES ? "the administrator's name or password is wrong"
        : error == EEXIST             ? "a user with that name already exists"
        : error == EINVAL             ? "that can't be a user's name"
        : error == EPERM              ? "it's the last administrator"
        : error == ENOENT             ? "there's no user with that name"
                                      : strerror(error);
    fprintf(stderr, "sysadminctl: %s %s failed: %s\n", doing, name, why);
    return 1;
}

}

int main(int argc, char** argv)
{
    char const* add = nullptr;
    char const* remove = nullptr;
    char const* full_name = "";
    char const* password = "";
    char const* hint = "";
    char const* administrator = "";
    char const* administrator_password = "";
    bool admin = false;
    bool keep_home = false;
    for (int i = 1; i < argc; ++i) {
        auto const is = [&](char const* option) { return strcmp(argv[i], option) == 0; };
        auto const next = [&]() -> char const* { return i + 1 < argc ? argv[++i] : nullptr; };
        char const** into = is("-addUser")   ? &add
            : is("-deleteUser")              ? &remove
            : is("-fullName")                ? &full_name
            : is("-password")                ? &password
            : is("-hint")                    ? &hint
            : is("-adminUser")               ? &administrator
            : is("-adminPassword")           ? &administrator_password
                                             : nullptr;
        if (into) {
            if (!(*into = next()))
                return usage();
        } else if (is("-admin")) {
            admin = true;
        } else if (is("-keepHome")) {
            keep_home = true;
        } else {
            return usage();
        }
    }
    if ((add != nullptr) == (remove != nullptr))
        return usage();

    if (geteuid() == 0) {
        if (add) {
            fprintf(stderr, "sysadminctl: Creating user record\xE2\x80\xA6\n");
            auto made = Core::AccountDatabase::create({ StringView(add), StringView(full_name), StringView(password), StringView(hint), admin });
            if (made.is_error())
                return report("creating", add, made.error().code());
            fprintf(stderr, "sysadminctl: Assigning UID: %u\n", made.value());
            fprintf(stderr, "sysadminctl: Creating home directory at /Users/%s\n", add);
            return 0;
        }
        fprintf(stderr, "sysadminctl: Deleting record for %s\n", remove);
        auto removed = Core::AccountDatabase::remove(StringView(remove), keep_home ? Core::AccountDatabase::HomeFolder::Keep : Core::AccountDatabase::HomeFolder::Delete);
        return removed.is_error() ? report("deleting", remove, removed.error().code()) : 0;
    }

    if (!*administrator) {
        fprintf(stderr, "sysadminctl: only root, or an administrator's name and password (-adminUser, -adminPassword), may change accounts\n");
        return 1;
    }
    auto port = IPC::look_up("com.vulture.directory"_sv);
    if (port.is_error()) {
        fprintf(stderr, "sysadminctl: com.vulture.directory isn't there\n");
        return 1;
    }
    DirectoryProxy directory(port.release_value());
    if (add) {
        fprintf(stderr, "sysadminctl: Creating user record\xE2\x80\xA6\n");
        auto answer = directory.create_user(StringView(administrator), StringView(administrator_password), StringView(add), StringView(full_name), StringView(password), StringView(hint), admin);
        return report("creating", add, answer.is_error() ? answer.error().code() : answer.value());
    }
    fprintf(stderr, "sysadminctl: Deleting record for %s\n", remove);
    auto answer = directory.delete_user(StringView(administrator), StringView(administrator_password), StringView(remove), !keep_home);
    return report("deleting", remove, answer.is_error() ? answer.error().code() : answer.value());
}
