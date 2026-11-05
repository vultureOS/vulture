/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibCore/EventLoop.h>
#include <LibGfx/Painter.h>
#include <LibIPC/Bootstrap.h>
#include <LibImage/Image.h>
#include <Kernel/API/POSIX/privacy.h>
#include <LibCrypto/SHA2.h>
#include <LibWindow/Window.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vulture.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/sysctl.h>
#include <unistd.h>

// PranaWindowServer as programs meet it: a vlaunchd of the test's own runs it and
// a client (this program with --client), which makes windows, works them with an
// injected pointer, and looks at the screen. It finishes with the 10-window drag
// M8 is measured by. The client reports in client.out.

namespace {

using namespace Gfx;
using Window::MouseEvent;
using Window::MouseKind;

char const* const s_directory = "/tmp/windows-test";

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

u64 now_microseconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_nsec) / 1000;
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
    static char text[65536];
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

struct Client {
    Core::EventLoop loop;
    OwnPtr<Window::Connection> connection;

    // Runs the loop a while: events arrive, frames are drawn.
    void settle(long milliseconds = 60)
    {
        bool done = false;
        loop.add_timer(static_cast<u64>(milliseconds), false, [&] { done = true; });
        while (!done)
            loop.pump();
    }

    NonnullRefPtr<Bitmap> screen()
    {
        settle();
        auto answer = MUST(connection->server().capture());
        VERIFY(answer.error == 0);
        return MUST(Image::decode_png(answer.png.span()));
    }

    void pointer(int x, int y, u32 buttons = 0)
    {
        (void)connection->server().inject_pointer(x, y, buttons, now_microseconds());
    }
};

NonnullOwnPtr<Window::Window> filled_window(Client& client, StringView title, IntRect frame, u32 flags, Color color)
{
    auto window = MUST(Window::Window::create(*client.connection, title, frame, flags));
    window->back_buffer().clear(color);
    window->present();
    window->wait_until_presented(client.loop);
    return window;
}

bool near(Color a, Color b, int tolerance = 2)
{
    auto within = [tolerance](int x, int y) { return x - y <= tolerance && y - x <= tolerance; };
    return within(a.red, b.red) && within(a.green, b.green) && within(a.blue, b.blue);
}

void test_basics(Client& client)
{
    auto size = client.connection->screen_size();
    int scale = client.connection->scale();
    report("the screen", size.width > 0 && size.height > 0 && scale >= 1);

    auto bad = Window::Window::create(*client.connection, "x"_sv, { 0, 0, -1, 10 }, 0);
    report("a window of negative size is refused", bad.is_error());
    auto huge = Window::Window::create(*client.connection, "x"_sv, { 0, 0, 100000, 10 }, 0);
    report("an enormous window is refused", huge.is_error());

    // A red window: red where it says, wallpaper outside.
    auto before = client.screen();
    Color const red = Color::from_rgb(0xff0000);
    auto window = filled_window(client, "Red"_sv, { 100, 100, 300, 200 }, Window::WindowTitled, red);
    auto shot = client.screen();
    int s = scale;
    report("the window's pixels show", near(shot->color_at(250 * s, 200 * s), red));
    report("under the title bar: the chrome", !near(shot->color_at(250 * s, 110 * s), red));
    report("away from it: the wallpaper", shot->color_at(20, 20) == before->color_at(20, 20));

    // The corner is rounded: the very corner shows what is behind (with shadow).
    auto corner = shot->color_at(100 * s, 100 * s);
    report("rounded corners", !near(corner, Color::from_rgb(0xececec), 8));
    report("the corner's edge is antialiased", shot->color_at(100 * s + 3 * s, 100 * s + 3 * s) != shot->color_at(100 * s + 40 * s, 100 * s + 3 * s));

    // A shadow: darker just below than the wallpaper there was.
    auto below_before = before->color_at(250 * s, 306 * s);
    auto below_after = shot->color_at(250 * s, 306 * s);
    report("a shadow below", below_after.red + 10 < below_before.red || below_after.blue + 10 < below_before.blue);

    // Drawing and presenting a part: that part changes.
    Color const blue = Color::from_rgb(0x0000ff);
    {
        Painter painter(window->back_buffer());
        painter.fill_rect({ 0, 100 * static_cast<float>(s), 50 * static_cast<float>(s), 50 * static_cast<float>(s) }, blue);
    }
    window->present({ 0, 100 * s, 50 * s, 50 * s });
    window->wait_until_presented(client.loop);
    shot = client.screen();
    report("a partial present", near(shot->color_at(110 * s, 210 * s), blue) && near(shot->color_at(180 * s, 210 * s), red));

    // Drawing on goes on from what was there: the other buffer has it too.
    {
        Painter painter(window->back_buffer());
        painter.fill_rect({ 200 * static_cast<float>(s), 100 * static_cast<float>(s), 50 * static_cast<float>(s), 50 * static_cast<float>(s) }, blue);
    }
    window->present({ 200 * s, 100 * s, 50 * s, 50 * s });
    window->wait_until_presented(client.loop);
    shot = client.screen();
    report("both buffers stay current", near(shot->color_at(110 * s, 210 * s), blue) && near(shot->color_at(310 * s, 210 * s), blue));

    // Bad requests are ignored, not fatal.
    (void)client.connection->server().present(9999, 0, 0, 0, 10, 10);
    (void)client.connection->server().present(window->id(), 7, 0, 0, 10, 10);
    (void)client.connection->server().set_frame(window->id(), 0, 0, -5, -5);
    auto small = vulture_shared_memory_create(16, O_CLOEXEC);
    auto refused = client.connection->server().set_surface(window->id(), IPC::Port(small), 100, 100);
    report("a surface too small is refused", !refused.is_error() && refused.value() == EINVAL);
    Vector<i32> wrong;
    wrong.append(1);
    (void)client.connection->server().set_materials(window->id(), wrong);
    client.settle();
    shot = client.screen();
    report("still drawing after bad requests", near(shot->color_at(180 * s, 210 * s), red));

    window->set_title("Renamed"_sv);
    window->set_frame({ 200, 150, 300, 200 });
    shot = client.screen();
    report("set_frame moves it", near(shot->color_at(350 * s, 250 * s), red) && !near(shot->color_at(120 * s, 150 * s), red));
    window->show(false);
    shot = client.screen();
    report("hidden, it is gone", !near(shot->color_at(350 * s, 250 * s), red));
    window->show(true);
    shot = client.screen();
    report("shown, it is back", near(shot->color_at(350 * s, 250 * s), red));
    window = filled_window(client, "x"_sv, { 0, 0, 10, 10 }, Window::WindowBare, red);
    // (The red window is gone with the old object.)
    shot = client.screen();
    report("destroyed, it is gone", !near(shot->color_at(350 * s, 250 * s), red));
    report("a bare window has no corners", near(shot->color_at(0, 0), red));
}

void test_pointer(Client& client)
{
    int s = client.connection->scale();
    auto window = filled_window(client, "Pointer"_sv, { 200, 200, 400, 300 }, Window::WindowTitled | Window::WindowResizable, Color::from_rgb(0x00ff00));
    Vector<MouseEvent> events;
    window->on_mouse = [&](MouseEvent const& event) { events.append(event); };
    auto saw = [&](MouseKind kind, int x, int y) {
        for (auto const& event : events) {
            if (event.kind == kind && event.position.x == x && event.position.y == y)
                return true;
        }
        return false;
    };

    client.pointer(300, 300);
    client.settle();
    report("entering", !events.is_empty() && events[0].kind == MouseKind::Entered);
    client.pointer(310, 305);
    client.settle();
    report("moving, in the window's coordinates", saw(MouseKind::Moved, 110, 105));
    client.pointer(310, 305, Window::ButtonLeft);
    client.settle();
    report("pressing", saw(MouseKind::Pressed, 110, 105));
    // While held, it goes on going to this window, even from outside it.
    client.pointer(50, 50, Window::ButtonLeft);
    client.settle();
    report("captured while held", saw(MouseKind::Moved, -150, -150));
    client.pointer(50, 50, 0);
    client.settle();
    report("releasing", saw(MouseKind::Released, -150, -150));
    report("leaving", saw(MouseKind::Left, -150, -150) || (client.pointer(50, 60), client.settle(), saw(MouseKind::Left, -150, -140)));

    // Dragging by the title bar.
    events.clear();
    client.pointer(400, 210);
    client.pointer(400, 210, Window::ButtonLeft);
    client.settle(30);
    client.pointer(450, 260, Window::ButtonLeft);
    client.settle(30);
    client.pointer(450, 260, 0);
    auto shot = client.screen();
    Color const green = Color::from_rgb(0x00ff00);
    report("dragging moves it", near(shot->color_at(600 * s, 400 * s), green) && !near(shot->color_at(210 * s, 240 * s), green));
    report("the title bar's press is not the client's", !saw(MouseKind::Pressed, 200, 10));

    // Resizing by an edge, with the client told; its surface follows.
    Vector<IntSize> sizes;
    window->on_resize = [&](IntSize size) {
        sizes.append(size);
        window->back_buffer().clear(green);
        window->present();
    };
    client.pointer(649, 400);
    client.pointer(649, 400, Window::ButtonLeft);
    client.settle(30);
    client.pointer(749, 400, Window::ButtonLeft);
    client.settle(30);
    client.pointer(749, 400, 0);
    client.settle();
    report("resizing tells the client", !sizes.is_empty() && sizes.last() == (IntSize { 500, 300 }));
    report("the window knows its size", window->size() == (IntSize { 500, 300 }));
    shot = client.screen();
    report("it draws at the new size", near(shot->color_at(730 * s, 400 * s), green));

    // A second window: a click on one activates and raises it.
    bool first_active = true;
    window->on_activation = [&](bool active) { first_active = active; };
    Color const blue = Color::from_rgb(0x0000ff);
    auto other = filled_window(client, "Other"_sv, { 300, 300, 200, 200 }, Window::WindowTitled, blue);
    client.settle();
    report("a new window takes over", !first_active);
    shot = client.screen();
    report("it is on top", near(shot->color_at(400 * s, 420 * s), blue));
    client.pointer(600, 500);
    client.pointer(600, 500, Window::ButtonLeft);
    client.pointer(600, 500, 0);
    client.settle();
    report("a click activates", first_active);
    shot = client.screen();
    report("and raises", near(shot->color_at(400 * s, 420 * s), green));

    // The traffic lights.
    bool close_asked = false;
    window->on_close = [&] { close_asked = true; };
    // At (250, 250): the first light's center is 14 points in, 14 down.
    auto frame_x = 250, frame_y = 250;
    client.pointer(frame_x + 14, frame_y + 14);
    client.pointer(frame_x + 14, frame_y + 14, Window::ButtonLeft);
    client.settle(30);
    client.pointer(frame_x + 14, frame_y + 14, 0);
    client.settle();
    report("the close button asks the client", close_asked);
    // Pressed there, released elsewhere: nothing.
    close_asked = false;
    client.pointer(frame_x + 14, frame_y + 14);
    client.pointer(frame_x + 14, frame_y + 14, Window::ButtonLeft);
    client.settle(30);
    client.pointer(frame_x + 200, frame_y + 100, Window::ButtonLeft);
    client.pointer(frame_x + 200, frame_y + 100, 0);
    client.settle();
    report("releasing elsewhere does not", !close_asked);
}

void test_materials(Client& client)
{
    int s = client.connection->scale();
    Color const orange = Color::from_rgb(0xff8000);
    auto behind = filled_window(client, "Behind"_sv, { 100, 100, 500, 400 }, Window::WindowTitled, orange);
    auto front = MUST(Window::Window::create(*client.connection, "Front"_sv, { 150, 150, 400, 300 }, Window::WindowTitled));
    // Clear, over a material on its left half; white on the right.
    {
        Painter painter(front->back_buffer());
        painter.fill_rect({ 200 * static_cast<float>(s), 0, 200 * static_cast<float>(s), 300 * static_cast<float>(s) }, Color::from_rgb(0xffffff));
    }
    Window::Window::MaterialRegion const regions[] = { { { 0, 0, 200, 300 }, Window::Material::Sidebar } };
    front->set_materials({ regions, 1 });
    front->present();
    front->wait_until_presented(client.loop);
    auto shot = client.screen();
    auto through = shot->color_at(250 * s, 300 * s);
    report("the opaque part hides what is behind", near(shot->color_at(450 * s, 300 * s), Color::from_rgb(0xffffff)));
    // Orange, tinted toward the material's light grey: warm, not orange itself.
    report("the material shows what is behind", through.red > through.blue + 20 && !near(through, orange, 20));
    // Moving what is behind changes it, with no present from the front window.
    behind->set_frame({ 700, 100, 300, 200 });
    shot = client.screen();
    auto without = shot->color_at(250 * s, 300 * s);
    report("the material follows what moves behind it", !near(through, without, 10));
    // It is smooth: neighbours differ little, there is no grid of tiles.
    int worst = 0;
    for (int y = 200 * s; y < 400 * s; y += 3) {
        auto a = shot->color_at(250 * s, y);
        auto b = shot->color_at(250 * s, y + 1);
        worst = max(worst, a.red > b.red ? a.red - b.red : b.red - a.red);
    }
    report("the blur is smooth", worst <= 6);
}

void test_clients(Client& client)
{
    int s = client.connection->scale();
    Color const magenta = Color::from_rgb(0xff00ff);
    int fds[2];
    (void)pipe(fds);
    pid_t child = fork();
    if (child == 0) {
        // A program of its own: a fresh start, as any other client is.
        char descriptor[16];
        snprintf(descriptor, sizeof(descriptor), "%d", fds[1]);
        execl("/usr/tests/windows", "windows", "--doomed-client", descriptor, nullptr);
        _exit(127);
    }
    char byte;
    (void)read(fds[0], &byte, 1);
    client.settle(100);
    auto shot = client.screen();
    report("another client's window shows", near(shot->color_at(900 * s, 200 * s), magenta));
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    shot = client.screen();
    report("a client's end takes its windows with it", !near(shot->color_at(900 * s, 200 * s), magenta));

    // Those who are not root may not look at the screen.
    child = fork();
    if (child == 0) {
        if (setgid(20) < 0 || setuid(501) < 0)
            _exit(9);
        auto port = IPC::look_up("com.vulture.windowserver"_sv);
        if (port.is_error())
            _exit(8);
        WindowServerProxy server(port.release_value());
        auto answer = server.capture();
        _exit(!answer.is_error() && answer.value().error == EPERM ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    report("only root captures the screen", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// M8's measure: ten windows with shadows, some with blur behind them, one being dragged across the others. In frames
// shown per second. And M21's: twelve, with all of the screen to be drawn again for every one of them.
void test_drag_rate(Client& client, int count, bool whole_screen)
{
    auto size = client.connection->screen_size();
    int s = client.connection->scale();
    char const* label = whole_screen ? "whole screen " : "";
    Vector<NonnullOwnPtr<Window::Window>> windows;
    for (int i = 0; i < count; ++i) {
        IntRect frame { 40 + (i % 5) * (size.width - 620) / 5, 40 + (i / 5) * (size.height - 420) / 2 + (i % 3) * 30, 560, 380 };
        auto window = MUST(Window::Window::create(*client.connection, "Window"_sv, frame, Window::WindowTitled));
        auto& bitmap = window->back_buffer();
        Painter painter(bitmap);
        GradientStop const stops[] = { { 0, Color::from_rgb(0xffffff) }, { 1, Color::from_rgb(0xd0e0ff - static_cast<u32>(i) * 0x101010) } };
        Path all;
        float const sidebar = (i % 2 == 0) ? 180 * static_cast<float>(s) : 0;
        all.add_rect({ sidebar, 0, static_cast<float>(bitmap.width()) - sidebar, static_cast<float>(bitmap.height()) });
        painter.fill_path(all, Gradient::linear({ 0, 0 }, { 0, static_cast<float>(bitmap.height()) }, { stops, 2 }));
        if (i % 2 == 0) {
            Window::Window::MaterialRegion const regions[] = { { { 0, 0, 180, 380 }, Window::Material::Sidebar } };
            window->set_materials({ regions, 1 });
        }
        window->present();
        windows.append(move(window));
    }
    for (auto& window : windows)
        window->wait_until_presented(client.loop);
    client.settle(200);

    // Take the top window by its title bar.
    int const last = count - 1;
    IntRect start { 40 + (last % 5) * (size.width - 620) / 5, 40 + (last / 5) * (size.height - 420) / 2 + (last % 3) * 30, 560, 380 };
    int grab_x = start.x + 280;
    int grab_y = start.y + 10;
    client.pointer(grab_x, grab_y);
    client.pointer(grab_x, grab_y, Window::ButtonLeft);
    client.settle(40);

    (void)client.connection->server().input_latency();
    auto before = MUST(client.connection->server().statistics());
    u64 started = now_microseconds();
    constexpr u64 duration = 5'000'000;
    u64 elapsed = 0;
    while (elapsed < duration) {
        // Back and forth across the others, as a hand would.
        double phase = static_cast<double>(elapsed) / 1'000'000.0;
        int x = grab_x - static_cast<int>(Base::Math::absolute(Base::Math::sine(phase)) * (size.width - 700));
        int y = grab_y - static_cast<int>((1 - Base::Math::cosine(phase * 1.7)) * 120);
        client.pointer(x, max(y, 12), Window::ButtonLeft);
        if (whole_screen)
            (void)client.connection->server().damage_everything();
        client.settle(8);
        elapsed = now_microseconds() - started;
    }
    auto after = MUST(client.connection->server().statistics());
    auto latency = MUST(client.connection->server().input_latency());
    client.pointer(0, 0, 0);
    u64 frames = after.frames - before.frames;
    double seconds = static_cast<double>(elapsed) / 1'000'000.0;
    double rate = static_cast<double>(frames) / seconds;
    u64 compose = frames ? (after.compose_microseconds - before.compose_microseconds) / frames : 0;
    u64 flush = frames ? (after.flush_microseconds - before.flush_microseconds) / frames : 0;
    u64 pixels = frames ? (after.pixels - before.pixels) / frames : 0;
    u64 materials = frames ? (after.material_microseconds - before.material_microseconds) / frames : 0;
    printf("%srate: %d.%d frames per second at %dx%d; per frame %llu us composing, of which %llu us materials, %llu us flushing, %llu pixels; input to flush p99 %d.%d ms, worst %d.%d ms\n", label, static_cast<int>(rate), static_cast<int>(rate * 10) % 10, size.width * s, size.height * s,
        static_cast<unsigned long long>(compose), static_cast<unsigned long long>(materials), static_cast<unsigned long long>(flush), static_cast<unsigned long long>(pixels),
        static_cast<int>(latency.p99_microseconds / 1000), static_cast<int>(latency.p99_microseconds % 1000 / 100), static_cast<int>(latency.worst_microseconds / 1000), static_cast<int>(latency.worst_microseconds % 1000 / 100));
    printf("budget: %sdrag frames: %d.%d fps\n", label, static_cast<int>(rate), static_cast<int>(rate * 10) % 10);
    printf("budget: %sinput to flush p99: %d.%d ms\n", label, static_cast<int>(latency.p99_microseconds / 1000), static_cast<int>(latency.p99_microseconds % 1000 / 100));
    printf("budget: %sinput to flush worst: %d.%d ms\n", label, static_cast<int>(latency.worst_microseconds / 1000), static_cast<int>(latency.worst_microseconds % 1000 / 100));
    printf("budget: %sinput timed: %llu moves\n", label, static_cast<unsigned long long>(latency.count));
    fflush(stdout);
    // (Which isn't something to ask of a whole machine emulated in software.)
    int emulated = 0;
    size_t emulated_size = sizeof(emulated);
    (void)sysctlbyname("machdep.cpu.emulated", &emulated, &emulated_size, nullptr, 0);
    report(whole_screen ? "60 frames per second, all of the screen drawn every time" : "60 frames per second, dragging", rate >= 58 || emulated);
}

NonnullOwnPtr<Window::Layer> filled_layer(Client& client, Window::Window& window, Window::Layer const* parent, IntSize points, Color color)
{
    int const s = client.connection->scale();
    auto layer = MUST(Window::Layer::create(window, parent));
    MUST(layer->set_surface({ points.width * s, points.height * s }));
    layer->back_buffer().clear(color);
    return layer;
}

void test_layers(Client& client)
{
    using Window::LayerProperty;
    int const s = client.connection->scale();
    Color const white = Color::from_rgb(0xffffff);
    Color const red = Color::from_rgb(0xff0000);
    Color const blue = Color::from_rgb(0x0000ff);
    IntRect const frame { 100, 100, 500, 400 };
    auto window = filled_window(client, "Layers"_sv, frame, Window::WindowTitled, white);
    auto at = [&](NonnullRefPtr<Bitmap> const& shot, int x, int y) { return shot->color_at((frame.x + x) * s, (frame.y + y) * s); };

    auto layer = filled_layer(client, *window, nullptr, { 100, 100 }, red);
    {
        Window::LayerTransaction change(*window);
        change.set_frame(*layer, { 100, 100, 100, 100 });
        change.present(*layer, layer->back_buffer().rect());
        u32 const serial = change.commit();
        u32 heard = 0;
        window->on_layers_presented = [&](u32 number) { heard = number; };
        window->wait_until_presented(client.loop);
        report("a commit is answered when it's on the screen", heard == serial);
    }
    auto shot = client.screen();
    report("a layer shows over its window", near(at(shot, 150, 150), red) && near(at(shot, 90, 150), white));

    auto commit = [&](Function<void(Window::LayerTransaction&)> what) {
        Window::LayerTransaction change(*window);
        what(change);
        (void)change.commit();
        return client.screen();
    };

    shot = commit([&](auto& change) { change.set(*layer, LayerProperty::Opacity, 0.5f); });
    report("it fades without being drawn again", near(at(shot, 150, 150), Color::from_rgb(0xff8080), 3));
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::Opacity, 1);
        change.set(*layer, LayerProperty::Translation, 150, 0);
    });
    report("it moves without being drawn again", near(at(shot, 300, 150), red) && near(at(shot, 150, 150), white));
    shot = commit([&](auto& change) { change.set(*layer, LayerProperty::Hidden, 1); });
    report("hidden, it's gone", near(at(shot, 300, 150), white));

    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::Hidden, 0);
        change.set(*layer, LayerProperty::Translation, 0, 0);
        change.set(*layer, LayerProperty::Scale, 2, 2);
    });
    report("it scales about its middle", near(at(shot, 60, 150), red) && near(at(shot, 240, 150), red) && near(at(shot, 30, 150), white));
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::Scale, 1, 1);
        change.set(*layer, LayerProperty::Rotation, 3.14159265f / 4);
    });
    report("it turns about its middle", near(at(shot, 150, 150), red) && near(at(shot, 105, 105), white) && near(at(shot, 150, 215), red));
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::Rotation, 0);
        change.set(*layer, LayerProperty::CornerRadius, 50);
    });
    report("its corners round off", near(at(shot, 150, 150), red) && near(at(shot, 103, 103), white));

    // What's in it: it goes with it, stops at its edge if it says so, and fades with it as one.
    auto inner = filled_layer(client, *window, layer.ptr(), { 60, 60 }, blue);
    auto other = filled_layer(client, *window, layer.ptr(), { 60, 60 }, blue);
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::CornerRadius, 0);
        change.set_frame(*inner, { 20, 20, 60, 60 });
        change.present(*inner, inner->back_buffer().rect());
        change.set_frame(*other, { 80, 20, 60, 60 });
        change.present(*other, other->back_buffer().rect());
    });
    report("what's in a layer goes over it", near(at(shot, 150, 150), blue));
    report("and past its edge, without being told", near(at(shot, 230, 150), blue));
    shot = commit([&](auto& change) { change.set(*layer, LayerProperty::Clips, 1); });
    report("and stops there, if it is", near(at(shot, 230, 150), white) && near(at(shot, 190, 150), blue));
    shot = commit([&](auto& change) { change.set(*layer, LayerProperty::BoundsOrigin, 0, 30); });
    report("what's in it scrolls", near(at(shot, 140, 135), blue) && near(at(shot, 140, 175), white));
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::BoundsOrigin, 0, 0);
        change.set_frame(*other, { 40, 20, 60, 60 });
        change.set(*layer, LayerProperty::Opacity, 0.5f);
    });
    report("it fades as one, not a part at a time", near(at(shot, 130, 150), at(shot, 150, 150), 2) && !near(at(shot, 150, 150), blue, 40));
    shot = commit([&](auto& change) {
        change.set(*layer, LayerProperty::Opacity, 1);
        change.destroy(*inner);
        change.destroy(*other);
    });
    report("gone, and what was in it too", near(at(shot, 150, 150), red));

    // Of a color, with no pixels; and a blur of what's behind.
    auto plain = MUST(Window::Layer::create(*window));
    shot = commit([&](auto& change) {
        change.set_frame(*plain, { 300, 200, 100, 100 });
        change.set(*plain, LayerProperty::Background, 0, 0, 1, 1);
    });
    report("a layer of a color", near(at(shot, 350, 250), blue));
    shot = commit([&](auto& change) {
        change.set(*plain, LayerProperty::Background, 0, 0, 0, 0);
        change.set(*plain, LayerProperty::Material, static_cast<float>(Window::Material::Sidebar));
    });
    auto const material = at(shot, 350, 250);
    report("a material behind it", !near(material, blue, 20) && !near(material, Color { 0, 0, 0, 255 }, 20));

    // Told how, and left to it.
    bool finished = false;
    window->on_layer_animation_finished = [&](u32, u32 tag) { finished = tag == 77; };
    float const from[] = { 1 };
    float const to[] = { 0 };
    Window::LayerTransaction change(*window);
    change.animate(*layer, LayerProperty::Opacity, { from, 1 }, { to, 1 }, Animation::Timing::ease_out(0.2f), 77);
    (void)change.commit();
    client.settle(400);
    report("an animation says when it's finished", finished);
    report("and ends where it said", near(client.screen()->color_at((frame.x + 150) * s, (frame.y + 150) * s), white));
    window->on_layer_animation_finished = nullptr;
    window->on_layers_presented = nullptr;
}

// A layer on a spring, and then nothing: it stops itself, and the server's left to it.
int run_frozen_client(int ready)
{
    using Window::LayerProperty;
    Client other;
    other.connection = MUST(Window::Connection::connect(other.loop));
    (void)other.connection->server().set_application("Frozen"_sv, "com.vulture.test.frozen"_sv);
    auto window = filled_window(other, "Frozen"_sv, { 600, 300, 400, 300 }, Window::WindowTitled | Window::WindowResizable, Color::from_rgb(0xffffff));
    auto layer = filled_layer(other, *window, nullptr, { 50, 50 }, Color::from_rgb(0xff0000));
    Window::LayerTransaction change(*window);
    change.set_frame(*layer, { 20, 60, 50, 50 });
    change.present(*layer, layer->back_buffer().rect());
    change.set(*layer, LayerProperty::Logged, 1);
    float const from[] = { 0, 0 };
    float const to[] = { 300, 0 };
    change.animate(*layer, LayerProperty::Translation, { from, 2 }, { to, 2 }, Animation::Timing::spring_with(1.2f, 0.3f));
    (void)change.commit();
    u32 const ids[2] = { window->id(), layer->id() };
    (void)write(ready, ids, sizeof(ids));
    raise(SIGSTOP);
    for (;;)
        other.settle(1000);
}

void test_frozen_program(Client& client)
{
    int fds[2];
    (void)pipe(fds);
    char path[32];
    snprintf(path, sizeof(path), "%d", fds[1]);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        execl("/usr/tests/windows", "windows", "--frozen-client", path, nullptr);
        _exit(127);
    }
    close(fds[1]);
    u32 ids[2] = {};
    bool const told = read(fds[0], ids, sizeof(ids)) == static_cast<ssize_t>(sizeof(ids));
    close(fds[0]);
    int status = 0;
    bool const stopped = told && waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status);
    report("a program can be stopped with its animation running", stopped);
    client.settle(1100);
    auto answer = MUST(client.connection->server().layer_frame_log(ids[0], ids[1]));

    // It hasn't been asked for anything yet, so nothing's known to be wrong. Move over it, and it has.
    String state;
    client.connection->on_shell_state = [&](StringView json) { state = MUST(String::from_view(json)); };
    (void)client.connection->server().subscribe_shell();
    client.pointer(800, 500);
    client.settle(200);
    report("asked for nothing, nothing's wrong", MUST(client.connection->server().cursor_shape()).shape != static_cast<i32>(Window::CursorShape::Wait));
    client.settle(2300);
    auto const waiting = MUST(client.connection->server().cursor_shape());
    report("two seconds on, the cursor over it spins", waiting.shape == static_cast<i32>(Window::CursorShape::Wait));
    client.settle(300);
    auto const later = MUST(client.connection->server().cursor_shape());
    report("and goes round", later.turn != waiting.turn);
    report("and it's said to be not responding", strstr(state.characters(), "\"notResponding\":true") != nullptr);
    // Elsewhere, the pointer's as it was.
    client.pointer(100, 100);
    client.settle(100);
    report("away from it, the cursor's as it was", MUST(client.connection->server().cursor_shape()).shape != static_cast<i32>(Window::CursorShape::Wait));
    // Its windows can still be moved about and resized: that's all done here. It takes a moment to give up waiting for it.
    auto const shot_before = client.screen();
    int const s = client.connection->scale();
    report("its window isn't there yet", near(shot_before->color_at(1040 * s, 630 * s), shot_before->color_at(1100 * s, 700 * s), 40));
    client.pointer(999, 599);
    client.settle(40);
    client.pointer(999, 599, Window::ButtonLeft);
    client.settle(40);
    client.pointer(1060, 650, Window::ButtonLeft);
    client.settle(300);
    client.pointer(1060, 650);
    client.settle(100);
    auto const shot_after = client.screen();
    report("its window can still be resized", !near(shot_after->color_at(1040 * s, 630 * s), shot_before->color_at(1040 * s, 630 * s), 2));
    client.pointer(800, 500);
    client.settle(100);
    kill(child, SIGCONT);
    client.settle(500);
    report("when it answers, it stops", MUST(client.connection->server().cursor_shape()).shape != static_cast<i32>(Window::CursorShape::Wait) && strstr(state.characters(), "\"notResponding\":true") == nullptr);
    client.pointer(0, 0);
    client.connection->on_shell_state = nullptr;

    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    struct Entry {
        double seconds;
        float values[4];
    };
    size_t const count = answer.log.size() / sizeof(Entry);
    auto const model = Animation::Timing::spring_with(1.2f, 0.3f);
    float worst = 0;
    double last = -1;
    double longest_gap = 0;
    for (size_t i = 0; i < count; ++i) {
        Entry entry;
        memcpy(&entry, answer.log.data() + i * sizeof(Entry), sizeof(Entry));
        float const expected = 300 * model.value_at(static_cast<float>(entry.seconds));
        worst = max(worst, Base::Math::absolute(entry.values[0] - expected));
        if (last >= 0)
            longest_gap = max(longest_gap, entry.seconds - last);
        last = entry.seconds;
    }
    printf("frozen: %zu frames over %d ms, worst %d.%02d points from the model, longest gap %d ms\n", count, static_cast<int>(last * 1000), static_cast<int>(worst), static_cast<int>(worst * 100) % 100, static_cast<int>(longest_gap * 1000));
    fflush(stdout);
    int emulated = 0;
    size_t emulated_size = sizeof(emulated);
    (void)sysctlbyname("machdep.cpu.emulated", &emulated, &emulated_size, nullptr, 0);
    report("the spring goes on, with its program stopped", last > 0.9 && (emulated || count >= 50));
    report("every frame of it within a pixel of the model", count > 0 && worst <= 1);
}

// A program that didn't come with the system, for these purposes: the same one, with a byte more, and a hash the kernel's been told is fine.
bool make_installed_copy(char const* to)
{
    int in = open("/usr/tests/windows", O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return false;
    Crypto::SHA256 hasher;
    static u8 buffer[64 * 1024];
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        hasher.update({ buffer, static_cast<size_t>(got) });
        if (write(out, buffer, static_cast<size_t>(got)) != got)
            return false;
    }
    u8 const extra = 0x5a;
    hasher.update({ &extra, 1 });
    if (write(out, &extra, 1) != 1)
        return false;
    close(in);
    close(out);
    auto const hash = hasher.digest();
    return sysctlbyname("security.code.trust", nullptr, nullptr, hash.data(), hash.size()) == 0;
}

// As that. Exits with 0 for a picture, 1 for being refused, 2 for anything else.
int run_looking_client()
{
    if (setgid(20) < 0 || setuid(501) < 0)
        return 2;
    Client other;
    auto connection = Window::Connection::connect(other.loop);
    if (connection.is_error())
        return 2;
    other.connection = connection.release_value();
    // Who's logged in, as far as this server's concerned: this one's the first to say.
    (void)other.connection->server().subscribe_shell();
    auto picture = other.connection->capture_screen();
    if (!picture.is_error())
        return 0;
    return picture.error().is_errno() && picture.error().code() == EPERM ? 1 : 2;
}

void test_consent(Client& client)
{
    // Whoever's logged in, as far as the server knows.
    constexpr char const* copy = "/tmp/windows-test/looker";
    report("an installed program to try it with", make_installed_copy(copy));
    auto run = [&] {
        fflush(stdout);
        pid_t const child = fork();
        if (child == 0) {
            execl(copy, copy, "--looking-client", nullptr);
            _exit(99);
        }
        return child;
    };
    // With nobody to ask, it's no.
    int status = 0;
    pid_t child = run();
    for (int waited = 0; waited < 400 && waitpid(child, &status, WNOHANG) == 0; ++waited)
        client.settle(25);
    report("with nobody to ask, looking at the screen is refused", WIFEXITED(status) && WEXITSTATUS(status) == 1);

    // With someone to ask, as privacyd would.
    int const device = open("/dev/privacy", O_RDWR | O_CLOEXEC);
    report("something to answer as", device >= 0);
    if (device < 0)
        return;
    auto answered_with = [&](bool allowed) {
        pid_t const asker = run();
        vulture_privacy_request request {};
        pollfd poller { device, POLLIN, 0 };
        bool asked = false;
        for (int waited = 0; waited < 200 && !asked; ++waited) {
            client.settle(25);
            if (poll(&poller, 1, 0) == 1)
                asked = read(device, &request, sizeof(request)) == sizeof(request);
        }
        if (asked) {
            vulture_privacy_answer const reply { request.id, allowed ? 1u : 0u };
            (void)write(device, &reply, sizeof(reply));
        }
        for (int waited = 0; waited < 400 && waitpid(asker, &status, WNOHANG) == 0; ++waited)
            client.settle(25);
        return asked && request.service == VULTURE_PRIVACY_SCREEN;
    };
    report("it's asked about the screen", answered_with(false));
    report("told no, it's refused", WIFEXITED(status) && WEXITSTATUS(status) == 1);
    bool const asked = answered_with(true);
    report("told yes, it can look", asked && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(device);

    // Root can still do what tests need, and asks nobody.
    report("and root doesn't have to ask", !client.connection->capture_screen().is_error());
}

void test_screencapture(Client& client)
{
    constexpr char const* file = "/tmp/windows-test/shot.png";
    int const s = client.connection->scale();
    report("screencapture takes part of the screen", system("/usr/sbin/screencapture -x -R 100,100,50,40 /tmp/windows-test/shot.png") == 0);
    int const fd = open(file, O_RDONLY);
    static u8 bytes[256 * 1024];
    ssize_t const got = fd >= 0 ? read(fd, bytes, sizeof(bytes)) : -1;
    if (fd >= 0)
        close(fd);
    auto const picture = got > 0 ? Image::decode_png({ bytes, static_cast<size_t>(got) }) : ErrorOr<NonnullRefPtr<Bitmap>>(Error::from_errno(EIO));
    report("of the size it was asked for", !picture.is_error() && picture.value()->width() == 50 * s && picture.value()->height() == 40 * s);
    report("and a display that isn't there is an error", system("/usr/sbin/screencapture -D 9 /tmp/windows-test/none.png 2>/dev/null") != 0);
    // A recording: frames as they come, once each.
    client.settle(100);
    report("screencapture records", system("/usr/sbin/screencapture -V 1 -R 0,0,100,100 /tmp/windows-test/frames > /tmp/windows-test/frames.out") == 0);
    int frames = 0;
    sscanf(read_file("frames.out"), "%d", &frames);
    printf("frames: %d in a second\n", frames);
    report("and there's a frame to look at", access("/tmp/windows-test/frames-0001.ppm", R_OK) == 0 && frames >= 1);
}

void test_reduced_motion(Client& client)
{
    int const s = client.connection->scale();
    auto const before = client.screen();
    auto window = filled_window(client, "Motion"_sv, { 200, 200, 300, 200 }, Window::WindowTitled, Color::from_rgb(0x00a000));
    client.settle(100);
    bool told = false;
    bool reduced = false;
    client.connection->on_motion_change = [&](bool value) {
        told = true;
        reduced = value;
    };
    (void)client.connection->server().set_reduce_motion(true);
    client.settle(100);
    report("everyone hears that motion's reduced", told && reduced);
    auto const frames_before = MUST(client.connection->server().statistics()).frames;
    window->minimize();
    // The very next frame, whenever that is, with no window going anywhere on its way down to the Dock.
    while (MUST(client.connection->server().statistics()).frames == frames_before)
        client.settle(1);
    auto const answer = MUST(client.connection->server().capture());
    auto const shot = MUST(Image::decode_png(answer.png.span()));
    report("minimizing happens at once", near(shot->color_at(350 * s, 300 * s), before->color_at(350 * s, 300 * s), 8) && near(shot->color_at(350 * s, 450 * s), before->color_at(350 * s, 450 * s), 8));
    (void)client.connection->server().set_reduce_motion(false);
    client.settle(100);
    report("and when it isn't", told && !reduced);
    client.connection->on_motion_change = nullptr;
    window->activate();
}

// Another client, soon killed: it says when its window is up.
int run_doomed_client(int ready)
{
    Client other;
    other.connection = MUST(Window::Connection::connect(other.loop));
    auto window = filled_window(other, "Doomed"_sv, { 800, 100, 200, 200 }, Window::WindowTitled, Color::from_rgb(0xff00ff));
    (void)write(ready, "x", 1);
    for (;;)
        other.settle(1000);
}

int run_client()
{
    Client client;
    auto connection = Window::Connection::connect(client.loop);
    report("connecting", !connection.is_error());
    if (connection.is_error())
        return 1;
    client.connection = connection.release_value();
    test_basics(client);
    test_pointer(client);
    test_materials(client);
    test_clients(client);
    test_layers(client);
    test_frozen_program(client);
    test_reduced_motion(client);
    test_consent(client);
    test_screencapture(client);
    test_drag_rate(client, 10, false);
    test_drag_rate(client, 12, true);
    printf("done\n");
    fflush(stdout);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--client") == 0)
        return run_client();
    if (argc > 2 && strcmp(argv[1], "--doomed-client") == 0)
        return run_doomed_client(atoi(argv[2]));
    if (argc > 1 && strcmp(argv[1], "--looking-client") == 0)
        return run_looking_client();
    if (argc > 2 && strcmp(argv[1], "--frozen-client") == 0)
        return run_frozen_client(atoi(argv[2]));
    // Only the drag, on the window server that's there: see Meta/budgets.sh.
    if (argc > 1 && strcmp(argv[1], "--budget") == 0) {
        Client client;
        client.connection = MUST(Window::Connection::connect(client.loop));
        test_drag_rate(client, 10, false);
    test_drag_rate(client, 12, true);
        return 0;
    }

    system("rm -rf /tmp/windows-test");
    mkdir(s_directory, 0755);
    write_file("windowserver.plist", R"({
        "Label" : "com.vulture.windowserver",
        "ProgramArguments" : [ "/System/Library/CoreServices/WindowServer", "--no-input" ],
        "MachServices" : { "com.vulture.windowserver" : true },
        "RunAtLoad" : true,
        "StandardErrorPath" : "/tmp/windows-test/server.err"
    })");
    write_file("client.plist", R"({
        "Label" : "com.vulture.test.windows",
        "ProgramArguments" : [ "/usr/tests/windows", "--client" ],
        "RunAtLoad" : true,
        "StandardOutPath" : "/tmp/windows-test/client.out",
        "StandardErrorPath" : "/tmp/windows-test/client.out"
    })");

    pid_t launchd = fork();
    if (launchd == 0) {
        char const* arguments[] = { "vlaunchd", "--log", "/tmp/windows-test/log", "--directory", s_directory, nullptr };
        execv("/sbin/vlaunchd", const_cast<char**>(arguments));
        _exit(127);
    }
    // (Capturing the screen, encoding and decoding it, is the slow part.)
    for (int waited = 0; waited < 2400 && !strstr(read_file("client.out"), "done"); ++waited) {
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
        fprintf(stderr, "windows: the client said:\n%s\nthe server said:\n%s\nvlaunchd's log:\n%s", report_text, read_file("server.err"), read_file("log"));
    if (char const* frozen = strstr(report_text, "frozen: ")) {
        char const* end = strchr(frozen, '\n');
        printf("windows: %.*s\n", static_cast<int>(end ? end - frozen : static_cast<long>(strlen(frozen))), frozen);
    }
    // What's been shown to work, for the ledger to go by.
    for (char const* line = strstr(report_text, "ok: "); line; line = strstr(line + 1, "ok: ")) {
        char const* end = strchr(line, '\n');
        printf("windows: %.*s\n", static_cast<int>(end ? end - line : static_cast<long>(strlen(line))), line);
    }
    // Each measure, with what it's of ("whole screen rate: ...").
    for (char const* rate = strstr(report_text, "rate: "); rate; rate = strstr(rate + 1, "rate: ")) {
        char const* begin = rate;
        while (begin > report_text && begin[-1] != '\n')
            --begin;
        char const* end = strchr(rate, '\n');
        printf("windows: %.*s\n", static_cast<int>(end ? end - begin : static_cast<long>(strlen(begin))), begin);
    }
    system("rm -rf /tmp/windows-test");
    return finish();
}
