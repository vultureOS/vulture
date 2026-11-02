/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/String.h>
#include <LibHTTP/Response.h>
#include <LibHTTP/URL.h>
#include <LibTest/TestCase.h>

using namespace HTTP;

namespace {

ReadonlyBytes bytes_of(StringView text)
{
    return { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() };
}

struct Parsed {
    ResponseHead head;
    StringBuilder body;
    int heads { 0 };
};

void connect_to(ResponseParser& parser, Parsed& parsed)
{
    parser.on_head = [&parsed](ResponseHead const& head) -> ErrorOr<void> {
        parsed.head = head;
        ++parsed.heads;
        return {};
    };
    parser.on_body = [&parsed](ReadonlyBytes bytes) -> ErrorOr<void> {
        parsed.body.append({ reinterpret_cast<char const*>(bytes.data()), bytes.size() });
        return {};
    };
}

// All at once, and then a byte at a time: it must not matter.
void expect_body(StringView response, StringView expected)
{
    {
        Parsed parsed;
        ResponseParser parser;
        connect_to(parser, parsed);
        MUST(parser.feed(bytes_of(response)));
        MUST(parser.finish());
        EXPECT(parsed.body.view() == expected);
        EXPECT_EQ(parsed.heads, 1);
    }
    {
        Parsed parsed;
        ResponseParser parser;
        connect_to(parser, parsed);
        for (size_t i = 0; i < response.length(); ++i)
            MUST(parser.feed(bytes_of(response.substring_view(i, 1))));
        MUST(parser.finish());
        EXPECT(parsed.body.view() == expected);
    }
}

}

TEST_CASE(urls)
{
    auto url = MUST(URL::parse("https://Example.COM:8443/a/b.txt?x=1#part"_sv));
    EXPECT(url.is_https());
    EXPECT(url.host.view() == "example.com"_sv);
    EXPECT_EQ(url.port, 8443);
    EXPECT(url.target.view() == "/a/b.txt?x=1"_sv);
    EXPECT(url.file_name().view() == "b.txt"_sv);
    EXPECT(url.host_header().view() == "example.com:8443"_sv);

    auto plain = MUST(URL::parse("example.com"_sv));
    EXPECT(!plain.is_https());
    EXPECT_EQ(plain.port, 80);
    EXPECT(plain.target.view() == "/"_sv);
    EXPECT(plain.host_header().view() == "example.com"_sv);
    EXPECT(plain.to_string().view() == "http://example.com/"_sv);

    EXPECT_EQ(MUST(URL::parse("https://example.com"_sv)).port, 443);
    EXPECT(MUST(URL::parse("http://example.com?q"_sv)).target.view() == "/?q"_sv);
}

TEST_CASE(bad_urls)
{
    EXPECT(URL::parse("ftp://example.com"_sv).is_error());
    EXPECT(URL::parse("http://"_sv).is_error());
    EXPECT(URL::parse("http://:80"_sv).is_error());
    EXPECT(URL::parse("http://example.com:99999"_sv).is_error());
    EXPECT(URL::parse("http://example.com:abc"_sv).is_error());
    EXPECT(URL::parse("http://example.com:0"_sv).is_error());
    // A user name is how a URL looks like one host and is another.
    EXPECT(URL::parse("http://trusted.example@evil.example/"_sv).is_error());
    // These could add lines to the request.
    EXPECT(URL::parse("http://example.com/a b"_sv).is_error());
    EXPECT(URL::parse("http://example.com/a\r\nX: y"_sv).is_error());
    EXPECT(URL::parse("http://exa mple.com/"_sv).is_error());
}

TEST_CASE(redirects)
{
    auto base = MUST(URL::parse("https://example.com/dir/page.html?x=1"_sv));
    EXPECT(MUST(base.resolve("https://other.example/x"_sv)).to_string().view() == "https://other.example/x"_sv);
    EXPECT(MUST(base.resolve("/root"_sv)).to_string().view() == "https://example.com/root"_sv);
    EXPECT(MUST(base.resolve("next.html"_sv)).to_string().view() == "https://example.com/dir/next.html"_sv);
    EXPECT(MUST(base.resolve("?y=2"_sv)).to_string().view() == "https://example.com/dir/page.html?y=2"_sv);
    EXPECT(MUST(base.resolve("//cdn.example/z"_sv)).to_string().view() == "https://cdn.example/z"_sv);
    EXPECT(base.resolve("/a\r\nInjected: yes"_sv).is_error());
}

TEST_CASE(a_length)
{
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Other: y\r\n\r\nhello"_sv, "hello"_sv);
}

TEST_CASE(what_comes_after_a_length_is_not_the_body)
{
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello and more"_sv, "hello"_sv);
}

TEST_CASE(until_the_end)
{
    expect_body("HTTP/1.0 200 OK\r\nServer: old\r\n\r\nall of this"_sv, "all of this"_sv);
}

TEST_CASE(chunks)
{
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n"_sv, "hello world"_sv);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n"_sv, "0123456789"_sv);
}

TEST_CASE(the_head)
{
    Parsed parsed;
    ResponseParser parser;
    connect_to(parser, parsed);
    MUST(parser.feed(bytes_of("HTTP/1.1 404 Not Found\r\ncontent-TYPE:  text/plain \r\nContent-Length: 0\r\n\r\n"_sv)));
    MUST(parser.finish());
    EXPECT(parser.is_complete());
    EXPECT_EQ(parsed.head.status, 404);
    EXPECT(parsed.head.reason.view() == "Not Found"_sv);
    EXPECT(parsed.head.version.view() == "HTTP/1.1"_sv);
    EXPECT(parsed.head.header("Content-Type"_sv).value() == "text/plain"_sv);
    EXPECT(!parsed.head.header("Nope"_sv).has_value());
}

TEST_CASE(interim_responses_are_not_the_answer)
{
    Parsed parsed;
    ResponseParser parser;
    connect_to(parser, parsed);
    MUST(parser.feed(bytes_of("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"_sv)));
    MUST(parser.finish());
    EXPECT_EQ(parsed.heads, 1);
    EXPECT_EQ(parsed.head.status, 200);
    EXPECT(parsed.body.view() == "ok"_sv);
}

TEST_CASE(no_body_for_some)
{
    StringView const responses[] = { "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n"_sv, "HTTP/1.1 304 Not Modified\r\n\r\n"_sv };
    for (auto const response : responses) {
        Parsed parsed;
        ResponseParser parser;
        connect_to(parser, parsed);
        MUST(parser.feed(bytes_of(response)));
        EXPECT(parser.is_complete());
    }
    Parsed parsed;
    ResponseParser parser(true);
    connect_to(parser, parsed);
    MUST(parser.feed(bytes_of("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"_sv)));
    EXPECT(parser.is_complete());
}

TEST_CASE(cut_short)
{
    auto truncated = [](StringView response) {
        ResponseParser parser;
        MUST(parser.feed(bytes_of(response)));
        return parser.finish().is_error();
    };
    EXPECT(truncated(""_sv));
    EXPECT(truncated("HTTP/1.1 200"_sv));
    EXPECT(truncated("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"_sv));
    EXPECT(truncated("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhell"_sv));
    EXPECT(truncated("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"_sv));
    EXPECT(truncated("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"_sv));
}

TEST_CASE(nonsense)
{
    auto refused = [](StringView response) {
        ResponseParser parser;
        return parser.feed(bytes_of(response)).is_error();
    };
    EXPECT(refused("SSH-2.0-OpenSSH\r\n"_sv));
    EXPECT(refused("HTTP/1.1 abc OK\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nno colon here\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\n: no name\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n"_sv));
    // Two lengths are how one response gets read as two.
    EXPECT(refused("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFF\r\n"_sv));
    EXPECT(refused("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nabc\r\n"_sv));
}

TEST_CASE(endless_lines_are_refused)
{
    ResponseParser parser;
    static u8 a_lot[128 * 1024];
    __builtin_memset(a_lot, 'x', sizeof(a_lot));
    EXPECT(parser.feed({ a_lot, sizeof(a_lot) }).is_error());
}
