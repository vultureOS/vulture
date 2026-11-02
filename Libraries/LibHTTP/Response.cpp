/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/CharacterTypes.h>
#include <LibHTTP/Response.h>

namespace HTTP {

namespace {

constexpr size_t largest_line = 64 * 1024;
constexpr size_t largest_headers = 256;

bool same_ignoring_case(StringView a, StringView b)
{
    if (a.length() != b.length())
        return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (to_ascii_lowercase(a[i]) != to_ascii_lowercase(b[i]))
            return false;
    }
    return true;
}

StringView trimmed(StringView text)
{
    size_t start = 0;
    size_t end = text.length();
    while (start < end && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return text.substring_view(start, end - start);
}

bool contains_token(StringView list, StringView token)
{
    bool found = false;
    list.for_each_split_view(',', SplitBehavior::SkipEmpty, [&](StringView part) {
        if (same_ignoring_case(trimmed(part), token))
            found = true;
    });
    return found;
}

}

Optional<StringView> ResponseHead::header(StringView name) const
{
    for (auto const& entry : headers) {
        if (same_ignoring_case(entry.name.view(), name))
            return entry.value.view();
    }
    return {};
}

ErrorOr<void> ResponseParser::begin_body()
{
    // Interim responses aren't the answer: back to waiting for that.
    if (m_head.status >= 100 && m_head.status < 200 && m_head.status != 101) {
        m_head = {};
        m_state = State::StatusLine;
        return {};
    }
    if (on_head)
        TRY(on_head(m_head));
    if (m_no_body || m_head.status == 204 || m_head.status == 304 || m_head.status == 101) {
        m_state = State::Done;
        return {};
    }
    auto const encoding = m_head.header("Transfer-Encoding"_sv);
    if (encoding.has_value() && contains_token(encoding.value(), "chunked"_sv)) {
        m_state = State::ChunkSize;
        return {};
    }
    auto const length = m_head.header("Content-Length"_sv);
    if (length.has_value()) {
        // Two different lengths are how one request becomes another in the wrong hands.
        u64 number = 0;
        StringView const text = trimmed(length.value());
        if (text.is_empty())
            return Error::from_string_literal("HTTP: a bad Content-Length");
        for (size_t i = 0; i < text.length(); ++i) {
            if (!is_ascii_digit(text[i]) || number > (~0ull - 9) / 10)
                return Error::from_string_literal("HTTP: a bad Content-Length");
            number = number * 10 + static_cast<u64>(text[i] - '0');
        }
        for (auto const& entry : m_head.headers) {
            if (same_ignoring_case(entry.name.view(), "Content-Length"_sv) && trimmed(entry.value.view()) != text)
                return Error::from_string_literal("HTTP: two different Content-Lengths");
        }
        m_remaining = number;
        m_state = number == 0 ? State::Done : State::BodyByLength;
        return {};
    }
    m_state = State::BodyUntilClose;
    return {};
}

ErrorOr<void> ResponseParser::process_line(StringView line)
{
    switch (m_state) {
    case State::StatusLine: {
        // HTTP/1.1 200 OK. (The reason may be empty, or absent.)
        if (!line.starts_with("HTTP/"_sv))
            return Error::from_string_literal("HTTP: not an HTTP response");
        auto const space = line.find(' ');
        if (!space.has_value() || line.length() < space.value() + 4)
            return Error::from_string_literal("HTTP: a bad status line");
        m_head.version = TRY(String::from_view(line.substring_view(0, space.value())));
        int status = 0;
        for (size_t i = 1; i <= 3; ++i) {
            char const c = line[space.value() + i];
            if (!is_ascii_digit(c))
                return Error::from_string_literal("HTTP: a bad status line");
            status = status * 10 + (c - '0');
        }
        m_head.status = status;
        if (line.length() > space.value() + 5)
            m_head.reason = TRY(String::from_view(line.substring_view(space.value() + 5)));
        m_state = State::Headers;
        return {};
    }
    case State::Headers: {
        if (line.is_empty())
            return begin_body();
        auto const colon = line.find(':');
        if (!colon.has_value() || colon.value() == 0)
            return Error::from_string_literal("HTTP: a bad header");
        if (m_head.headers.size() >= largest_headers)
            return Error::from_string_literal("HTTP: too many headers");
        Header header;
        header.name = TRY(String::from_view(line.substring_view(0, colon.value())));
        header.value = TRY(String::from_view(trimmed(line.substring_view(colon.value() + 1))));
        TRY(m_head.headers.try_append(move(header)));
        return {};
    }
    case State::ChunkSize: {
        // Hexadecimal, and maybe extensions after a semicolon.
        u64 size = 0;
        size_t digits = 0;
        for (size_t i = 0; i < line.length() && line[i] != ';' && line[i] != ' '; ++i) {
            char const c = line[i];
            u32 value;
            if (c >= '0' && c <= '9')
                value = static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value = static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value = static_cast<u32>(c - 'A' + 10);
            else
                return Error::from_string_literal("HTTP: a bad chunk size");
            if (++digits > 15)
                return Error::from_string_literal("HTTP: a bad chunk size");
            size = size * 16 + value;
        }
        if (digits == 0)
            return Error::from_string_literal("HTTP: a bad chunk size");
        m_remaining = size;
        m_state = size == 0 ? State::Trailers : State::ChunkData;
        return {};
    }
    case State::ChunkDataEnd:
        if (!line.is_empty())
            return Error::from_string_literal("HTTP: a chunk went on past its size");
        m_state = State::ChunkSize;
        return {};
    case State::Trailers:
        if (line.is_empty())
            m_state = State::Done;
        return {};
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<void> ResponseParser::feed(ReadonlyBytes bytes)
{
    size_t at = 0;
    while (at < bytes.size()) {
        switch (m_state) {
        case State::BodyByLength:
        case State::ChunkData: {
            size_t const size = min<u64>(m_remaining, bytes.size() - at);
            if (on_body)
                TRY(on_body({ bytes.data() + at, size }));
            at += size;
            m_remaining -= size;
            if (m_remaining == 0)
                m_state = m_state == State::BodyByLength ? State::Done : State::ChunkDataEnd;
            break;
        }
        case State::BodyUntilClose:
            if (on_body)
                TRY(on_body({ bytes.data() + at, bytes.size() - at }));
            at = bytes.size();
            break;
        case State::Done:
            // More than was promised: nothing here wants it.
            return {};
        default: {
            char const c = static_cast<char>(bytes[at++]);
            if (c == '\n') {
                size_t size = m_line.size();
                if (size > 0 && m_line[size - 1] == '\r')
                    --size;
                Vector<char> line = move(m_line);
                m_line = {};
                TRY(process_line({ line.data(), size }));
            } else {
                if (m_line.size() >= largest_line)
                    return Error::from_string_literal("HTTP: a line too long");
                TRY(m_line.try_append(c));
            }
            break;
        }
        }
    }
    return {};
}

ErrorOr<void> ResponseParser::finish()
{
    switch (m_state) {
    case State::BodyUntilClose:
        m_state = State::Done;
        return {};
    case State::Done:
        return {};
    case State::StatusLine:
        if (m_line.is_empty())
            return Error::from_string_literal("HTTP: an empty reply from the server");
        return Error::from_string_literal("HTTP: the connection ended in the headers");
    case State::Headers:
        return Error::from_string_literal("HTTP: the connection ended in the headers");
    default:
        return Error::from_string_literal("HTTP: the connection ended before the whole response");
    }
}

}
