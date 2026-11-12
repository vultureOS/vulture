/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Operations.h"
#include "Common.h"
#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Panels.h>
#include <errno.h>
#include <fcntl.h>
#include <sandbox.h>
#include <stdio.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace UI;

namespace FilesApp {

namespace {

constexpr char const* put_back_attribute = "com.vulture.putback";
constexpr StringView helper_path = "/usr/libexec/FilesHelper"_sv;
// Below this it's over before a window could say anything about it.
constexpr u64 worth_a_window = 16 * MiB;
constexpr size_t chunk = 1 * MiB;

struct Step {
    String from;
    String to;
    bool is_directory { false };
    bool is_link { false };
    u32 mode { 0644 };
    u64 size { 0 };
};

class Transfer {
public:
    Transfer(TransferKind kind, Vector<String> sources, StringView folder, UI::Window& parent, Function<void()> done)
        : m_kind(kind)
        , m_sources(move(sources))
        , m_folder(MUST(String::from_view(folder)))
        , m_parent(parent)
        , m_done(move(done))
    {
    }

    void start()
    {
        m_next_source = 0;
        next_source();
    }

private:
    // One top-level item at a time: anything already there is asked about.
    void next_source()
    {
        while (m_next_source < m_sources.size()) {
            auto const source = m_sources[m_next_source];
            auto const name = last_component(source.view());
            auto target = joined(m_folder.view(), name);
            if (target.view() == source.view()) {
                // Onto itself: as Finder does with a copy, a new one beside it; a move is nothing.
                ++m_next_source;
                if (m_kind == TransferKind::Copy)
                    add(source.view(), joined(m_folder.view(), free_copy_name(m_folder.view(), name).view()).view());
                continue;
            }
            if (exists(target.view())) {
                ask(source.view(), target.view());
                return;
            }
            ++m_next_source;
            add(source.view(), target.view());
        }
        begin();
    }

    void ask(StringView source, StringView target)
    {
        StringBuilder message;
        message.appendff("An item named \xE2\x80\x9C{}\xE2\x80\x9D already exists in this location. Do you want to replace it with the one you\xE2\x80\x99re {}?", last_component(source), m_kind == TransferKind::Copy ? "copying"_sv : "moving"_sv);
        Alert alert(message.view());
        alert.add_button("Replace"_sv, true);
        alert.add_button("Stop"_sv);
        alert.add_button("Keep Both"_sv);
        auto owned_source = MUST(String::from_view(source));
        auto owned_target = MUST(String::from_view(target));
        printf("files: asking about %.*s\n", static_cast<int>(last_component(source).length()), last_component(source).characters_without_null_termination());
        fflush(stdout);
        alert.show(m_parent, [this, owned_source = move(owned_source), owned_target = move(owned_target)](size_t answer) {
            ++m_next_source;
            if (answer == 1) {
                finish();
                return;
            }
            if (answer == 0) {
                (void)remove_tree(owned_target.view());
                add(owned_source.view(), owned_target.view());
            } else {
                add(owned_source.view(), joined(m_folder.view(), free_name_keeping_extension(last_component(owned_source.view())).view()).view());
            }
            next_source();
        });
    }

    [[nodiscard]] String free_name_keeping_extension(StringView name)
    {
        auto dot = name.find_last('.');
        auto stem = dot.has_value() && dot.value() > 0 ? name.substring_view(0, dot.value()) : name;
        auto extension = dot.has_value() && dot.value() > 0 ? name.substring_view(dot.value()) : StringView {};
        for (int i = 2;; ++i) {
            StringBuilder candidate;
            candidate.appendff("{} {}{}", stem, i, extension);
            auto result = MUST(candidate.to_string());
            if (!exists(joined(m_folder.view(), result.view()).view()))
                return result;
        }
    }

    void add(StringView source, StringView target)
    {
        // On the same disk, a move's a change of name.
        if (m_kind == TransferKind::Move) {
            auto from = MUST(String::from_view(source));
            auto to = MUST(String::from_view(target));
            if (rename(from.characters(), to.characters()) == 0)
                return;
            m_to_remove.append(move(from));
        }
        flatten(source, target);
    }

    void flatten(StringView source, StringView target)
    {
        auto from = MUST(String::from_view(source));
        struct stat information {};
        if (lstat(from.characters(), &information) != 0)
            return;
        Step step;
        step.from = move(from);
        step.to = MUST(String::from_view(target));
        step.mode = information.st_mode & 07777;
        step.is_directory = S_ISDIR(information.st_mode);
        step.is_link = S_ISLNK(information.st_mode);
        step.size = S_ISREG(information.st_mode) ? static_cast<u64>(information.st_size) : 0;
        m_total += step.size;
        m_steps.append(move(step));
        if (S_ISDIR(information.st_mode)) {
            for (auto const& item : list_folder(source, true))
                flatten(item.path.view(), joined(target, item.name.view()).view());
        }
    }

    void begin()
    {
        if (m_steps.is_empty()) {
            finish();
            return;
        }
        printf("files: %s %zu\n", m_kind == TransferKind::Copy ? "copying" : "moving", m_steps.size());
        fflush(stdout);
        if (m_total >= worth_a_window)
            show_window();
        tick();
    }

    void show_window()
    {
        auto screen = Platform::the().screen_size();
        int const width = 420;
        // (With its title bar: what's in it is a line, the bar, and a line.)
        int const height = 124;
        m_window = adopt_own(*new UI::Window(m_kind == TransferKind::Copy ? "Copy"_sv : "Move"_sv, { (screen.width - width) / 2, (screen.height - height) / 3, width, height }, WS::WindowTitled));
        auto& stack = m_window->set_content<StackView>(Axis::Vertical, 6, Alignment::Fill, Insets::all(14));
        StringBuilder title;
        title.appendff("{} {} item{} to \xE2\x80\x9C{}\xE2\x80\x9D", m_kind == TransferKind::Copy ? "Copying"_sv : "Moving"_sv, m_steps.size(), m_steps.size() == 1 ? ""_sv : "s"_sv, last_component(m_folder.view()));
        (void)stack.add<Label>(title.view());
        auto& row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        m_bar = &row.add<ProgressBar>(0.f);
        m_bar->set_stretch(1, 0);
        m_bar->set_accessibility_label("Progress"_sv);
        auto& stop = row.add<Button>("Stop"_sv);
        stop.on_action = [this] { m_stopped = true; };
        m_detail = &stack.add<Label>(StringView {});
        m_detail->set_style(LabelStyle::Secondary);
        m_window->on_close_request = [this] {
            m_stopped = true;
            return false;
        };
        m_window->show();
    }

    void tick()
    {
        // In a test, slowly enough to be seen; otherwise, as fast as it'll go, a piece at a time so that nothing else has to wait.
        size_t budget = s_still ? chunk : chunk * 4;
        auto& loop = Application::the().event_loop();
        if (!m_stopped && m_current < m_steps.size()) {
            work(budget);
            update_window();
        }
        if (m_stopped || m_current >= m_steps.size()) {
            close_files();
            finish();
            return;
        }
        if (s_still && m_window)
            loop.add_timer(40, false, [this] { tick(); });
        else
            loop.deferred_invoke([this] { tick(); });
    }

    void close_files()
    {
        if (m_in >= 0)
            close(m_in);
        if (m_out >= 0)
            close(m_out);
        m_in = m_out = -1;
    }

    void work(size_t budget)
    {
        while (budget > 0 && m_current < m_steps.size()) {
            auto const& step = m_steps[m_current];
            if (step.is_directory) {
                (void)mkdir(step.to.characters(), step.mode);
                ++m_current;
                continue;
            }
            if (step.is_link) {
                char link_target[1024];
                ssize_t const length = readlink(step.from.characters(), link_target, sizeof(link_target) - 1);
                if (length >= 0) {
                    link_target[length] = 0;
                    (void)symlink(link_target, step.to.characters());
                }
                ++m_current;
                continue;
            }
            if (m_in < 0) {
                // On the same disk, the blocks can simply be shared.
                if (clonefile(step.from.characters(), step.to.characters(), 0) == 0) {
                    m_done_bytes += step.size;
                    ++m_current;
                    continue;
                }
                m_in = open(step.from.characters(), O_RDONLY | O_CLOEXEC);
                m_out = open(step.to.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, step.mode);
                if (m_in < 0 || m_out < 0) {
                    m_error = errno;
                    m_stopped = true;
                    return;
                }
            }
            static char buffer[chunk];
            ssize_t const got = read(m_in, buffer, sizeof(buffer));
            if (got > 0) {
                if (write(m_out, buffer, static_cast<size_t>(got)) != got) {
                    m_error = errno;
                    m_stopped = true;
                    return;
                }
                m_done_bytes += static_cast<u64>(got);
                budget -= min(budget, static_cast<size_t>(got));
                continue;
            }
            close_files();
            ++m_current;
        }
    }

    void update_window()
    {
        if (!m_window)
            return;
        m_bar->set_value(m_total ? static_cast<float>(static_cast<double>(m_done_bytes) / static_cast<double>(m_total)) : 1);
        StringBuilder detail;
        detail.appendff("{} of {}", size_text(m_done_bytes).view(), size_text(m_total).view());
        m_detail->set_text(detail.view());
    }

    void finish()
    {
        // What was stopped halfway isn't kept; what's been moved all the way isn't left behind.
        if (m_stopped && m_current < m_steps.size())
            (void)remove_tree(m_steps[m_current].to.view());
        if (!m_stopped) {
            for (auto const& source : m_to_remove)
                (void)remove_tree(source.view());
        }
        printf("files: %s\n", m_stopped ? "stopped" : "transfer done");
        fflush(stdout);
        if (m_error != 0) {
            StringBuilder message;
            message.append("The operation can\xE2\x80\x99t be completed."_sv);
            Alert alert(message.view(), StringView(strerror(m_error), strlen(strerror(m_error))));
            alert.add_button("OK"_sv);
            alert.show(m_parent, [](size_t) { });
        }
        if (m_window)
            m_window->hide();
        auto done = move(m_done);
        auto* self = this;
        Application::the().event_loop().deferred_invoke([self] { delete self; });
        if (done)
            done();
    }

    TransferKind m_kind;
    Vector<String> m_sources;
    String m_folder;
    UI::Window& m_parent;
    Function<void()> m_done;
    size_t m_next_source { 0 };
    Vector<Step> m_steps;
    Vector<String> m_to_remove;
    size_t m_current { 0 };
    u64 m_total { 0 };
    u64 m_done_bytes { 0 };
    int m_in { -1 };
    int m_out { -1 };
    int m_error { 0 };
    bool m_stopped { false };
    OwnPtr<UI::Window> m_window;
    ProgressBar* m_bar { nullptr };
    Label* m_detail { nullptr };
};

String trash_folder()
{
    return joined(Desktop::home_directory().view(), ".Trash"_sv);
}

// The helper, in a sandbox that reaches only these, and then what's to happen.
void run_helper(Vector<String> arguments, Vector<String> readable, Vector<String> writable, Function<void()> done)
{
    auto program = MUST(String::from_view(helper_path));
    StringBuilder rules;
    rules.append("read /usr/lib\nread /System/Library\n"_sv);
    rules.appendff("exec {}\n", program.view());
    for (auto const& path : readable)
        rules.appendff("read {}\n", path.view());
    for (auto const& path : writable)
        rules.appendff("write {}\n", path.view());
    auto profile = MUST(rules.to_string());
    pid_t child = fork();
    if (child < 0) {
        if (done)
            done();
        return;
    }
    if (child == 0) {
        Vector<char*> argv;
        argv.append(const_cast<char*>(program.characters()));
        for (auto& argument : arguments)
            argv.append(const_cast<char*>(argument.characters()));
        argv.append(nullptr);
        if (sandbox_init(profile.characters(), 0, nullptr) != 0)
            _exit(126);
        execv(argv[0], argv.data());
        _exit(127);
    }
    auto* timer = new int(-1);
    auto shared_done = adopt_own(*new Function<void()>(move(done)));
    auto& loop = Application::the().event_loop();
    *timer = loop.add_timer(50, true, [child, timer, done_pointer = shared_done.leak_ptr()] {
        int status = 0;
        pid_t const ended = waitpid(child, &status, WNOHANG);
        // (Files ignores SIGCHLD, so a child's end may be noticed only as there being no such child.)
        if (ended == 0)
            return;
        Application::the().event_loop().cancel_timer(*timer);
        delete timer;
        if (*done_pointer)
            (*done_pointer)();
        delete done_pointer;
    });
}

String common_folder(Vector<String> const& paths)
{
    return paths.is_empty() ? String {} : parent_of(paths[0].view());
}

}

void start_transfer(TransferKind kind, Vector<String> sources, StringView folder, UI::Window& parent, Function<void()> done)
{
    (new Transfer(kind, move(sources), folder, parent, move(done)))->start();
}

void move_to_trash(Span<String const> paths)
{
    auto trash = trash_folder();
    (void)mkdir(trash.characters(), 0755);
    for (auto const& path : paths) {
        auto name = free_name(trash.view(), last_component(path.view()));
        auto target = joined(trash.view(), name.view());
        if (move_item(path.view(), target.view()).is_error())
            continue;
        // Where it came from, for Put Back.
        (void)setxattr(target.characters(), put_back_attribute, path.characters(), path.length(), 0, XATTR_NOFOLLOW);
    }
}

bool can_put_back(StringView trashed)
{
    char buffer[1024];
    return getxattr(MUST(String::from_view(trashed)).characters(), put_back_attribute, buffer, sizeof(buffer), 0, XATTR_NOFOLLOW) > 0;
}

ErrorOr<void> put_back(StringView trashed)
{
    auto from = TRY(String::from_view(trashed));
    char buffer[1024];
    ssize_t const length = getxattr(from.characters(), put_back_attribute, buffer, sizeof(buffer) - 1, 0, XATTR_NOFOLLOW);
    if (length <= 0)
        return Error::from_errno(ENOATTR);
    StringView const original { buffer, static_cast<size_t>(length) };
    auto folder = parent_of(original);
    // Its folder may have gone as well.
    for (size_t i = 1; i <= folder.length(); ++i) {
        if (i == folder.length() || folder.view()[i] == '/')
            (void)mkdir(TRY(String::from_view(folder.view().substring_view(0, i))).characters(), 0755);
    }
    auto name = free_name(folder.view(), last_component(original));
    auto target = joined(folder.view(), name.view());
    TRY(move_item(from.view(), target.view()));
    (void)removexattr(target.characters(), put_back_attribute, XATTR_NOFOLLOW);
    return {};
}

void empty_trash(UI::Window& parent, Function<void()> done)
{
    Alert alert("Are you sure you want to permanently erase the items in the Trash?"_sv, "You can\xE2\x80\x99t undo this action."_sv);
    alert.add_button("Empty Trash"_sv, true);
    alert.add_button("Cancel"_sv);
    auto shared = adopt_own(*new Function<void()>(move(done)));
    alert.show(parent, [shared = move(shared)](size_t answer) mutable {
        if (answer != 0)
            return;
        for (auto const& item : list_folder(trash_folder().view(), true))
            (void)remove_tree(item.path.view());
        Platform::the().play_sound("trash"_sv);
        printf("files: trash emptied\n");
        fflush(stdout);
        if (*shared)
            (*shared)();
    });
}

ErrorOr<String> duplicate(StringView path)
{
    auto folder = parent_of(path);
    auto name = free_copy_name(folder.view(), last_component(path));
    auto target = joined(folder.view(), name.view());
    auto source = TRY(String::from_view(path));
    if (clonefile(source.characters(), target.characters(), 0) != 0)
        TRY(copy_file(path, target.view()));
    return target;
}

ErrorOr<String> make_alias(StringView path)
{
    auto folder = parent_of(path);
    StringBuilder wanted;
    wanted.appendff("{} alias", last_component(path));
    auto name = free_name(folder.view(), wanted.view());
    auto target = joined(folder.view(), name.view());
    if (symlink(TRY(String::from_view(path)).characters(), target.characters()) != 0)
        return Error::from_errno(errno);
    return target;
}

void compress(Vector<String> paths, Function<void()> done)
{
    if (paths.is_empty())
        return;
    auto folder = common_folder(paths);
    String archive_name;
    if (paths.size() == 1) {
        StringBuilder name;
        name.appendff("{}.zip", last_component(paths[0].view()));
        archive_name = free_name(folder.view(), name.view());
    } else {
        archive_name = free_name(folder.view(), "Archive.zip"_sv);
    }
    auto archive = joined(folder.view(), archive_name.view());
    Vector<String> arguments;
    arguments.append(MUST(String::from_view("compress"_sv)));
    arguments.append(archive);
    arguments.append(folder);
    Vector<String> readable;
    for (auto const& path : paths) {
        arguments.append(MUST(String::from_view(last_component(path.view()))));
        readable.append(path);
    }
    Vector<String> writable;
    writable.append(archive);
    run_helper(move(arguments), move(readable), move(writable), move(done));
}

void expand(StringView archive, Function<void()> done)
{
    auto folder = parent_of(archive);
    auto name = last_component(archive);
    if (name.ends_with(".zip"_sv))
        name = name.substring_view(0, name.length() - 4);
    auto destination = joined(folder.view(), free_name(folder.view(), name).view());
    (void)mkdir(destination.characters(), 0755);
    Vector<String> arguments;
    arguments.append(MUST(String::from_view("expand"_sv)));
    arguments.append(MUST(String::from_view(archive)));
    arguments.append(destination);
    Vector<String> readable;
    readable.append(MUST(String::from_view(archive)));
    Vector<String> writable;
    writable.append(destination);
    run_helper(move(arguments), move(readable), move(writable), move(done));
}

void rotate_picture(StringView path, bool clockwise, Function<void()> done)
{
    Vector<String> arguments;
    arguments.append(MUST(String::from_view("rotate"_sv)));
    arguments.append(MUST(String::from_view(clockwise ? "right"_sv : "left"_sv)));
    arguments.append(MUST(String::from_view(path)));
    Vector<String> writable;
    writable.append(MUST(String::from_view(path)));
    run_helper(move(arguments), {}, move(writable), [done = move(done)] {
        printf("files: rotated\n");
        fflush(stdout);
        if (done)
            done();
    });
}

}
