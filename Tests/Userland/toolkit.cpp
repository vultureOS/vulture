/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibImage/Image.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/Panels.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// pranaUI on the real window server, as a program meets it: a vlaunchd of the test's
// own runs the server and a client (this program with --client), which makes windows
// and works them with an injected pointer. What host tests cannot show: that the two
// sides agree, that popups stay out of the way of the keyboard, drags between windows,
// moving, resizing, and appearances. The client reports in client.out.

namespace {

using namespace UI;
using namespace Gfx;

char const* const s_directory = "/tmp/toolkit-test";

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

void write_file(char const* name, char const* text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    (void)write(fd, text, strlen(text));
    close(fd);
}

char const* read_file(char const* name)
{
    static char text[16384];
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    text[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return text;
    size_t length = 0;
    ssize_t got;
    while (length + 1 < sizeof(text) && (got = read(fd, text + length, sizeof(text) - 1 - length)) > 0)
        length += static_cast<size_t>(got);
    text[length] = '\0';
    close(fd);
    return text;
}

void report(char const* what, bool ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
}

Application* s_app;

void settle(long milliseconds = 80)
{
    bool done = false;
    s_app->event_loop().add_timer(static_cast<u64>(milliseconds), false, [&] { done = true; });
    while (!done)
        s_app->event_loop().pump();
}

void pointer(int x, int y, u32 buttons = 0)
{
    (void)s_app->connection().server().inject_pointer(x, y, buttons);
    settle(40);
}

void click(int x, int y)
{
    pointer(x, y);
    pointer(x, y, WS::ButtonLeft);
    pointer(x, y);
}

NonnullRefPtr<Bitmap> screen()
{
    settle();
    auto answer = MUST(s_app->connection().server().capture());
    VERIFY(answer.error == 0);
    return MUST(Image::decode_png(answer.png.span()));
}

Color screen_color(int x, int y)
{
    auto picture = screen();
    int scale = s_app->scale();
    return picture->color_at(x * scale, y * scale);
}

OwnPtr<UI::Window> make_window(StringView title, IntRect frame, u32 flags = WS::WindowTitled | WS::WindowResizable)
{
    auto window = adopt_own(*new UI::Window(title, frame, flags));
    window->show();
    settle();
    return window;
}

void test_buttons()
{
    auto window = make_window("Buttons"_sv, { 100, 100, 400, 200 });
    report("a window that is shown is the active one", window->is_key());
    auto& button = window->root_view().add<Button>("Press"_sv);
    button.set_frame({ 20, 60, 100, 20 });
    int pressed = 0;
    button.on_action = [&] { ++pressed; };
    settle();
    auto frame = window->frame();
    click(frame.x + 40, frame.y + 70);
    report("a click on a button", pressed == 1);
    click(frame.x + 300, frame.y + 150);
    report("a click beside it", pressed == 1);
    window->hide();
}

void test_menus()
{
    auto window = make_window("Menus"_sv, { 100, 100, 400, 300 });
    auto& popup = window->root_view().add<PopUpButton>();
    popup.add_item("First"_sv);
    popup.add_item("Second"_sv);
    popup.add_item("Third"_sv);
    popup.set_frame({ 40, 60, 140, 20 });
    int changed = -1;
    popup.on_change = [&](int index) { changed = index; };
    settle();
    auto frame = window->frame();
    auto const& t = Appearance::the();
    int row = static_cast<int>(t.metric("menu.rowHeight"_sv));
    int button_y = frame.y + 70;
    pointer(frame.x + 60, button_y);
    pointer(frame.x + 60, button_y, WS::ButtonLeft);
    settle();
    report("a press opens the menu", popup.menu().is_open());
    report("an open menu takes no keyboard from its window", window->is_key());
    // Dragged down a row and let go, it chooses.
    pointer(frame.x + 60, button_y + row, WS::ButtonLeft);
    report("what is under the pointer lights up", popup.menu().highlight() == 1);
    pointer(frame.x + 60, button_y + row);
    settle();
    report("let go on an item, it is chosen", !popup.menu().is_open() && changed == 1 && popup.selected_index() == 1);

    // Pressed and let go at once, it stays; a press elsewhere ends it with no choice.
    pointer(frame.x + 60, button_y);
    pointer(frame.x + 60, button_y, WS::ButtonLeft);
    pointer(frame.x + 60, button_y);
    settle();
    report("a click leaves it open", popup.menu().is_open());
    changed = -1;
    pointer(frame.x + 380, frame.y + 280);
    pointer(frame.x + 380, frame.y + 280, WS::ButtonLeft);
    pointer(frame.x + 380, frame.y + 280);
    settle();
    report("a press outside closes it", !popup.menu().is_open() && changed == -1);
    window->hide();
}

class DragSource final : public View {
public:
    bool mouse_down(MouseEvent const& event) override
    {
        if (event.button != WS::ButtonLeft)
            return false;
        window()->begin_drag("public.utf8-plain-text"_sv, ReadonlyBytes(reinterpret_cast<u8 const*>("hello"), 5), { 40, 20 }, [](Graphics& graphics) {
            graphics.fill({ 0, 0, 40, 20 }, Color { 0, 122, 255, 128 });
        });
        return true;
    }
    void draw(Graphics& graphics) override { graphics.fill({ 0, 0, width(), height() }, Color { 200, 0, 0, 255 }); }
};

class DropTarget final : public View {
public:
    [[nodiscard]] bool accepts_drop(StringView type) const override { return type == "public.utf8-plain-text"_sv; }
    void drag_entered() override { ++entered; }
    void drag_exited() override { ++exited; }
    void perform_drop(StringView, ReadonlyBytes data) override { received = MUST(String::from_view(StringView(reinterpret_cast<char const*>(data.data()), data.size()))); }
    void draw(Graphics& graphics) override { graphics.fill({ 0, 0, width(), height() }, Color { 0, 200, 0, 255 }); }

    int entered { 0 };
    int exited { 0 };
    String received;
};

void test_drag_and_drop()
{
    auto from = make_window("From"_sv, { 60, 60, 240, 160 });
    auto to = make_window("To"_sv, { 400, 60, 240, 160 });
    auto& source = from->root_view().add<DragSource>();
    source.set_frame({ 20, 40, 80, 60 });
    auto& target = to->root_view().add<DropTarget>();
    target.set_frame({ 20, 40, 80, 60 });
    settle();
    auto from_frame = from->frame();
    auto to_frame = to->frame();
    int accepted = -1;
    from->on_drag_finished = [&](bool was) { accepted = was ? 1 : 0; };

    pointer(from_frame.x + 40, from_frame.y + 60);
    pointer(from_frame.x + 40, from_frame.y + 60, WS::ButtonLeft);
    pointer(from_frame.x + 120, from_frame.y + 60, WS::ButtonLeft);
    pointer(to_frame.x + 40, to_frame.y + 60, WS::ButtonLeft);
    settle();
    report("a drag reaches the window under it", target.entered == 1);
    pointer(to_frame.x + 200, to_frame.y + 140, WS::ButtonLeft);
    settle();
    report("and leaves what it left", target.exited == 1);
    pointer(to_frame.x + 40, to_frame.y + 60, WS::ButtonLeft);
    pointer(to_frame.x + 40, to_frame.y + 60);
    settle();
    report("a drop brings the data", target.received.view() == "hello"_sv);
    report("the source hears it was taken", accepted == 1);

    // Dropped on nothing.
    accepted = -1;
    pointer(from_frame.x + 40, from_frame.y + 60);
    pointer(from_frame.x + 40, from_frame.y + 60, WS::ButtonLeft);
    pointer(800, 700, WS::ButtonLeft);
    pointer(800, 700);
    settle();
    report("dropped on nothing, it was not taken", accepted == 0);
    from->hide();
    to->hide();
}

void test_moving_and_resizing()
{
    auto window = make_window("Moves"_sv, { 200, 150, 300, 200 });
    FloatSize last_size;
    window->on_resize = [&](FloatSize size) { last_size = size; };
    auto frame = window->frame();
    // By its title bar.
    pointer(frame.x + 150, frame.y + 14);
    pointer(frame.x + 150, frame.y + 14, WS::ButtonLeft);
    pointer(frame.x + 190, frame.y + 44, WS::ButtonLeft);
    pointer(frame.x + 190, frame.y + 44);
    settle();
    auto moved = window->frame();
    report("dragged by its title bar", moved.x == frame.x + 40 && moved.y == frame.y + 30);

    // By its corner.
    pointer(moved.right() - 2, moved.bottom() - 2);
    pointer(moved.right() - 2, moved.bottom() - 2, WS::ButtonLeft);
    pointer(moved.right() + 58, moved.bottom() + 38, WS::ButtonLeft);
    pointer(moved.right() + 58, moved.bottom() + 38);
    settle(200);
    auto resized = window->frame();
    report("dragged by its corner", resized.width == moved.width + 60 && resized.height == moved.height + 40);
    report("the toolkit hears, and lays out", last_size.width == static_cast<float>(resized.width) && window->root_view().width() == last_size.width);

    // A toolbar's empty part moves the window too.
    window->hide();
    auto unified = make_window("Unified"_sv, { 100, 100, 500, 300 }, WS::WindowTitled | WS::WindowUnifiedTitle);
    auto& toolbar = unified->root_view().add<Toolbar>();
    toolbar.set_frame({ 0, 0, 500, 52 });
    settle();
    frame = unified->frame();
    pointer(frame.x + 300, frame.y + 20);
    pointer(frame.x + 300, frame.y + 20, WS::ButtonLeft);
    pointer(frame.x + 330, frame.y + 40, WS::ButtonLeft);
    pointer(frame.x + 330, frame.y + 40);
    settle();
    report("dragged by its toolbar", unified->frame().x == frame.x + 30 && unified->frame().y == frame.y + 20);
    unified->hide();
}

void test_levels_and_closing()
{
    auto below = make_window("Below"_sv, { 100, 100, 300, 200 });
    below->root_view().add<View>();
    auto above = adopt_own(*new UI::Window("Above"_sv, { 150, 150, 200, 100 }, WS::WindowBare));
    class Filled final : public View {
    public:
        void draw(Graphics& graphics) override { graphics.fill({ 0, 0, width(), height() }, Color { 10, 200, 30, 255 }); }
    };
    above->root_view().add<Filled>().set_frame({ 0, 0, 200, 100 });
    above->backend().set_level(WS::LevelDock);
    above->show();
    settle();
    below->backend().activate();
    settle();
    auto seen = screen_color(200, 180);
    report("a higher level stays in front of what is activated", seen.green > 180 && seen.red < 40);
    report("the active one is still active", below->is_key());
    above->hide();

    bool closed = false;
    below->on_closed = [&] { closed = true; };
    auto frame = below->frame();
    auto const& t = Appearance::the();
    (void)t;
    click(frame.x + 14, frame.y + 14);
    settle();
    report("the close button closes it", closed);
}

void test_appearance()
{
    auto window = make_window("Looks"_sv, { 300, 200, 300, 200 });
    settle();
    auto light = screen_color(420, 320);
    report("the light background", light.red > 200);
    auto error = MUST(s_app->connection().server().set_appearance("Sepia"_sv));
    report("an appearance there is not is refused", error != 0);
    error = MUST(s_app->connection().server().set_appearance("Dark"_sv));
    report("dark is accepted", error == 0);
    settle(200);
    report("the program hears", Appearance::the().is_dark());
    auto dark = screen_color(420, 320);
    report("and redraws in it", dark.red < 80);
    (void)MUST(s_app->connection().server().set_appearance("Light"_sv));
    settle(200);
    report("and back", !Appearance::the().is_dark());
    window->hide();
}

void test_sheet()
{
    auto window = make_window("Sheet"_sv, { 200, 100, 500, 400 });
    auto& under = window->root_view().add<Button>("Under"_sv);
    under.set_frame({ 200, 340, 100, 20 });
    int under_pressed = 0;
    under.on_action = [&] { ++under_pressed; };
    size_t answer = 99;
    Alert alert("Really?"_sv, "This is the test's question."_sv);
    alert.add_button("Yes"_sv);
    alert.add_button("No"_sv);
    alert.show(*window, [&](size_t which) { answer = which; });
    settle(100);
    auto frame = window->frame();
    click(frame.x + 240, frame.y + 350);
    report("under a sheet, nothing can be pressed", under_pressed == 0);
    auto tree = Accessibility::tree_of(*window);
    auto const* no = Accessibility::find(tree, Accessibility::Role::Button, "No"_sv);
    report("the tree has its buttons", no != nullptr);
    if (no) {
        auto rect = no->frame;
        click(frame.x + static_cast<int>(rect.x + rect.width / 2), frame.y + static_cast<int>(rect.y + rect.height / 2));
    }
    settle(100);
    report("a button answers it", answer == 1);
    report("and it goes", !Sheet::is_presented(*window));
    click(frame.x + 240, frame.y + 350);
    report("what was under works again", under_pressed == 1);

    // Escape is Cancel, wherever that was added, and never a button that loses something.
    {
        size_t escaped = 99;
        Alert careful("Close without saving?"_sv, ""_sv);
        careful.add_button("Save"_sv, true);
        careful.add_button("Cancel"_sv);
        careful.add_button("Don't Save"_sv);
        careful.show(*window, [&](size_t which) { escaped = which; });
        settle(100);
        window->handle_key(true, KeyEscape, 0, {});
        settle(100);
        report("escape answers Cancel", escaped == 1);
    }
    {
        size_t escaped = 99;
        Alert nothing_to_cancel("Save the changes?"_sv, ""_sv);
        nothing_to_cancel.add_button("Save"_sv, true);
        nothing_to_cancel.add_button("Don't Save"_sv);
        nothing_to_cancel.show(*window, [&](size_t which) { escaped = which; });
        settle(100);
        window->handle_key(true, KeyEscape, 0, {});
        settle(100);
        report("with no Cancel, escape chooses nothing", escaped == 99);

        // Two that fit: side by side, the default on the right.
        auto side_by_side = Accessibility::tree_of(*window);
        auto const* save = Accessibility::find(side_by_side, Accessibility::Role::Button, "Save"_sv);
        auto const* discard = Accessibility::find(side_by_side, Accessibility::Role::Button, "Don't Save"_sv);
        report("two that fit are side by side, the default on the right",
            save && discard && save->frame.y == discard->frame.y && save->frame.x > discard->frame.x);
        window->handle_key(true, KeyEnter, 0, {});
        settle(100);
        report("return answers the default", escaped == 0);
    }
    {
        size_t answer_to_long = 99;
        Alert long_titles("Erase the disk?"_sv, ""_sv);
        long_titles.add_button("Cancel"_sv);
        long_titles.add_button("Erase and Install Everything"_sv, true);
        long_titles.show(*window, [&](size_t which) { answer_to_long = which; });
        settle(100);
        auto stacked = Accessibility::tree_of(*window);
        auto const* erase = Accessibility::find(stacked, Accessibility::Role::Button, "Erase and Install Everything"_sv);
        auto const* cancel = Accessibility::find(stacked, Accessibility::Role::Button, "Cancel"_sv);
        report("two that don't fit are stacked, the default first",
            erase && cancel && erase->frame.x == cancel->frame.x && erase->frame.y < cancel->frame.y);
        window->handle_key(true, KeyEscape, 0, {});
        settle(100);
        report("and answer as they were added", answer_to_long == 0);
    }
    window->hide();
}

int run_client()
{
    auto app_or_error = Application::create("toolkit-test"_sv);
    report("connecting", !app_or_error.is_error());
    if (app_or_error.is_error())
        return 1;
    auto app = app_or_error.release_value();
    s_app = app.ptr();
    set_motion_enabled(false);
    test_buttons();
    test_menus();
    test_drag_and_drop();
    test_moving_and_resizing();
    test_levels_and_closing();
    test_appearance();
    test_sheet();
    report("no token was missing", Appearance::missing_tokens() == 0);
    printf("done\n");
    fflush(stdout);
    _exit(0);
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--client") == 0)
        return run_client();

    system("rm -rf /tmp/toolkit-test");
    mkdir(s_directory, 0755);
    write_file("windowserver.plist", R"({
        "Label" : "com.vulture.windowserver",
        "ProgramArguments" : [ "/System/Library/CoreServices/WindowServer", "--no-input" ],
        "MachServices" : { "com.vulture.windowserver" : true },
        "RunAtLoad" : true,
        "StandardErrorPath" : "/tmp/toolkit-test/server.err"
    })");
    write_file("client.plist", R"({
        "Label" : "com.vulture.test.toolkit",
        "ProgramArguments" : [ "/usr/tests/toolkit", "--client" ],
        "RunAtLoad" : true,
        "StandardOutPath" : "/tmp/toolkit-test/client.out",
        "StandardErrorPath" : "/tmp/toolkit-test/client.out"
    })");

    pid_t launchd = fork();
    if (launchd == 0) {
        char const* arguments[] = { "vlaunchd", "--log", "/tmp/toolkit-test/log", "--directory", s_directory, nullptr };
        execv("/sbin/vlaunchd", const_cast<char**>(arguments));
        _exit(127);
    }
    for (int waited = 0; waited < 1600 && !strstr(read_file("client.out"), "done"); ++waited) {
        pause_milliseconds(50);
        int status = 0;
        if (waitpid(launchd, &status, WNOHANG) == launchd)
            break;
    }
    kill(launchd, SIGTERM);
    int status = 0;
    waitpid(launchd, &status, 0);

    static char report_text[16384];
    snprintf(report_text, sizeof(report_text), "%s", read_file("client.out"));
    int failures = 0;
    for (char const* at = report_text; (at = strstr(at, "FAIL:")); ++at)
        ++failures;
    bool finished = strstr(report_text, "done") != nullptr;
    CHECK(finished);
    CHECK(failures == 0);
    if (!finished || failures)
        fprintf(stderr, "toolkit: the client said:\n%s\nthe server said:\n%s\nvlaunchd's log:\n%s", report_text, read_file("server.err"), read_file("log"));
    system("rm -rf /tmp/toolkit-test");
    return finish();
}
