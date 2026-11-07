/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <DirectoryEndpoint.h>
#include <LibCore/Account.h>
#include <LibCore/AccountDatabase.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <errno.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// com.vulture.directory: the people who use this computer, for Settings (see Directory.ipc). It's root's, for the account
// files, and its sandbox lets it change those, the password hints and the homes in /Users, and nothing else.

namespace {

os_log_t directory_log()
{
    static os_log_t const log = os_log_create("com.vulture.directoryd", "accounts");
    return log;
}

class Service final : public DirectoryStub {
public:
    i32 check_administrator(IPC::Sender const& sender, String administrator, String password) override
    {
        return allowed(sender, administrator.view(), password.view()) ? 0 : EACCES;
    }

    i32 create_user(IPC::Sender const& sender, String administrator, String password, String name, String full_name, String new_password, String hint, bool admin) override
    {
        if (!allowed(sender, administrator.view(), password.view()))
            return EACCES;
        auto made = Core::AccountDatabase::create({ name.view(), full_name.view(), new_password.view(), hint.view(), admin });
        if (made.is_error()) {
            os_log_error(directory_log(), "Couldn't make %s: %s", name.characters(), strerror(made.error().code()));
            return made.error().code();
        }
        os_log(directory_log(), "Made %s (%u)%s, for process %d", name.characters(), made.value(), admin ? ", an administrator" : "", static_cast<int>(sender.pid));
        printf("directoryd: made %s\n", name.characters());
        fflush(stdout);
        return 0;
    }

    i32 delete_user(IPC::Sender const& sender, String administrator, String password, String name, bool delete_home) override
    {
        if (!allowed(sender, administrator.view(), password.view()))
            return EACCES;
        // Not one's own: there'd be nobody to have asked.
        auto asker = Core::Account::from_uid(sender.uid);
        if (!asker.is_error() && asker.value().name() == name.view())
            return EBUSY;
        auto removed = Core::AccountDatabase::remove(name.view(), delete_home ? Core::AccountDatabase::HomeFolder::Delete : Core::AccountDatabase::HomeFolder::Keep);
        if (removed.is_error())
            return removed.error().code();
        os_log(directory_log(), "Took away %s, for process %d", name.characters(), static_cast<int>(sender.pid));
        printf("directoryd: took away %s\n", name.characters());
        fflush(stdout);
        return 0;
    }

    i32 set_administrator(IPC::Sender const& sender, String administrator, String password, String name, bool admin) override
    {
        if (!allowed(sender, administrator.view(), password.view()))
            return EACCES;
        auto changed = Core::AccountDatabase::set_administrator(name.view(), admin);
        if (changed.is_error())
            return changed.error().code();
        os_log(directory_log(), "%s is %s an administrator, for process %d", name.characters(), admin ? "now" : "no longer", static_cast<int>(sender.pid));
        return 0;
    }

private:
    // Root, or the name and password of someone who administers the computer. A wrong one takes a second to say so,
    // so that guessing takes a long time.
    bool allowed(IPC::Sender const& sender, StringView administrator, StringView password)
    {
        if (sender.uid == 0)
            return true;
        auto account = Core::Account::from_name(administrator);
        bool const right = !account.is_error() && account.value().is_in_group("admin"_sv) && account.value().authenticate(password);
        if (!right) {
            os_log(directory_log(), "Process %d (user %u) gave a wrong administrator's name or password", static_cast<int>(sender.pid), sender.uid);
            struct timespec second { 1, 0 };
            nanosleep(&second, nullptr);
        }
        return right;
    }
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.directory"_sv);
    if (port.is_error()) {
        fprintf(stderr, "directoryd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Service service;
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
