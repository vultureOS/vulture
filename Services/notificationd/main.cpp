/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <NotificationsEndpoint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// com.vulture.notificationd: what programs post goes to whoever shows it (the banners and Notification Center), and stays
// until cleared, as each application's settings in Settings > Notifications say. One for the machine, as the system's
// daemons are: everyone's are kept apart by who they are, and each one's settings are their own, in
// ~/Library/Preferences/com.vulture.notifications.json. Which applications have posted, for Settings to list, are kept
// here, in /Library/Preferences/com.vulture.notificationd.json, by user.

namespace {

constexpr size_t kept_at_most = 64;
constexpr size_t actions_at_most = 4;
constexpr char const* seen_path = "/Library/Preferences/com.vulture.notificationd.json";
constexpr char const* seen_path_new = "/Library/Preferences/com.vulture.notificationd.json.new";

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

Optional<JSON::Value> read_json(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    StringBuilder text;
    char buffer[4096];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        text.append(StringView { buffer, static_cast<size_t>(got) });
    }
    close(fd);
    auto parsed = JSON::parse(text.view());
    if (parsed.is_error() || !parsed.value().is_object())
        return {};
    return parsed.release_value();
}

String key_of(uid_t uid)
{
    StringBuilder key;
    key.appendff("{}", uid);
    return MUST(key.to_string());
}

struct Note {
    uid_t uid { 0 };
    u32 id { 0 };
    String application;
    String title;
    String body;
    Vector<String> actions;
    i64 when { 0 };
};

// One application's settings, with a Mac's defaults: allowed, as temporary banners, kept, with a sound.
struct Rules {
    bool allow { true };
    String style;
    bool center { true };
    bool sound { true };
};

// Someone's home, from /etc/passwd: name:password:uid:gid:gecos:home:shell. (Read as it is, as root's getpwuid would read
// the file with the hashes in it, which this has no business seeing.)
Optional<String> home_of(uid_t uid)
{
    FILE* file = fopen("/etc/passwd", "r");
    if (!file)
        return {};
    char line[512];
    Optional<String> home;
    while (!home.has_value() && fgets(line, sizeof(line), file)) {
        StringView fields[7];
        size_t count = 0;
        size_t start = 0;
        size_t const length = strcspn(line, "\n");
        for (size_t i = 0; i <= length && count < 7; ++i) {
            if (i == length || line[i] == ':') {
                fields[count++] = StringView(line + start, i - start);
                start = i + 1;
            }
        }
        if (count < 7 || line[0] == '#')
            continue;
        auto const number = copy(fields[2]);
        if (static_cast<uid_t>(strtoul(number.characters(), nullptr, 10)) == uid && !fields[5].is_empty())
            home = copy(fields[5]);
    }
    fclose(file);
    return home;
}

Rules rules_for(uid_t uid, StringView application)
{
    Rules rules;
    rules.style = copy("temporary"_sv);
    auto const home = home_of(uid);
    if (!home.has_value())
        return rules;
    StringBuilder path;
    path.append(home->view());
    path.append("/Library/Preferences/com.vulture.notifications.json"_sv);
    auto const terminated = MUST(path.to_string());
    auto preferences = read_json(terminated.characters());
    if (!preferences.has_value())
        return rules;
    auto const* all = preferences->get_object("applications"_sv);
    auto const* mine = all ? all->get_object(application) : nullptr;
    if (!mine)
        return rules;
    rules.allow = mine->get_boolean("allow"_sv).value_or(true);
    if (auto style = mine->get_string("style"_sv); style.has_value() && (*style == "none"_sv || *style == "temporary"_sv || *style == "persistent"_sv))
        rules.style = copy(*style);
    rules.center = mine->get_boolean("center"_sv).value_or(true);
    rules.sound = mine->get_boolean("sound"_sv).value_or(true);
    return rules;
}

class Center final : public NotificationServiceStub {
public:
    Center()
    {
        auto seen = read_json(seen_path);
        if (!seen.has_value())
            return;
        for (size_t i = 0; i < seen->size(); ++i) {
            auto const& list = seen->at(i);
            if (!list.is_array())
                continue;
            auto const key = copy(seen->key_at(i));
            uid_t const uid = static_cast<uid_t>(strtoul(key.characters(), nullptr, 10));
            for (size_t j = 0; j < list.size(); ++j) {
                if (list.at(j).is_string())
                    m_applications.append({ uid, copy(list.at(j).as_string()) });
            }
        }
    }

    u32 post(IPC::Sender const& sender, String application, String title, String body, Vector<String> actions) override
    {
        if (application.is_empty())
            return 0;
        remember(sender.uid, application.view());
        auto const rules = rules_for(sender.uid, application.view());
        if (!rules.allow)
            return 0;
        while (actions.size() > actions_at_most)
            (void)actions.take_last();
        Note note { sender.uid, m_next_id++, move(application), move(title), move(body), move(actions), static_cast<i64>(time(nullptr)) };
        if (m_posted_by.size() > kept_at_most * 4)
            m_posted_by.clear();
        m_posted_by.set(note.id, Owner { note.uid, copy(note.application.view()) });
        // Only to what shows that user's.
        for (size_t i = 0; i < m_listeners.size();) {
            if (m_listeners[i].uid != note.uid) {
                ++i;
                continue;
            }
            if (m_listeners[i].proxy->posted(note.id, note.application.view(), note.title.view(), note.body.view(), note.actions, rules.style.view(), rules.sound).is_error()) {
                m_listeners.remove(i);
                continue;
            }
            ++i;
        }
        u32 const id = note.id;
        uid_t const uid = note.uid;
        // Not kept, when its application says so: it's shown, and then it's gone.
        if (rules.center) {
            m_notes.insert(0, move(note));
            size_t mine = 0;
            for (size_t i = 0; i < m_notes.size();) {
                if (m_notes[i].uid == uid && ++mine > kept_at_most) {
                    m_notes.remove(i);
                    continue;
                }
                ++i;
            }
            changed(uid);
        }
        return id;
    }

    String all(IPC::Sender const& sender) override
    {
        auto list = JSON::Value::array();
        for (auto const& note : m_notes) {
            if (note.uid != sender.uid)
                continue;
            auto one = JSON::Value::object();
            MUST(one.set("id"_sv, JSON::Value(static_cast<double>(note.id))));
            MUST(one.set("application"_sv, MUST(JSON::Value::string(note.application.view()))));
            MUST(one.set("title"_sv, MUST(JSON::Value::string(note.title.view()))));
            MUST(one.set("body"_sv, MUST(JSON::Value::string(note.body.view()))));
            MUST(one.set("when"_sv, JSON::Value(static_cast<double>(note.when))));
            auto actions = JSON::Value::array();
            for (auto const& action : note.actions)
                MUST(actions.append(MUST(JSON::Value::string(action.view()))));
            MUST(one.set("actions"_sv, move(actions)));
            MUST(list.append(move(one)));
        }
        return MUST(list.serialize());
    }

    void remove(IPC::Sender const& sender, u32 id) override
    {
        for (size_t i = 0; i < m_notes.size(); ++i) {
            if (m_notes[i].id == id && m_notes[i].uid == sender.uid) {
                m_notes.remove(i);
                changed(sender.uid);
                return;
            }
        }
    }

    void clear_application(IPC::Sender const& sender, String application) override
    {
        bool removed = false;
        for (size_t i = 0; i < m_notes.size();) {
            if (m_notes[i].uid == sender.uid && m_notes[i].application.view() == application.view()) {
                m_notes.remove(i);
                removed = true;
                continue;
            }
            ++i;
        }
        if (removed)
            changed(sender.uid);
    }

    void clear(IPC::Sender const& sender) override
    {
        for (size_t i = 0; i < m_notes.size();) {
            if (m_notes[i].uid == sender.uid) {
                m_notes.remove(i);
                continue;
            }
            ++i;
        }
        changed(sender.uid);
    }

    void subscribe(IPC::Sender const& sender, IPC::Port events) override { m_listeners.append({ sender.uid, adopt_own(*new NotificationEventsProxy(move(events))) }); }

    void listen(IPC::Sender const& sender, String application, IPC::Port responses) override
    {
        // The newest to listen is the one that hears.
        for (size_t i = 0; i < m_responders.size(); ++i) {
            if (m_responders[i].uid == sender.uid && m_responders[i].application.view() == application.view()) {
                m_responders.remove(i);
                break;
            }
        }
        m_responders.append(Responder { sender.uid, move(application), adopt_own(*new NotificationResponsesProxy(move(responses))) });
    }

    void respond(IPC::Sender const& sender, u32 id, String action) override
    {
        // Only what was posted to someone can be answered, by them.
        auto const* owner = m_posted_by.get(id);
        if (!owner || owner->uid != sender.uid)
            return;
        auto const application = copy(owner->application.view());
        for (size_t i = 0; i < m_notes.size(); ++i) {
            if (m_notes[i].id == id) {
                // Acted on, it's done with, as on a Mac.
                m_notes.remove(i);
                changed(sender.uid);
                break;
            }
        }
        for (size_t i = 0; i < m_responders.size(); ++i) {
            if (m_responders[i].uid != sender.uid || m_responders[i].application.view() != application.view())
                continue;
            if (m_responders[i].proxy->responded(id, action.view()).is_error())
                m_responders.remove(i);
            return;
        }
    }

    String applications(IPC::Sender const& sender) override
    {
        auto list = JSON::Value::array();
        for (auto const& seen : m_applications) {
            if (seen.uid == sender.uid)
                MUST(list.append(MUST(JSON::Value::string(seen.application.view()))));
        }
        return MUST(list.serialize());
    }

private:
    struct Owner {
        uid_t uid { 0 };
        String application;
    };
    struct Listener {
        uid_t uid { 0 };
        OwnPtr<NotificationEventsProxy> proxy;
    };
    struct Responder {
        uid_t uid { 0 };
        String application;
        OwnPtr<NotificationResponsesProxy> proxy;
    };
    struct Seen {
        uid_t uid { 0 };
        String application;
    };

    void remember(uid_t uid, StringView application)
    {
        for (auto const& seen : m_applications) {
            if (seen.uid == uid && seen.application.view() == application)
                return;
        }
        m_applications.append({ uid, copy(application) });
        // Everyone's, by user.
        Vector<uid_t> users;
        for (auto const& seen : m_applications) {
            if (!users.contains_slow(seen.uid))
                users.append(seen.uid);
        }
        auto all = JSON::Value::object();
        for (auto user : users) {
            auto list = JSON::Value::array();
            for (auto const& seen : m_applications) {
                if (seen.uid == user)
                    MUST(list.append(MUST(JSON::Value::string(seen.application.view()))));
            }
            MUST(all.set(key_of(user).view(), move(list)));
        }
        auto text = all.serialize(true);
        if (text.is_error())
            return;
        // The whole file or none of it, however the machine stops.
        int fd = open(seen_path_new, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return;
        bool const written = write(fd, text.value().characters(), text.value().length()) == static_cast<ssize_t>(text.value().length());
        fsync(fd);
        close(fd);
        if (!written || rename(seen_path_new, seen_path) < 0)
            unlink(seen_path_new);
    }

    void changed(uid_t uid)
    {
        for (size_t i = 0; i < m_listeners.size();) {
            if (m_listeners[i].uid == uid && m_listeners[i].proxy->changed().is_error()) {
                m_listeners.remove(i);
                continue;
            }
            ++i;
        }
    }

    u32 m_next_id { 1 };
    Vector<Note> m_notes;
    Vector<Seen> m_applications;
    HashMap<u32, Owner> m_posted_by;
    Vector<Listener> m_listeners;
    Vector<Responder> m_responders;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.notificationd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "notificationd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Center center;
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)center.handle(message); });
    return loop.exec();
}
