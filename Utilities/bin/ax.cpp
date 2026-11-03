/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibJSON/JSON.h>
#include <LibWindow/Window.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// ax: what's on the screen, in words, as the applications say it is. Only root may ask.
//
//   ax                         every window, front to back, and what's in it
//   ax find ROLE LABEL         where the first such thing is (ROLE may be "*"), or a failure status if there's none
//   ax --serve                 for tests: answers one request a line, on standard input and output
//
// A test on the host talks to --serve over the serial console, so it can find things by what they say rather than where
// they are, and check what they are saying. It then presses them with the real pointer: this only ever looks.
//
//   @ax ID find ROLE LABEL     @ax ID found X Y WIDTH HEIGHT SCREEN_WIDTH SCREEN_HEIGHT ENABLED FOCUSED VALUE
//                              (points on the screen; ENABLED and FOCUSED are 0 or 1), or @ax ID none
//   @ax ID dump                @ax ID tree LINE for each line of what "ax" prints, then @ax ID end
//
// Anything else on the line is left alone: the console has the kernel's and everyone else's messages on it too.

namespace {

struct Screen {
    int width { 0 };
    int height { 0 };
    JSON::Value windows;
    // What each application answered, a list of { "window", "tree" }.
    Vector<JSON::Value> answers;
};

// How long a slow application gets. One that's stuck doesn't get to hold everyone up, and its windows have no tree.
constexpr int answer_attempts = 25;
constexpr useconds_t answer_interval = 20'000;

ErrorOr<Screen> look()
{
    Core::EventLoop loop;
    auto connection = TRY(Window::Connection::connect(loop));
    auto request = TRY(connection->server().request_accessibility());
    if (request.error != 0)
        return Error::from_errno(request.error);
    WindowServerEndpoint::AccessibilityAnswer answer {};
    for (int attempt = 0; attempt < answer_attempts; ++attempt) {
        answer = TRY(connection->server().accessibility(request.generation));
        if (answer.error != 0)
            return Error::from_errno(answer.error);
        if (answer.answered >= answer.asked)
            break;
        usleep(answer_interval);
    }
    auto state = TRY(JSON::parse(answer.json.view()));
    Screen screen;
    screen.width = static_cast<int>(state.get_integer("width"_sv).value_or(0));
    screen.height = static_cast<int>(state.get_integer("height"_sv).value_or(0));
    if (auto const* windows = state.get_array("windows"_sv))
        screen.windows = *windows;
    if (auto const* answers = state.get_array("answers"_sv)) {
        for (size_t i = 0; i < answers->size(); ++i) {
            auto parsed = JSON::parse(answers->at(i).as_string());
            if (!parsed.is_error())
                screen.answers.append(parsed.release_value());
        }
    }
    return screen;
}

// What an application said about this window.
JSON::Value const* tree_of(Screen const& screen, i64 window)
{
    for (auto const& answer : screen.answers) {
        for (size_t i = 0; i < answer.size(); ++i) {
            if (answer.at(i).get_integer("window"_sv).value_or(-1) == window)
                return answer.at(i).get("tree"_sv);
        }
    }
    return nullptr;
}

JSON::Value const* find_in(JSON::Value const& node, StringView role, StringView label)
{
    bool const role_matches = role == "*"_sv || node.get_string("role"_sv).value_or(StringView {}) == role;
    if (role_matches && node.get_string("label"_sv).value_or(StringView {}) == label)
        return &node;
    if (auto const* children = node.get_array("children"_sv)) {
        for (size_t i = 0; i < children->size(); ++i) {
            if (auto const* found = find_in(children->at(i), role, label))
                return found;
        }
    }
    return nullptr;
}

struct Found {
    int x { 0 };
    int y { 0 };
    int width { 0 };
    int height { 0 };
    bool enabled { true };
    bool focused { false };
    String value;
};

// In the frontmost window that has one.
Optional<Found> find(Screen const& screen, StringView role, StringView label)
{
    for (size_t i = 0; i < screen.windows.size(); ++i) {
        auto const& window = screen.windows.at(i);
        auto const* tree = tree_of(screen, window.get_integer("id"_sv).value_or(-1));
        if (!tree)
            continue;
        auto const* node = find_in(*tree, role, label);
        if (!node)
            continue;
        Found found;
        found.x = static_cast<int>(window.get_integer("x"_sv).value_or(0) + static_cast<i64>(node->get_number("x"_sv).value_or(0)));
        found.y = static_cast<int>(window.get_integer("y"_sv).value_or(0) + static_cast<i64>(node->get_number("y"_sv).value_or(0)));
        found.width = static_cast<int>(node->get_number("width"_sv).value_or(0));
        found.height = static_cast<int>(node->get_number("height"_sv).value_or(0));
        found.enabled = node->get_boolean("enabled"_sv).value_or(true);
        found.focused = node->get_boolean("focused"_sv).value_or(false);
        found.value = MUST(String::from_view(node->get_string("value"_sv).value_or(StringView {})));
        return found;
    }
    return {};
}

// Whoever's listening is reading lines: a value can't be allowed to end one.
void print_flattened(FILE* out, StringView text)
{
    for (size_t i = 0; i < text.length(); ++i)
        fputc(text[i] == '\n' || text[i] == '\r' ? ' ' : text[i], out);
}

void print_node(FILE* out, char const* prefix, JSON::Value const& node, int depth)
{
    fputs(prefix, out);
    for (int i = 0; i < depth; ++i)
        fputs("  ", out);
    print_flattened(out, node.get_string("role"_sv).value_or(StringView {}));
    auto const label = node.get_string("label"_sv).value_or(StringView {});
    if (!label.is_empty()) {
        fputs(" \"", out);
        print_flattened(out, label);
        fputs("\"", out);
    }
    auto const value = node.get_string("value"_sv).value_or(StringView {});
    if (!value.is_empty()) {
        fputs(" = ", out);
        print_flattened(out, value);
    }
    if (!node.get_boolean("enabled"_sv).value_or(true))
        fputs(" (disabled)", out);
    if (node.get_boolean("focused"_sv).value_or(false))
        fputs(" (focused)", out);
    fprintf(out, " [%d,%d %dx%d]\n", static_cast<int>(node.get_number("x"_sv).value_or(0)), static_cast<int>(node.get_number("y"_sv).value_or(0)), static_cast<int>(node.get_number("width"_sv).value_or(0)), static_cast<int>(node.get_number("height"_sv).value_or(0)));
    if (auto const* children = node.get_array("children"_sv)) {
        for (size_t i = 0; i < children->size(); ++i)
            print_node(out, prefix, children->at(i), depth + 1);
    }
}

void print_screen(FILE* out, char const* prefix, Screen const& screen)
{
    fprintf(out, "%sscreen %dx%d\n", prefix, screen.width, screen.height);
    for (size_t i = 0; i < screen.windows.size(); ++i) {
        auto const& window = screen.windows.at(i);
        fprintf(out, "%swindow %d of ", prefix, static_cast<int>(window.get_integer("id"_sv).value_or(0)));
        print_flattened(out, window.get_string("application"_sv).value_or("(not an application)"_sv));
        fprintf(out, " at %d,%d %dx%d%s\n", static_cast<int>(window.get_integer("x"_sv).value_or(0)), static_cast<int>(window.get_integer("y"_sv).value_or(0)), static_cast<int>(window.get_integer("width"_sv).value_or(0)), static_cast<int>(window.get_integer("height"_sv).value_or(0)), window.get_boolean("active"_sv).value_or(false) ? " (active)" : "");
        auto const* tree = tree_of(screen, window.get_integer("id"_sv).value_or(-1));
        if (tree)
            print_node(out, prefix, *tree, 1);
        else
            fprintf(out, "%s  (it hasn't said)\n", prefix);
    }
}

// "ROLE LABEL" into its two parts.
bool split_query(StringView query, StringView& role, StringView& label)
{
    auto const space = query.find(' ');
    if (!space.has_value())
        return false;
    role = query.substring_view(0, space.value());
    label = query.substring_view(space.value() + 1);
    return true;
}

void answer_line(StringView line)
{
    auto const start = line.find("@ax "_sv);
    if (!start.has_value())
        return;
    auto request = line.substring_view(start.value() + 4).trim_whitespace();
    auto const after_id = request.find(' ');
    if (!after_id.has_value())
        return;
    auto const id = request.substring_view(0, after_id.value());
    auto const command = request.substring_view(after_id.value() + 1);
    auto say = [&](auto&&... parts) {
        // One write: other programs are saying things on this console too.
        char buffer[8192];
        int const length = snprintf(buffer, sizeof(buffer), parts...);
        if (length > 0)
            (void)!write(STDOUT_FILENO, buffer, min(static_cast<size_t>(length), sizeof(buffer) - 1));
    };
    auto const id_string = MUST(String::from_view(id));
    auto screen = look();
    if (screen.is_error()) {
        say("@ax %s error %s\n", id_string.characters(), screen.error().is_errno() ? strerror(screen.error().code()) : "no answer");
        return;
    }
    if (command == "dump"_sv) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "@ax %s tree ", id_string.characters());
        print_screen(stdout, prefix, screen.value());
        fflush(stdout);
        say("@ax %s end\n", id_string.characters());
        return;
    }
    if (!command.starts_with("find "_sv)) {
        say("@ax %s error unknown request\n", id_string.characters());
        return;
    }
    StringView role, label;
    if (!split_query(command.substring_view(5), role, label)) {
        say("@ax %s error bad request\n", id_string.characters());
        return;
    }
    auto const found = find(screen.value(), role, label);
    if (!found.has_value()) {
        say("@ax %s none\n", id_string.characters());
        return;
    }
    // One line, whatever it's a value of: a terminal's is its whole screen.
    StringBuilder value;
    for (char character : found->value.view())
        value.append(character == '\n' || character == '\r' ? ' ' : character);
    auto const flattened = MUST(value.to_string());
    say("@ax %s found %d %d %d %d %d %d %d %d %s\n", id_string.characters(), found->x, found->y, found->width, found->height, screen.value().width, screen.value().height, found->enabled ? 1 : 0, found->focused ? 1 : 0, flattened.characters());
}

int serve()
{
    setvbuf(stdout, nullptr, _IOFBF, 16384);
    // The host's own words shouldn't come back to it.
    termios settings {};
    if (tcgetattr(STDIN_FILENO, &settings) == 0) {
        settings.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHOE | ECHOK | ECHONL));
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &settings);
    }
    Vector<char> line;
    char chunk[512];
    for (;;) {
        ssize_t const got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return 0;
        for (ssize_t i = 0; i < got; ++i) {
            if (chunk[i] != '\n' && chunk[i] != '\r') {
                line.append(chunk[i]);
                continue;
            }
            if (!line.is_empty())
                answer_line(StringView(line.data(), line.size()));
            line.clear();
        }
    }
}

}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--serve") == 0)
        return serve();
    if (argc != 1 && !(argc == 4 && strcmp(argv[1], "find") == 0)) {
        fprintf(stderr, "usage: ax\n       ax find ROLE LABEL\n       ax --serve\n");
        return 2;
    }
    auto screen = look();
    if (screen.is_error()) {
        fprintf(stderr, "ax: %s\n", screen.error().is_errno() ? strerror(screen.error().code()) : "no answer");
        return 1;
    }
    if (argc == 1) {
        print_screen(stdout, "", screen.value());
        return 0;
    }
    auto const found = find(screen.value(), StringView(argv[2]), StringView(argv[3]));
    if (!found.has_value())
        return 1;
    printf("%d %d %d %d%s%s%s%s\n", found->x, found->y, found->width, found->height, found->enabled ? "" : " (disabled)", found->focused ? " (focused)" : "", found->value.is_empty() ? "" : " = ", found->value.characters());
    return 0;
}
