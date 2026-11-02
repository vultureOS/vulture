/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibArchive/Tar.h>
#include <LibCompress/Deflate.h>
#include <LibUI/Application.h>
#include <LibUI/Panels.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Folders to .tar.gz and back. The work happens in a process of its own, so that the
// window stays alive, and so that an archive made to do harm has only that process to
// work on. An archive names its own members, so none is believed: nothing unpacks
// outside the new folder, through a link or otherwise.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

StringView last_component(StringView path)
{
    while (path.length() > 1 && path[path.length() - 1] == '/')
        path = path.substring_view(0, path.length() - 1);
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/')
            return path.substring_view(i, path.length() - i);
    }
    return path;
}

StringView parent_of(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/')
            return i == 1 ? "/"_sv : path.substring_view(0, i - 1);
    }
    return "."_sv;
}

String joined(StringView directory, StringView name)
{
    StringBuilder builder;
    builder.append(directory);
    if (directory.is_empty() || directory[directory.length() - 1] != '/')
        builder.append("/"_sv);
    builder.append(name);
    return MUST(builder.to_string());
}

ErrorOr<Vector<u8>> read_whole_file(StringView path)
{
    auto name = TRY(String::from_view(path));
    int fd = open(name.characters(), O_RDONLY);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> contents;
    u8 buffer[65536];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            return Error::from_errno(saved);
        }
        if (got == 0)
            break;
        for (ssize_t i = 0; i < got; ++i)
            TRY(contents.try_append(buffer[i]));
    }
    close(fd);
    return contents;
}

ErrorOr<void> write_whole_file(StringView path, ReadonlyBytes bytes, mode_t mode)
{
    auto name = TRY(String::from_view(path));
    // O_EXCL: never through something already there, such as a link.
    int fd = open(name.characters(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0)
        return Error::from_errno(errno);
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t wrote = write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            return Error::from_errno(saved);
        }
        done += static_cast<size_t>(wrote);
    }
    if (close(fd) != 0)
        return Error::from_errno(errno);
    return {};
}

[[nodiscard]] bool exists(StringView path)
{
    auto name = MUST(String::from_view(path));
    struct stat info {};
    return lstat(name.characters(), &info) == 0;
}

// "name", or else "name 2", ...
String free_name(StringView directory, StringView stem, StringView suffix)
{
    for (int i = 1;; ++i) {
        StringBuilder name;
        name.append(stem);
        if (i > 1)
            name.appendff(" {}", i);
        name.append(suffix);
        auto path = joined(directory, name.view());
        if (!exists(path.view()))
            return path;
    }
}

class VectorSink final : public Archive::TarSink {
public:
    void write(ReadonlyBytes bytes) override
    {
        for (u8 byte : bytes)
            m_bytes.append(byte);
    }

    [[nodiscard]] Vector<u8>& bytes() { return m_bytes; }

private:
    Vector<u8> m_bytes;
};

ErrorOr<void> add_tree(Archive::TarWriter& writer, StringView path, StringView member)
{
    auto name = TRY(String::from_view(path));
    struct stat info {};
    if (lstat(name.characters(), &info) != 0)
        return Error::from_errno(errno);
    if (S_ISLNK(info.st_mode)) {
        char target[1024];
        ssize_t length = readlink(name.characters(), target, sizeof(target));
        if (length < 0)
            return Error::from_errno(errno);
        return writer.add_symbolic_link(member, StringView(target, static_cast<size_t>(length)));
    }
    if (S_ISREG(info.st_mode)) {
        auto contents = TRY(read_whole_file(path));
        return writer.add_file(member, info.st_mode & 0777, contents.span());
    }
    if (!S_ISDIR(info.st_mode))
        return {}; // Devices and the like are not for archives.
    TRY(writer.add_directory(member, info.st_mode & 0777));
    DIR* directory = opendir(name.characters());
    if (!directory)
        return Error::from_errno(errno);
    Vector<String> names;
    while (auto* entry = readdir(directory)) {
        StringView entry_name(entry->d_name, strlen(entry->d_name));
        if (entry_name == "."_sv || entry_name == ".."_sv)
            continue;
        names.append(TRY(String::from_view(entry_name)));
    }
    closedir(directory);
    for (auto const& entry_name : names)
        TRY(add_tree(writer, joined(path, entry_name.view()).view(), joined(member, entry_name.view()).view()));
    return {};
}

ErrorOr<String> compress_folder(StringView folder)
{
    VectorSink sink;
    Archive::TarWriter writer(sink);
    TRY(add_tree(writer, folder, last_component(folder)));
    writer.finish();
    auto compressed = TRY(Compress::gzip_compress(sink.bytes().span()));
    auto output = free_name(parent_of(folder), last_component(folder), ".tar.gz"_sv);
    TRY(write_whole_file(output.view(), compressed.span(), 0644));
    return output;
}

// Every folder on the way to `member`, made if need be: refused if one is a link, since
// a link made earlier in this same archive could lead anywhere.
ErrorOr<void> make_parents(StringView root, StringView member)
{
    for (size_t i = 0; i < member.length(); ++i) {
        if (member[i] != '/')
            continue;
        auto path = joined(root, member.substring_view(0, i));
        struct stat info {};
        if (lstat(path.characters(), &info) == 0) {
            if (!S_ISDIR(info.st_mode))
                return Error::from_string_literal("the archive goes through something that is not a folder");
        } else if (mkdir(path.characters(), 0755) != 0) {
            return Error::from_errno(errno);
        }
    }
    return {};
}

ErrorOr<String> expand_archive(StringView archive)
{
    auto bytes = TRY(read_whole_file(archive));
    // By what it is, not what it is called.
    Vector<u8> unpacked;
    bool gzipped = bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
    if (gzipped)
        unpacked = TRY(Compress::gzip_decompress(bytes.span()));
    auto tar = gzipped ? unpacked.span() : bytes.span();

    auto reader = TRY(Archive::TarReader::parse(tar));

    // Everything under one folder: that folder is what comes out, beside the archive.
    // Otherwise a new folder, named after the archive, holds what there is.
    Optional<String> only_top;
    bool one_top = true;
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        size_t slash = 0;
        while (slash < entry.path.length() && entry.path[slash] != '/')
            ++slash;
        auto top = entry.path.substring_view(0, slash);
        if (!only_top.has_value())
            only_top = MUST(String::from_view(top));
        else if (only_top->view() != top)
            one_top = false;
        return one_top ? Archive::IterationDecision::Continue : Archive::IterationDecision::Break;
    }));
    one_top = one_top && only_top.has_value() && Archive::is_safe_member_path(only_top->view()) && only_top->view() != "."_sv;
    size_t strip = one_top ? only_top->length() : 0;
    // Where a member goes, under the new folder.
    auto inside = [&](StringView member) -> StringView {
        if (strip == 0)
            return member;
        if (member.length() <= strip + 1)
            return {};
        return member.substring_view(strip + 1, member.length() - strip - 1);
    };

    // All of it is looked at before anything is made, and as it will land: a link that
    // stays inside the archive may still leave the folder its top was taken off of.
    bool safe = true;
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        if (!Archive::is_safe_member_path(entry.path))
            safe = false;
        auto place = inside(entry.path);
        if (safe && !place.is_empty() && entry.type == Archive::TarEntryType::SymbolicLink && !Archive::is_safe_link_target(place, entry.link_target))
            safe = false;
        // The folder itself cannot be a link.
        if (safe && place.is_empty() && strip > 0 && entry.type != Archive::TarEntryType::Directory)
            safe = false;
        return safe ? Archive::IterationDecision::Continue : Archive::IterationDecision::Break;
    }));
    if (!safe)
        return Error::from_string_literal("the archive reaches outside itself: nothing was unpacked");

    StringView stem = one_top ? only_top->view() : last_component(archive);
    if (!one_top) {
        StringView const suffixes[] = { ".tar.gz"_sv, ".tgz"_sv, ".tar"_sv, ".gz"_sv };
        for (auto suffix : suffixes) {
            if (stem.length() > suffix.length() && stem.ends_with(suffix)) {
                stem = stem.substring_view(0, stem.length() - suffix.length());
                break;
            }
        }
    }
    auto root = free_name(parent_of(archive), stem, {});
    if (mkdir(root.characters(), 0755) != 0)
        return Error::from_errno(errno);

    Optional<Error> failure;
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        auto step = [&]() -> ErrorOr<void> {
            auto place = inside(entry.path);
            // The top folder: made already.
            if (place.is_empty())
                return {};
            TRY(make_parents(root.view(), place));
            auto path = joined(root.view(), place);
            switch (entry.type) {
            case Archive::TarEntryType::Directory: {
                struct stat info {};
                if (lstat(path.characters(), &info) == 0) {
                    if (!S_ISDIR(info.st_mode))
                        return Error::from_string_literal("the archive has a folder where something else is");
                    return {};
                }
                if (mkdir(path.characters(), (entry.mode & 0777) | 0700) != 0)
                    return Error::from_errno(errno);
                return {};
            }
            case Archive::TarEntryType::File:
                // No set-user bits, whatever it asks for.
                return write_whole_file(path.view(), entry.data, entry.mode & 0777);
            case Archive::TarEntryType::SymbolicLink: {
                auto target = TRY(String::from_view(entry.link_target));
                if (symlink(target.characters(), path.characters()) != 0)
                    return Error::from_errno(errno);
                return {};
            }
            case Archive::TarEntryType::Other:
                return {};
            }
            return {};
        };
        if (auto result = step(); result.is_error()) {
            failure = result.release_error();
            return Archive::IterationDecision::Break;
        }
        return Archive::IterationDecision::Continue;
    }));
    if (failure.has_value())
        return failure.release_value();
    return root;
}

// "ok <path>" or "error <what>", down the pipe.
template<typename Work>
void report_from_child(int fd, Work work)
{
    auto result = work();
    StringBuilder line;
    if (result.is_error()) {
        line.append("error "_sv);
        auto const& error = result.error();
        if (error.is_errno())
            line.append(StringView(strerror(error.code()), strlen(strerror(error.code()))));
        else
            line.append(error.string_literal());
    } else {
        line.append("ok "_sv);
        line.append(result.value().view());
    }
    auto text = line.view();
    (void)write(fd, text.characters_without_null_termination(), text.length());
}

class ArchiveWindow final : public UI::Window {
public:
    ArchiveWindow()
        : UI::Window("Archiver"_sv, frame())
    {
        auto& stack = set_content<StackView>(Axis::Vertical, 12, Alignment::Fill, Insets::all(20));
        auto const& t = tokens();
        auto& header = stack.add<StackView>(Axis::Horizontal, 14, Alignment::Center);
        header.set_fixed_height(t.metric("archiver.iconSize"_sv));
        auto& icon = header.add<IconView>();
        icon.set_fixed_size({ t.metric("archiver.iconSize"_sv), t.metric("archiver.iconSize"_sv) });
        auto& text = header.add<StackView>(Axis::Vertical, 4);
        text.set_stretch(1, 0);
        (void)text.add<Label>("Archiver"_sv, t.named_font("headline"_sv));
        auto& explanation = text.add<Label>("Compress a folder into a .tar.gz file, or expand one back into a folder beside it."_sv);
        explanation.set_style(LabelStyle::Secondary);
        explanation.set_wraps(true);

        m_status = &stack.add<Label>("Ready."_sv);
        m_status->set_wraps(true);
        m_progress = &stack.add<ProgressBar>();
        m_progress->set_hidden(true);
        auto& filler = stack.add<View>();
        filler.set_stretch(0, 1);
        auto& buttons = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& space = buttons.add<View>();
        space.set_stretch(1, 0);
        m_compress = &buttons.add<Button>("Compress Folder\xE2\x80\xA6"_sv);
        m_compress->on_action = [this] { choose_folder(); };
        m_expand = &buttons.add<Button>("Expand Archive\xE2\x80\xA6"_sv, ButtonStyle::Default);
        m_expand->on_action = [this] { choose_archive(); };
        buttons.set_fixed_height(22);
    }

    void compress(StringView folder)
    {
        auto path = MUST(String::from_view(folder));
        begin("Compressing"_sv, path.view(), [path = move(path)](int fd) {
            report_from_child(fd, [&] { return compress_folder(path.view()); });
        });
    }

    void expand(StringView archive)
    {
        auto path = MUST(String::from_view(archive));
        begin("Expanding"_sv, path.view(), [path = move(path)](int fd) {
            report_from_child(fd, [&] { return expand_archive(path.view()); });
        });
    }

private:
    class IconView final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            auto icon = Desktop::icon_named("Archiver"_sv, static_cast<int>(width()), static_cast<int>(graphics.scale()));
            if (icon)
                graphics.painter().draw_bitmap(*icon, FloatRect { 0, 0, width(), height() });
        }
    };

    [[nodiscard]] static IntRect frame()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("archiver.width"_sv));
        int height = static_cast<int>(t.metric("archiver.height"_sv));
        auto screen = Platform::the().screen_size();
        return { (screen.width - width) / 2, max(40, (screen.height - height) / 3), width, height };
    }

    void choose_folder()
    {
        auto home = Desktop::home_directory();
        FilePanel::run(*this, FilePanel::Kind::OpenFolder, home.view(), {}, [this](StringView chosen) {
            if (!chosen.is_empty())
                compress(chosen);
        });
    }

    void choose_archive()
    {
        auto home = Desktop::home_directory();
        FilePanel::run(*this, FilePanel::Kind::Open, home.view(), {}, [this](StringView chosen) {
            if (!chosen.is_empty())
                expand(chosen);
        });
    }

    void begin(StringView doing, StringView path, Function<void(int)> work)
    {
        if (m_child > 0)
            return;
        int fds[2];
        if (pipe(fds) != 0) {
            m_status->set_text("It could not be started."_sv);
            return;
        }
        pid_t child = fork();
        if (child < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            m_status->set_text("It could not be started."_sv);
            return;
        }
        if (child == 0) {
            ::close(fds[0]);
            work(fds[1]);
            _exit(0);
        }
        ::close(fds[1]);
        m_child = child;
        m_reading = fds[0];
        m_answer.clear();
        StringBuilder status;
        status.append(doing);
        status.append(" \xE2\x80\x9C"_sv);
        status.append(last_component(path));
        status.append("\xE2\x80\x9D\xE2\x80\xA6"_sv);
        m_status->set_text(status.view());
        m_progress->set_hidden(false);
        m_progress->set_indeterminate(true);
        m_compress->set_enabled(false);
        m_expand->set_enabled(false);
        m_watch = Application::the().event_loop().watch(m_reading, Core::EventLoop::Interest::Read, [this] { child_spoke(); });
    }

    void child_spoke()
    {
        char buffer[1024];
        ssize_t got = read(m_reading, buffer, sizeof(buffer));
        if (got > 0) {
            for (ssize_t i = 0; i < got; ++i)
                m_answer.append(buffer[i]);
            return;
        }
        if (got < 0 && errno == EINTR)
            return;
        Application::the().event_loop().unwatch(m_watch);
        ::close(m_reading);
        int status = 0;
        (void)waitpid(m_child, &status, 0);
        m_child = -1;
        m_progress->set_hidden(true);
        m_progress->set_indeterminate(false);
        m_compress->set_enabled(true);
        m_expand->set_enabled(true);
        StringView answer(m_answer.data(), m_answer.size());
        StringBuilder text;
        if (answer.starts_with("ok "_sv)) {
            text.append("Done: "_sv);
            text.append(answer.substring_view(3, answer.length() - 3));
        } else if (answer.starts_with("error "_sv)) {
            text.append("It failed: "_sv);
            text.append(answer.substring_view(6, answer.length() - 6));
        } else {
            text.append("It failed: the work stopped without an answer."_sv);
        }
        m_status->set_text(text.view());
        // For whoever is watching from a terminal (and for tests).
        printf("Archiver: %s\n", MUST(text.to_string()).characters());
        fflush(stdout);
    }

    Label* m_status { nullptr };
    ProgressBar* m_progress { nullptr };
    Button* m_compress { nullptr };
    Button* m_expand { nullptr };
    pid_t m_child { -1 };
    int m_reading { -1 };
    int m_watch { -1 };
    Vector<char> m_answer;
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Archiver"_sv, "com.vulture.Archiver"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Archiver: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    auto& window = application->make_window<ArchiveWindow>();
    window.show();
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-')
            continue;
        // In full: what it says at the end should mean something.
        char where[1024];
        String path = MUST(String::from_view(StringView(argv[i], strlen(argv[i]))));
        if (argv[i][0] != '/' && getcwd(where, sizeof(where)))
            path = joined(StringView(where, strlen(where)), path.view());
        struct stat info {};
        if (stat(path.characters(), &info) == 0 && S_ISDIR(info.st_mode))
            window.compress(path.view());
        else
            window.expand(path.view());
        break;
    }
    return application->exec();
}
