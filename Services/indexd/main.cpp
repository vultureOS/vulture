/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Index.h"
#include <Desktop.h>
#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <SearchEndpoint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sandbox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsevents.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// com.vulture.indexd: what files there are and what's in them, for Search. The first time it goes over the whole disk.
// After that the file system's log of changes says what's different, even from before the machine last started, so the
// index only ever has to be told what's new. A pass over the disk comes again only if the log loses track.

namespace {

// How much is done before giving someone a turn: a keystroke shouldn't have to wait for an index.
constexpr size_t entries_at_a_time = 96;
constexpr size_t changes_at_a_time = 64;
constexpr int deepest = 32;
constexpr u64 quiet_milliseconds = 50;

constexpr StringView watched_places[] = { "/Applications"_sv, "/Users"_sv };

bool should_index(StringView path)
{
    // Nothing hidden, and none of what a program keeps for itself in someone's Library.
    size_t components = 0;
    size_t start = 0;
    for (size_t i = 0; i <= path.length(); ++i) {
        if (i < path.length() && path[i] != '/')
            continue;
        auto component = path.substring_view(start, i - start);
        if (component.starts_with('.'))
            return false;
        // /Users/name/Library: the fourth piece counting the empty one before the first slash.
        if (components == 3 && component == "Library"_sv && path.starts_with("/Users/"_sv))
            return false;
        ++components;
        start = i + 1;
    }
    return true;
}

class Service final : public SearchStub {
public:
    Service(Core::EventLoop& loop, NonnullOwnPtr<Index::Store> store)
        : m_loop(loop)
        , m_store(move(store))
    {
        start();
    }

    String query(IPC::Sender const&, String text, u32 limit) override
    {
        return m_store->query(text.view(), limit);
    }

    void update(IPC::Sender const&) override
    {
        read_events();
        while (!m_changes.is_empty())
            handle_some_changes();
    }

    SearchEndpoint::StatusAnswer status(IPC::Sender const&) override
    {
        SearchEndpoint::StatusAnswer answer;
        answer.files = m_store->file_count();
        answer.words = m_store->word_count();
        answer.ready = m_walking.is_empty() && m_changes.is_empty() && !m_walk_pending;
        return answer;
    }

    SearchEndpoint::FindAnswer find(IPC::Sender const&, String query_text, String only_in, bool name_only, u32 limit) override
    {
        update_for_a_reader();
        SearchEndpoint::FindAnswer answer;
        answer.json = m_store->find(query_text.view(), only_in.view(), name_only, limit, answer.error);
        return answer;
    }

    String attributes(IPC::Sender const&, String path) override
    {
        update_for_a_reader();
        return m_store->attributes(path.view());
    }

private:
    void update_for_a_reader()
    {
        read_events();
        while (!m_changes.is_empty())
            handle_some_changes();
    }

    void start()
    {
        m_events = open("/dev/fsevents", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        bool const has_scanned = m_store->meta_integer("scan_complete"_sv).value_or(0) != 0;
        auto const last_seen = m_store->meta_integer("last_event"_sv);
        unsigned long long since = FSEVENTS_SINCE_NOW;
        if (has_scanned && last_seen.has_value())
            since = static_cast<unsigned long long>(last_seen.value());
        if (m_events >= 0) {
            char const* paths[sizeof(watched_places) / sizeof(watched_places[0])];
            Vector<String> keep;
            for (size_t i = 0; i < sizeof(watched_places) / sizeof(watched_places[0]); ++i) {
                keep.append(MUST(String::from_view(watched_places[i])));
                paths[i] = keep.last().characters();
            }
            vulture_fsevents_watch watch {};
            watch.since = since;
            watch.paths = paths;
            watch.path_count = sizeof(watched_places) / sizeof(watched_places[0]);
            if (ioctl(m_events, FSEVENTS_WATCH, &watch) != 0) {
                fprintf(stderr, "indexd: can't follow changes: %s\n", strerror(errno));
                close(m_events);
                m_events = -1;
            } else {
                m_loop.watch(m_events, Core::EventLoop::Interest::Read, [this] { read_events(); });
            }
        }
        // First of all, or if there's no way of knowing what's happened since.
        if (!has_scanned || m_events < 0) {
            begin_walk();
        } else {
            printf("indexd: ready, with %llu files from before\n", static_cast<unsigned long long>(m_store->file_count()));
            fflush(stdout);
        }
    }

    void begin_walk()
    {
        ++m_generation_counter;
        m_generation = m_store->meta_integer("generation"_sv).value_or(0) + 1;
        (void)m_store->begin();
        (void)m_store->set_meta_integer("generation"_sv, m_generation);
        (void)m_store->commit();
        m_walking.clear();
        // Last in, first done: applications first, as they're what's looked for first.
        m_walking.append(WalkEntry { MUST(String::from_view("/Users"_sv)), 0 });
        m_walking.append(WalkEntry { MUST(String::from_view("/System/Applications"_sv)), 0 });
        m_walking.append(WalkEntry { MUST(String::from_view("/Applications"_sv)), 0 });
        schedule_walk();
    }

    void schedule_walk()
    {
        if (m_walk_pending)
            return;
        m_walk_pending = true;
        m_loop.deferred_invoke([this] {
            m_walk_pending = false;
            walk_some();
        });
    }

    void walk_some()
    {
        (void)m_store->begin();
        size_t done = 0;
        while (done < entries_at_a_time && !m_walking.is_empty()) {
            auto entry = move(m_walking.last());
            m_walking.remove(m_walking.size() - 1);
            done += walk_directory(entry);
        }
        if (m_walking.is_empty()) {
            (void)m_store->forget_older_than(m_generation);
            (void)m_store->set_meta_integer("scan_complete"_sv, 1);
            remember_position();
        }
        if (auto result = m_store->commit(); result.is_error())
            fprintf(stderr, "indexd: saving: %s\n", strerror(result.error().code()));
        if (!m_walking.is_empty()) {
            schedule_walk();
        } else {
            printf("indexd: ready, with %llu files\n", static_cast<unsigned long long>(m_store->file_count()));
            fflush(stdout);
        }
    }

    struct WalkEntry {
        String path;
        int depth;
    };

    size_t walk_directory(WalkEntry const& entry)
    {
        DIR* directory = opendir(entry.path.characters());
        if (!directory)
            return 1;
        size_t count = 0;
        while (auto* found = readdir(directory)) {
            StringView name { found->d_name, strlen(found->d_name) };
            if (name == "."_sv || name == ".."_sv || name.starts_with('.'))
                continue;
            StringBuilder full;
            full.append(entry.path.view());
            if (!entry.path.view().ends_with('/'))
                full.append('/');
            full.append(name);
            auto path = MUST(full.to_string());
            if (!should_index(path.view()))
                continue;
            (void)m_store->put_path(path.view(), m_generation);
            ++count;
            struct stat information;
            if (lstat(path.characters(), &information) == 0 && S_ISDIR(information.st_mode) && !name.ends_with(".app"_sv) && entry.depth < deepest)
                m_walking.append(WalkEntry { move(path), entry.depth + 1 });
        }
        closedir(directory);
        return count + 1;
    }

    void read_events()
    {
        if (m_events < 0)
            return;
        static char bytes[128 * 1024];
        for (;;) {
            ssize_t const got = read(m_events, bytes, sizeof(bytes));
            if (got <= 0)
                break;
            for (ssize_t at = 0; at + static_cast<ssize_t>(sizeof(vulture_fsevent)) <= got;) {
                auto const* event = reinterpret_cast<vulture_fsevent const*>(bytes + at);
                note_event(*event, StringView(bytes + at + sizeof(vulture_fsevent), event->path_length ? event->path_length - 1 : 0));
                at += static_cast<ssize_t>((sizeof(vulture_fsevent) + event->path_length + 7) & ~7ul);
            }
        }
        if (!m_changes.is_empty() && !m_flush_timer_armed) {
            m_flush_timer_armed = true;
            m_loop.add_timer(quiet_milliseconds, false, [this] {
                m_flush_timer_armed = false;
                handle_some_changes();
                if (!m_changes.is_empty())
                    read_events_again_soon();
            });
        }
    }

    void read_events_again_soon()
    {
        m_loop.deferred_invoke([this] {
            if (m_changes.is_empty())
                return;
            handle_some_changes();
            if (!m_changes.is_empty())
                read_events_again_soon();
        });
    }

    void note_event(vulture_fsevent const& event, StringView path)
    {
        m_last_event = event.id;
        // The log's lost track, of some or of all.
        if (event.flags & (FSE_MUST_SCAN_SUBDIRECTORIES | FSE_KERNEL_DROPPED | FSE_ROOT_CHANGED)) {
            if (m_walking.is_empty() && !m_walk_pending)
                begin_walk();
            return;
        }
        // (The end of what's happened since last time is said as an event, of nothing in particular.)
        if ((event.flags & FSE_HISTORY_DONE) || path.is_empty() || path == "/"_sv || !should_index(path))
            return;
        if (m_seen_changes.contains(path))
            return;
        auto copy = MUST(String::from_view(path));
        m_seen_changes.set(copy, true);
        m_changes.append(move(copy));
    }

    void remember_position()
    {
        if (m_last_event != 0) {
            (void)m_store->set_meta_integer("last_event"_sv, static_cast<i64>(m_last_event));
            return;
        }
        // Nothing's been heard yet: the log's present position is where to pick it up from.
        if (m_events >= 0) {
            unsigned long long latest = 0;
            if (ioctl(m_events, FSEVENTS_LATEST, &latest) == 0)
                (void)m_store->set_meta_integer("last_event"_sv, static_cast<i64>(latest));
        }
    }

    void handle_some_changes()
    {
        (void)m_store->begin();
        size_t done = 0;
        i64 const generation = m_store->meta_integer("generation"_sv).value_or(1);
        while (done < changes_at_a_time && !m_changes.is_empty()) {
            auto path = move(m_changes.last());
            m_changes.remove(m_changes.size() - 1);
            m_seen_changes.remove(path.view());
            apply_change(path.view(), generation);
            ++done;
        }
        if (m_changes.is_empty())
            remember_position();
        (void)m_store->commit();
    }

    void apply_change(StringView path, i64 generation)
    {
        auto terminated = MUST(String::from_view(path));
        struct stat information;
        if (lstat(terminated.characters(), &information) != 0) {
            (void)m_store->forget_path(path);
            return;
        }
        (void)m_store->put_path(path, generation);
        // A folder that arrives whole hasn't said anything of what's inside it.
        if (S_ISDIR(information.st_mode) && !path.ends_with(".app"_sv))
            put_tree(path, generation, 0);
    }

    void put_tree(StringView path, i64 generation, int depth)
    {
        if (depth >= deepest)
            return;
        auto terminated = MUST(String::from_view(path));
        DIR* directory = opendir(terminated.characters());
        if (!directory)
            return;
        while (auto* found = readdir(directory)) {
            StringView name { found->d_name, strlen(found->d_name) };
            if (name.starts_with('.'))
                continue;
            StringBuilder full;
            full.append(path);
            if (!path.ends_with('/'))
                full.append('/');
            full.append(name);
            auto child = MUST(full.to_string());
            if (!should_index(child.view()))
                continue;
            (void)m_store->put_path(child.view(), generation);
            struct stat information;
            if (lstat(child.characters(), &information) == 0 && S_ISDIR(information.st_mode) && !name.ends_with(".app"_sv))
                put_tree(child.view(), generation, depth + 1);
        }
        closedir(directory);
    }

    Core::EventLoop& m_loop;
    NonnullOwnPtr<Index::Store> m_store;
    int m_events { -1 };
    Vector<WalkEntry> m_walking;
    bool m_walk_pending { false };
    i64 m_generation { 1 };
    int m_generation_counter { 0 };
    Vector<String> m_changes;
    HashMap<String, bool> m_seen_changes;
    bool m_flush_timer_armed { false };
    unsigned long long m_last_event { 0 };
};

ErrorOr<void> make_directories(StringView path)
{
    for (size_t i = 0; i <= path.length(); ++i) {
        if (i == path.length() || path[i] == '/') {
            if (i > 0) {
                auto terminated = MUST(String::from_view(path.substring_view(0, i)));
                if (mkdir(terminated.characters(), 0700) != 0 && errno != EEXIST)
                    return Error::from_errno(errno);
            }
        }
    }
    return {};
}

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    // Someone's own: each person's session has one, which sees what they can and says so only to them.
    char const* home_variable = getenv("HOME");
    if (!home_variable) {
        fprintf(stderr, "indexd: nobody's home\n");
        return 1;
    }
    StringView const home { home_variable, strlen(home_variable) };
    StringBuilder folder;
    folder.append(home);
    folder.append("/Library/Metadata"_sv);
    if (auto made = make_directories(folder.view()); made.is_error()) {
        fprintf(stderr, "indexd: %.*s: %s\n", static_cast<int>(folder.view().length()), folder.view().characters_without_null_termination(), strerror(made.error().code()));
        return 1;
    }
    auto pair = IPC::create_port();
    if (pair.is_error())
        return 1;
    auto ports = pair.release_value();
    if (auto registered = IPC::register_service("com.vulture.indexd"_sv, move(ports.send)); registered.is_error()) {
        fprintf(stderr, "indexd: register: %s\n", strerror(registered.error().code()));
        return 1;
    }
    // From here on, what it needs and no more: it reads whatever's in anyone's files.
    if (sandbox_init_for_program("service-search", SANDBOX_NAMED, "/usr/libexec/indexd", nullptr) != 0) {
        fprintf(stderr, "indexd: sandbox: %s\n", strerror(errno));
        return 1;
    }
    StringBuilder database;
    database.append(folder.view());
    database.append("/index.db"_sv);
    auto store = Index::Store::open(database.view());
    if (store.is_error()) {
        // A damaged index is only a thing to make again.
        auto terminated = MUST(database.to_string());
        unlink(terminated.characters());
        StringBuilder journal;
        journal.append(database.view());
        journal.append("-journal"_sv);
        auto journal_terminated = MUST(journal.to_string());
        unlink(journal_terminated.characters());
        store = Index::Store::open(database.view());
        if (store.is_error()) {
            fprintf(stderr, "indexd: can't open the index\n");
            return 1;
        }
    }
    Core::EventLoop loop;
    Service service(loop, store.release_value());
    IPC::Listener listener(loop, move(ports.receive), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
