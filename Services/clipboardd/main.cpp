/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <PasteboardEndpoint.h>
#include <errno.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// pboard: the pasteboard (com.vulture.pasteboard). Boards are per user
// (the kernel says who is asking), so one user never sees what another
// copied. Each holds one item of each type it was given at once; every
// change counts, as NSPasteboard's changeCount does.

namespace {

constexpr size_t max_item_size = 64 * 1024 * 1024;
constexpr size_t max_boards = 64;

struct Item {
    String type;
    Vector<u8> data;
};

struct Board {
    uid_t uid { 0 };
    String name;
    Vector<Item> items;
    u64 change_count { 0 };
};

class PasteboardService final : public PasteboardStub {
public:
    PasteboardService()
        : m_log(os_log_create("com.vulture.pasteboard", "server"))
    {
    }

    PasteboardEndpoint::SetAnswer set(IPC::Sender const& sender, String board_name, String type, Vector<u8> data) override
    {
        PasteboardEndpoint::SetAnswer answer;
        if (type.is_empty() || data.size() > max_item_size) {
            answer.error = EINVAL;
            return answer;
        }
        auto* board = find(sender.uid, board_name.view(), true);
        if (!board) {
            answer.error = ENOMEM;
            return answer;
        }
        board->items.clear();
        size_t size = data.size();
        board->items.append({ move(type), move(data) });
        answer.change_count = ++board->change_count;
        os_log_info(m_log, "%d (uid %u) copied %zu bytes of %s to %s", sender.pid, sender.uid, size, board->items[0].type.characters(),
            board->name.characters());
        return answer;
    }

    PasteboardEndpoint::GetAnswer get(IPC::Sender const& sender, String board_name, String type) override
    {
        PasteboardEndpoint::GetAnswer answer;
        if (auto* board = find(sender.uid, board_name.view(), false)) {
            for (auto const& item : board->items) {
                if (item.type == type) {
                    answer.found = true;
                    answer.data = item.data;
                    break;
                }
            }
        }
        os_log_debug(m_log, "%d (uid %u) pasted %s from %s: %s", sender.pid, sender.uid, type.characters(), board_name.characters(),
            answer.found ? "found" : "not there");
        return answer;
    }

    Vector<String> types(IPC::Sender const& sender, String board_name) override
    {
        Vector<String> types;
        if (auto* board = find(sender.uid, board_name.view(), false)) {
            for (auto const& item : board->items)
                types.append(item.type);
        }
        return types;
    }

    u64 clear(IPC::Sender const& sender, String board_name) override
    {
        auto* board = find(sender.uid, board_name.view(), true);
        if (!board)
            return 0;
        board->items.clear();
        return ++board->change_count;
    }

    u64 change_count(IPC::Sender const& sender, String board_name) override
    {
        auto* board = find(sender.uid, board_name.view(), false);
        return board ? board->change_count : 0;
    }

private:
    Board* find(uid_t uid, StringView name, bool create)
    {
        if (name.is_empty())
            name = "general"_sv;
        for (auto& board : m_boards) {
            if (board.uid == uid && board.name == name)
                return &board;
        }
        if (!create || m_boards.size() >= max_boards)
            return nullptr;
        m_boards.append({ uid, MUST(String::from_view(name)), {}, 0 });
        return &m_boards.last();
    }

    os_log_t m_log;
    Vector<Board> m_boards;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.pasteboard"_sv);
    if (port.is_error()) {
        fprintf(stderr, "pboard: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    PasteboardService service;
    os_log(os_log_create("com.vulture.pasteboard", "server"), "Starting");
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
