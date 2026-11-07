/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibCore/EventLoop.h>
#include <stdio.h>
#include <string.h>

// notification [-a application] [-t title] [-m message] [-o action]... [-w]: posts one, as an
// application would, shown as that application's settings say. With -w it waits for what's done
// with it, and says: the action chosen, or "clicked".

int main(int argc, char** argv)
{
    StringView application = "Terminal"_sv;
    StringView title;
    StringView message;
    Vector<StringView> actions;
    bool wait = false;
    for (int i = 1; i < argc; ++i) {
        auto const argument = StringView(argv[i], strlen(argv[i]));
        bool const has_value = i + 1 < argc;
        if (argument == "-w"_sv) {
            wait = true;
        } else if (argument == "-a"_sv && has_value) {
            application = StringView(argv[i + 1], strlen(argv[i + 1]));
            ++i;
        } else if (argument == "-t"_sv && has_value) {
            title = StringView(argv[i + 1], strlen(argv[i + 1]));
            ++i;
        } else if (argument == "-m"_sv && has_value) {
            message = StringView(argv[i + 1], strlen(argv[i + 1]));
            ++i;
        } else if (argument == "-o"_sv && has_value) {
            actions.append(StringView(argv[i + 1], strlen(argv[i + 1])));
            ++i;
        } else {
            fprintf(stderr, "usage: notification [-a application] [-t title] [-m message] [-o action]... [-w]\n");
            return 64;
        }
    }
    if (title.is_empty() && message.is_empty()) {
        fprintf(stderr, "notification: a title or a message is needed\n");
        return 64;
    }
    Core::EventLoop loop;
    // Listening first, so that an answer can't come before there's anyone to hear it.
    OwnPtr<Desktop::NotificationResponses> responses;
    if (wait)
        responses = adopt_own(*new Desktop::NotificationResponses(loop, application));
    u32 const id = Desktop::post_notification(application, title, message, actions.span());
    if (id == 0) {
        fprintf(stderr, "notification: not allowed for %.*s, in Settings\n", static_cast<int>(application.length()), application.characters_without_null_termination());
        return 1;
    }
    if (!wait)
        return 0;
    int status = 0;
    responses->on_response = [&](u32 answered, StringView action) {
        if (answered != id)
            return;
        if (action.is_empty())
            printf("clicked\n");
        else
            printf("%.*s\n", static_cast<int>(action.length()), action.characters_without_null_termination());
        fflush(stdout);
        loop.quit(status);
    };
    return loop.exec();
}
