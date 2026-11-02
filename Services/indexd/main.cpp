/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <SearchEndpoint.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// com.vulture.indexd: what files there are and what words they hold, for Search. It
// walks the places people keep things, and the applications, keeps an inverted index in
// memory, and walks again now and then and when asked. Names always; text only of files
// small enough to read in passing and that look like text.

namespace {

constexpr size_t largest_text_file = 256 * 1024;
constexpr u64 rescan_milliseconds = 60'000;
constexpr int deepest = 12;

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool is_word_character(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || static_cast<u8>(c) >= 0x80;
}

struct FileEntry {
    String path;
    String name;
    String lowered_name;
    // "application", "folder" or "document".
    StringView kind;
    // What an application is, in lower case: its name is not all it can be found by.
    String lowered_kind {};
};

class Index {
public:
    void clear()
    {
        m_files.clear();
        m_words.clear();
    }

    void add_tree(StringView path, int depth = 0)
    {
        auto terminated = MUST(String::from_view(path));
        DIR* directory = opendir(terminated.characters());
        if (!directory)
            return;
        while (auto* entry = readdir(directory)) {
            StringView name { entry->d_name, strlen(entry->d_name) };
            if (name.starts_with('.'))
                continue;
            StringBuilder full;
            full.append(path);
            if (!path.ends_with('/'))
                full.append("/"_sv);
            full.append(name);
            auto full_path = MUST(full.to_string());
            struct stat info;
            if (lstat(full_path.characters(), &info) != 0)
                continue;
            bool is_directory = S_ISDIR(info.st_mode);
            bool is_application = is_directory && name.ends_with(".app"_sv);
            add_file(full_path.view(), name, is_application ? "application"_sv : (is_directory ? "folder"_sv : "document"_sv));
            if (is_application) {
                if (auto bundle = Desktop::read_bundle(full_path.view()); !bundle.is_error()) {
                    StringBuilder lowered;
                    auto kind = bundle.value().kind.view();
                    for (size_t i = 0; i < kind.length(); ++i) {
                        char c = lower(kind[i]);
                        lowered.append(StringView(&c, 1));
                    }
                    m_files.last().lowered_kind = MUST(lowered.to_string());
                }
                continue;
            }
            if (is_directory) {
                if (depth < deepest)
                    add_tree(full_path.view(), depth + 1);
            } else if (S_ISREG(info.st_mode) && static_cast<size_t>(info.st_size) <= largest_text_file) {
                index_words(full_path.view(), m_files.size() - 1);
            }
        }
        closedir(directory);
    }

    [[nodiscard]] size_t file_count() const { return m_files.size(); }
    [[nodiscard]] size_t word_count() const { return m_words.size(); }

    [[nodiscard]] String query(StringView text, size_t limit) const
    {
        auto results = JSON::Value::array();
        StringBuilder lowered_query;
        for (size_t i = 0; i < text.length(); ++i) {
            char lowered = lower(text[i]);
            lowered_query.append(StringView(&lowered, 1));
        }
        auto wanted = lowered_query.view();
        if (wanted.is_empty())
            return MUST(results.serialize());

        struct Hit {
            size_t file;
            int score;
        };
        Vector<Hit> hits;
        // Names first: those that start with it above those that have it inside;
        // applications above all.
        for (size_t i = 0; i < m_files.size(); ++i) {
            auto const& file = m_files[i];
            auto name = file.lowered_name.view();
            int score = -1;
            if (name.starts_with(wanted))
                score = 100;
            else if (name.contains(wanted))
                score = 60;
            else if (file.lowered_kind.view().contains(wanted))
                score = 50;
            if (score >= 0) {
                if (file.kind == "application"_sv)
                    score += 50;
                hits.append(Hit { i, score });
            }
        }
        // Then words in the text: the whole word, or the start of one.
        m_words.for_each([&](String const& word, Vector<u32> const& files) {
            if (!word.view().starts_with(wanted))
                return;
            int score = word.view() == wanted ? 40 : 20;
            for (auto file : files) {
                bool seen = false;
                for (auto const& hit : hits) {
                    if (hit.file == file)
                        seen = true;
                }
                if (!seen)
                    hits.append(Hit { file, score });
            }
        });
        for (size_t i = 1; i < hits.size(); ++i) {
            for (size_t j = i; j > 0 && hits[j - 1].score < hits[j].score; --j) {
                auto moved = hits[j];
                hits[j] = hits[j - 1];
                hits[j - 1] = moved;
            }
        }
        for (size_t i = 0; i < hits.size() && i < limit; ++i) {
            auto const& file = m_files[hits[i].file];
            auto one = JSON::Value::object();
            MUST(one.set("path"_sv, MUST(JSON::Value::string(file.path.view()))));
            MUST(one.set("name"_sv, MUST(JSON::Value::string(file.name.view()))));
            MUST(one.set("kind"_sv, MUST(JSON::Value::string(file.kind))));
            MUST(results.append(move(one)));
        }
        return MUST(results.serialize());
    }

private:
    void add_file(StringView path, StringView name, StringView kind)
    {
        StringBuilder lowered;
        for (size_t i = 0; i < name.length(); ++i) {
            char c = lower(name[i]);
            lowered.append(StringView(&c, 1));
        }
        m_files.append(FileEntry { MUST(String::from_view(path)), MUST(String::from_view(name)), MUST(lowered.to_string()), kind });
    }

    void index_words(StringView path, size_t file)
    {
        auto terminated = MUST(String::from_view(path));
        int fd = open(terminated.characters(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        static char buffer[largest_text_file];
        ssize_t got = read(fd, buffer, sizeof(buffer));
        close(fd);
        if (got <= 0)
            return;
        // Text has no zero bytes in its first stretch.
        for (ssize_t i = 0; i < min<ssize_t>(got, 512); ++i) {
            if (buffer[i] == 0)
                return;
        }
        char word[64];
        size_t length = 0;
        auto finish = [&] {
            if (length >= 3) {
                StringView text { word, length };
                auto* files = m_words.get(text);
                if (!files) {
                    m_words.set(MUST(String::from_view(text)), Vector<u32> {});
                    files = m_words.get(text);
                }
                if (files->is_empty() || files->last() != file)
                    files->append(static_cast<u32>(file));
            }
            length = 0;
        };
        for (ssize_t i = 0; i < got; ++i) {
            if (is_word_character(buffer[i])) {
                if (length < sizeof(word))
                    word[length++] = lower(buffer[i]);
            } else {
                finish();
            }
        }
        finish();
    }

    Vector<FileEntry> m_files;
    mutable HashMap<String, Vector<u32>> m_words;
};

class Service final : public SearchStub {
public:
    explicit Service(Core::EventLoop& loop)
        : m_loop(loop)
    {
        scan();
        m_loop.add_timer(rescan_milliseconds, true, [this] { scan(); });
    }

    String query(IPC::Sender const&, String text, u32 limit) override
    {
        return m_index.query(text.view(), limit);
    }

    void update(IPC::Sender const&) override
    {
        m_loop.deferred_invoke([this] { scan(); });
    }

    SearchEndpoint::StatusAnswer status(IPC::Sender const&) override
    {
        SearchEndpoint::StatusAnswer answer;
        answer.files = m_index.file_count();
        answer.words = m_index.word_count();
        return answer;
    }

private:
    void scan()
    {
        m_index.clear();
        m_index.add_tree("/Applications"_sv);
        m_index.add_tree("/System/Applications"_sv);
        m_index.add_tree("/Users"_sv);
    }

    Core::EventLoop& m_loop;
    Index m_index;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.indexd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "indexd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Service service(loop);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
