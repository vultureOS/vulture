/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <PasteboardEndpoint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// pbcopy [-pboard general|ruler|find|font]: standard input onto the
// pasteboard, as text.
// pbpaste [-pboard ...] [-Prefer txt|rtf|ps]: the pasteboard's text to
// standard output (nothing if there is none). One program, by name.

namespace {

bool is_board(char const* name)
{
    return strcmp(name, "general") == 0 || strcmp(name, "ruler") == 0 || strcmp(name, "find") == 0 || strcmp(name, "font") == 0;
}

constexpr StringView text_type = "public.utf8-plain-text"_sv;

PasteboardProxy* connect(char const* tool)
{
    auto port = IPC::look_up("com.vulture.pasteboard"_sv);
    if (port.is_error()) {
        fprintf(stderr, "%s: could not connect to the pasteboard: %s\n", tool, strerror(port.error().code()));
        return nullptr;
    }
    return new PasteboardProxy(port.release_value());
}

int copy(char const* board)
{
    Vector<u8> data;
    u8 buffer[4096];
    ssize_t got;
    while ((got = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < got; ++i) {
            if (data.try_append(buffer[i]).is_error()) {
                fprintf(stderr, "pbcopy: out of memory\n");
                return 1;
            }
        }
    }
    if (got < 0) {
        perror("pbcopy: read");
        return 1;
    }
    auto* pasteboard = connect("pbcopy");
    if (!pasteboard)
        return 1;
    auto answer = pasteboard->set(StringView(board), text_type, data.span());
    int error = answer.is_error() ? answer.error().code() : answer.value().error;
    if (error) {
        fprintf(stderr, "pbcopy: %s\n", strerror(error));
        return 1;
    }
    return 0;
}

int paste(char const* board)
{
    auto* pasteboard = connect("pbpaste");
    if (!pasteboard)
        return 1;
    auto answer = pasteboard->get(StringView(board), text_type);
    if (answer.is_error()) {
        fprintf(stderr, "pbpaste: %s\n", strerror(answer.error().code()));
        return 1;
    }
    auto const& data = answer.value().data;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t wrote = write(STDOUT_FILENO, data.data() + done, data.size() - done);
        if (wrote <= 0)
            return 1;
        done += static_cast<size_t>(wrote);
    }
    return 0;
}

}

int main(int argc, char** argv)
{
    char const* name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    bool is_paste = strcmp(name, "pbpaste") == 0;
    char const* board = "general";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-pboard") == 0 && i + 1 < argc && is_board(argv[i + 1])) {
            board = argv[++i];
        } else if (is_paste && strcmp(argv[i], "-Prefer") == 0 && i + 1 < argc) {
            ++i; // Text is all there is.
        } else {
            if (is_paste)
                fprintf(stderr, "usage: pbpaste [-help] [-pboard {general | ruler | find | font}] [-Prefer {txt | rtf | ps}]\n");
            else
                fprintf(stderr, "usage: pbcopy [-help] [-pboard {general | ruler | find | font}]\n");
            return strcmp(argv[i], "-help") == 0 ? 0 : 1;
        }
    }
    return is_paste ? paste(board) : copy(board);
}
