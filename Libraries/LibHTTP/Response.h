/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace HTTP {

struct Header {
    String name;
    String value;
};

struct ResponseHead {
    int status { 0 };
    String reason;
    String version;
    Vector<Header> headers;

    // The first of this name; any case.
    [[nodiscard]] Optional<StringView> header(StringView name) const;
};

// Reads a response, fed in whatever pieces it arrives in: the status line and headers, and
// then the body by its Content-Length, as chunks, or up to the end of the connection. Limits
// on what it will hold mean a server can't make it use up memory.
class ResponseParser {
public:
    // No body to expect, whatever the headers say: the answer to a HEAD.
    explicit ResponseParser(bool no_body = false)
        : m_no_body(no_body)
    {
    }

    Function<ErrorOr<void>(ResponseHead const&)> on_head;
    Function<ErrorOr<void>(ReadonlyBytes)> on_body;

    ErrorOr<void> feed(ReadonlyBytes);
    // The connection has ended. Fails if the response had more to come.
    ErrorOr<void> finish();

    [[nodiscard]] bool is_complete() const { return m_state == State::Done; }
    [[nodiscard]] ResponseHead const& head() const { return m_head; }

private:
    enum class State {
        StatusLine,
        Headers,
        BodyByLength,
        BodyUntilClose,
        ChunkSize,
        ChunkData,
        ChunkDataEnd,
        Trailers,
        Done,
    };

    ErrorOr<void> process_line(StringView);
    ErrorOr<void> begin_body();

    State m_state { State::StatusLine };
    bool m_no_body;
    ResponseHead m_head;
    Vector<char> m_line;
    u64 m_remaining { 0 };
};

}
