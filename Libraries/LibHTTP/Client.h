/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibHTTP/Response.h>
#include <LibHTTP/URL.h>

// Asking a server for something, over TCP or TLS, one request to a connection.

namespace HTTP {

// What went wrong, with the number curl gives that kind of thing.
struct Failure {
    int code { 0 };
    String message;
};

enum class FailureCode {
    BadURL = 3,
    CouldNotResolve = 6,
    CouldNotConnect = 7,
    PartialFile = 18,
    TimedOut = 28,
    TLSConnect = 35,
    TooManyRedirects = 47,
    SendError = 55,
    ReceiveError = 56,
    BadCertificate = 60,
    EmptyReply = 52,
    Other = 1,
    BodyRefused = 23,
};

struct Request {
    StringView method { "GET"_sv };
    URL url;
    Vector<Header> headers;
    ReadonlyBytes body;
};

struct Options {
    bool follow_redirects { false };
    int largest_redirects { 10 };
    // Skip checking the server's certificate ("curl -k").
    bool insecure { false };
    // Where the roots we trust are.
    StringView certificates_file { "/etc/ssl/cert.pem"_sv };
    // For the whole of it: nothing, for no limit.
    u32 timeout_seconds { 0 };
    u32 connect_timeout_seconds { 30 };
    // Ask for gzip, and undo it.
    bool accept_compression { false };
    StringView user_agent { "vultureOS"_sv };

    // Called with the head of the final response, and then its body in pieces.
    Function<ErrorOr<void>(ResponseHead const&)> on_head;
    Function<ErrorOr<void>(ReadonlyBytes)> on_body;
    // What it is doing, for "curl -v": a line at a time, with who said it as the prefix.
    Function<void(char prefix, StringView)> on_verbose;
    // Told how much of how much has arrived: total 0 means nobody knows.
    Function<void(u64 received, u64 total)> on_progress;
};

// False if it went wrong, and then `failure` says how. True for any answer at all from the
// server, even "404 Not Found": that is an answer.
[[nodiscard]] bool fetch(Request const&, Options&, ResponseHead& head, Failure& failure);

}
