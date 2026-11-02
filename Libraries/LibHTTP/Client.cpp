/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCompress/Deflate.h>
#include <LibHTTP/Client.h>
#include <LibTLS/Connection.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace HTTP {

namespace {

u64 now_milliseconds()
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<u64>(time.tv_sec) * 1000 + static_cast<u64>(time.tv_nsec) / 1'000'000;
}

Failure make_failure(FailureCode code, StringView message)
{
    Failure failure;
    failure.code = static_cast<int>(code);
    failure.message = MUST(String::from_view(message));
    return failure;
}

Failure make_failure(FailureCode code, Error const& error)
{
    if (error.is_errno())
        return make_failure(code, StringView { strerror(error.code()), strlen(strerror(error.code())) });
    return make_failure(code, error.string_literal());
}

// The socket, watched by a deadline.
class SocketTransport final : public TLS::Transport {
public:
    SocketTransport(int fd, u64 deadline)
        : m_fd(fd)
        , m_deadline(deadline)
    {
    }

    ErrorOr<size_t> read(Bytes output) override
    {
        for (;;) {
            TRY(wait_until(POLLIN));
            ssize_t got = ::read(m_fd, output.data(), output.size());
            if (got >= 0)
                return static_cast<size_t>(got);
            if (errno != EINTR && errno != EAGAIN)
                return Error::from_errno(errno);
        }
    }

    ErrorOr<void> write(ReadonlyBytes bytes) override
    {
        size_t done = 0;
        while (done < bytes.size()) {
            TRY(wait_until(POLLOUT));
            ssize_t sent = ::send(m_fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                return Error::from_errno(errno);
            }
            done += static_cast<size_t>(sent);
        }
        return {};
    }

private:
    ErrorOr<void> wait_until(short events)
    {
        for (;;) {
            int timeout = -1;
            if (m_deadline != 0) {
                u64 const now = now_milliseconds();
                if (now >= m_deadline)
                    return Error::from_errno(ETIMEDOUT);
                timeout = static_cast<int>(m_deadline - now);
            }
            pollfd poller { m_fd, events, 0 };
            int ready = poll(&poller, 1, timeout);
            if (ready > 0)
                return {};
            if (ready == 0)
                return Error::from_errno(ETIMEDOUT);
            if (errno != EINTR)
                return Error::from_errno(errno);
        }
    }

    int m_fd;
    u64 m_deadline;
};

void fill_randomly(Bytes output)
{
    size_t done = 0;
    while (done < output.size()) {
        size_t const piece = min<size_t>(output.size() - done, 256);
        (void)getentropy(output.data() + done, piece);
        done += piece;
    }
}

bool read_whole_file(StringView path, Vector<u8>& bytes)
{
    StringBuilder terminated;
    terminated.append(path);
    terminated.append("\0"_sv);
    int fd = open(terminated.view().characters_without_null_termination(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    for (;;) {
        u8 chunk[65536];
        ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            close(fd);
            return false;
        }
        if (got == 0)
            break;
        size_t const at = bytes.size();
        MUST(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return true;
}

// Connects to the first address that answers.
int connect_to_host(URL const& url, Options& options, Failure& failure)
{
    StringBuilder host_text;
    host_text.append(url.host.view());
    host_text.append("\0"_sv);
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", url.port);
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    int const resolved = getaddrinfo(host_text.view().characters_without_null_termination(), port_text, &hints, &results);
    if (resolved != 0) {
        StringBuilder message;
        message.appendff("Could not resolve host: {}", url.host.view());
        failure = make_failure(FailureCode::CouldNotResolve, message.view());
        return -1;
    }
    int connected = -1;
    int last_error = 0;
    for (auto* info = results; info && connected < 0; info = info->ai_next) {
        char address_text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr, address_text, sizeof(address_text));
        if (options.on_verbose) {
            StringBuilder line;
            line.appendff("Trying {}:{}...", StringView { address_text, strlen(address_text) }, url.port);
            options.on_verbose('*', line.view());
        }
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        int const result = connect(fd, info->ai_addr, info->ai_addrlen);
        if (result != 0 && errno != EINPROGRESS) {
            last_error = errno;
            close(fd);
            continue;
        }
        if (result != 0) {
            pollfd poller { fd, POLLOUT, 0 };
            int const timeout = options.connect_timeout_seconds ? static_cast<int>(options.connect_timeout_seconds * 1000) : -1;
            int ready;
            do {
                ready = poll(&poller, 1, timeout);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0) {
                last_error = ready == 0 ? ETIMEDOUT : errno;
                close(fd);
                continue;
            }
            int error = 0;
            socklen_t size = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
            if (error != 0) {
                last_error = error;
                close(fd);
                continue;
            }
        }
        // Requests are small, and nothing is gained by waiting on them.
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        connected = fd;
        if (options.on_verbose) {
            StringBuilder line;
            line.appendff("Connected to {} ({}) port {}", url.host.view(), StringView { address_text, strlen(address_text) }, url.port);
            options.on_verbose('*', line.view());
        }
    }
    freeaddrinfo(results);
    if (connected < 0) {
        StringBuilder message;
        message.appendff("Failed to connect to {} port {}: {}", url.host.view(), url.port, StringView { strerror(last_error), strlen(strerror(last_error)) });
        failure = make_failure(last_error == ETIMEDOUT ? FailureCode::TimedOut : FailureCode::CouldNotConnect, message.view());
    }
    return connected;
}

// One request on one connection. Which of them it was is up to the caller, with redirects.
bool fetch_once(Request const& request, Options& options, u64 deadline, ResponseHead& final_head, bool deliver_body, Failure& failure)
{
    int const fd = connect_to_host(request.url, options, failure);
    if (fd < 0)
        return false;
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer { fd };

    SocketTransport socket(fd, deadline);
    OwnPtr<TLS::Connection> connection;
    TLS::TrustStore store;

    if (request.url.is_https()) {
        if (!options.insecure) {
            Vector<u8> certificates;
            if (!read_whole_file(options.certificates_file, certificates)) {
                failure = make_failure(FailureCode::BadCertificate, "can't read the certificate authorities' file"_sv);
                return false;
            }
            store.add_pem(certificates.span());
        }
        TLS::Options tls;
        tls.host = request.url.host.view();
        tls.trust_store = &store;
        tls.now = time(nullptr);
        tls.random = fill_randomly;
        tls.verify_certificate = !options.insecure;
        tls.application_protocol = "http/1.1"_sv;
        if (options.on_verbose) {
            tls.on_progress = [&options](StringView line) { options.on_verbose('*', line); };
        }
        auto handshake = TLS::Connection::handshake(socket, move(tls));
        if (handshake.is_error()) {
            auto const& error = handshake.error();
            // The certificate's complaints read as such; the rest are the connection's.
            StringView const text = error.is_errno() ? StringView {} : error.string_literal();
            if (!error.is_errno() && !text.starts_with("TLS:"_sv)) {
                StringBuilder message;
                message.appendff("SSL certificate problem: {}", text);
                failure = make_failure(FailureCode::BadCertificate, message.view());
            } else if (error.is_errno() && error.code() == ETIMEDOUT) {
                failure = make_failure(FailureCode::TimedOut, "Operation timed out"_sv);
            } else {
                failure = make_failure(FailureCode::TLSConnect, error);
            }
            return false;
        }
        connection = handshake.release_value();
        if (options.on_verbose) {
            StringBuilder line;
            line.appendff("SSL connection using TLSv1.3 / {} / {}", connection->cipher_suite_name(), connection->group_name());
            options.on_verbose('*', line.view());
            if (!options.insecure) {
                StringBuilder subject;
                subject.appendff("Server certificate: subject {}, issuer {}", connection->peer_subject().view(), connection->peer_issuer().view());
                options.on_verbose('*', subject.view());
            }
        }
    }

    StringBuilder text;
    text.appendff("{} {} HTTP/1.1\r\n", request.method, request.url.target.view());
    text.appendff("Host: {}\r\n", request.url.host_header().view());
    bool saw_user_agent = false;
    bool saw_accept = false;
    bool saw_content_length = false;
    for (auto const& header : request.headers) {
        auto is = [&](StringView name) {
            if (header.name.length() != name.length())
                return false;
            for (size_t i = 0; i < name.length(); ++i) {
                char a = header.name.view()[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z')
                    a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z')
                    b = static_cast<char>(b + 32);
                if (a != b)
                    return false;
            }
            return true;
        };
        saw_user_agent = saw_user_agent || is("User-Agent"_sv);
        saw_accept = saw_accept || is("Accept"_sv);
        saw_content_length = saw_content_length || is("Content-Length"_sv);
        text.appendff("{}: {}\r\n", header.name.view(), header.value.view());
    }
    if (!saw_user_agent)
        text.appendff("User-Agent: {}\r\n", options.user_agent);
    if (!saw_accept)
        text.append("Accept: */*\r\n"_sv);
    if (options.accept_compression)
        text.append("Accept-Encoding: gzip\r\n"_sv);
    if (!request.body.is_empty() && !saw_content_length)
        text.appendff("Content-Length: {}\r\n", request.body.size());
    text.append("Connection: close\r\n\r\n"_sv);

    if (options.on_verbose) {
        text.view().for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
            if (line.ends_with("\r"_sv))
                line = line.substring_view(0, line.length() - 1);
            options.on_verbose('>', line);
        });
    }

    auto send = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        if (connection)
            return connection->write(bytes);
        return socket.write(bytes);
    };
    auto const request_view = text.view();
    auto sent = send({ reinterpret_cast<u8 const*>(request_view.characters_without_null_termination()), request_view.length() });
    if (!sent.is_error() && !request.body.is_empty())
        sent = send(request.body);
    if (sent.is_error()) {
        failure = make_failure(sent.error().is_errno() && sent.error().code() == ETIMEDOUT ? FailureCode::TimedOut : FailureCode::SendError, sent.error());
        return false;
    }

    bool const is_head = request.method == "HEAD"_sv;
    ResponseParser parser(is_head);
    u64 received = 0;
    u64 total = 0;
    bool compressed = false;
    Vector<u8> compressed_body;
    parser.on_head = [&](ResponseHead const& head) -> ErrorOr<void> {
        if (options.on_verbose) {
            StringBuilder line;
            line.appendff("{} {} {}", head.version.view(), head.status, head.reason.view());
            options.on_verbose('<', line.view());
            for (auto const& header : head.headers) {
                StringBuilder header_line;
                header_line.appendff("{}: {}", header.name.view(), header.value.view());
                options.on_verbose('<', header_line.view());
            }
        }
        auto const encoding = head.header("Content-Encoding"_sv);
        compressed = options.accept_compression && encoding.has_value() && encoding.value() == "gzip"_sv;
        if (auto length = head.header("Content-Length"_sv); length.has_value()) {
            for (size_t i = 0; i < length.value().length(); ++i)
                total = total * 10 + static_cast<u64>(length.value()[i] - '0');
        }
        if (deliver_body && options.on_head)
            return options.on_head(head);
        return {};
    };
    parser.on_body = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        received += bytes.size();
        if (deliver_body && options.on_progress)
            options.on_progress(received, total);
        if (!deliver_body)
            return {};
        if (compressed) {
            size_t const at = compressed_body.size();
            TRY(compressed_body.try_resize(at + bytes.size()));
            memcpy(compressed_body.data() + at, bytes.data(), bytes.size());
            return {};
        }
        if (options.on_body)
            return options.on_body(bytes);
        return {};
    };

    for (;;) {
        u8 chunk[65536];
        auto got = connection ? connection->read({ chunk, sizeof(chunk) }) : socket.read({ chunk, sizeof(chunk) });
        if (got.is_error()) {
            auto const& error = got.error();
            // A server that goes without a goodbye, having said all it means to, is common.
            if (connection && !error.is_errno() && !parser.is_complete() && received > 0 && total == 0) {
                break;
            }
            if (parser.is_complete())
                break;
            failure = make_failure(error.is_errno() && error.code() == ETIMEDOUT ? FailureCode::TimedOut : FailureCode::ReceiveError, error);
            return false;
        }
        if (got.value() == 0)
            break;
        auto fed = parser.feed({ chunk, got.value() });
        if (fed.is_error()) {
            failure = make_failure(FailureCode::BodyRefused, fed.error());
            return false;
        }
        if (parser.is_complete())
            break;
    }
    auto finished = parser.finish();
    if (finished.is_error()) {
        bool const nothing = finished.error().string_literal().contains("empty reply"_sv);
        failure = make_failure(nothing ? FailureCode::EmptyReply : FailureCode::PartialFile, finished.error());
        return false;
    }
    if (connection)
        (void)connection->close();

    if (compressed) {
        auto inflated = Compress::gzip_decompress(compressed_body.span());
        if (inflated.is_error()) {
            failure = make_failure(FailureCode::Other, "the server's gzip wouldn't decompress"_sv);
            return false;
        }
        if (options.on_body) {
            auto result = options.on_body(inflated.value().span());
            if (result.is_error()) {
                failure = make_failure(FailureCode::BodyRefused, result.error());
                return false;
            }
        }
    }
    final_head = parser.head();
    return true;
}

}

bool fetch(Request const& original, Options& options, ResponseHead& head, Failure& failure)
{
    u64 const deadline = options.timeout_seconds ? now_milliseconds() + static_cast<u64>(options.timeout_seconds) * 1000 : 0;
    URL url = original.url;
    StringView method = original.method;
    ReadonlyBytes body = original.body;
    for (int redirects = 0;; ++redirects) {
        Request request;
        request.method = method;
        request.url = url;
        request.headers = original.headers;
        request.body = body;

        // Only the last response's body is the answer: a redirect's is passed by, once it is
        // clear that it is one. Whether it is shows in the status, which comes first.
        bool deliver = !options.follow_redirects;
        ResponseHead this_head;
        Function<ErrorOr<void>(ResponseHead const&)> caller_head = move(options.on_head);
        Function<ErrorOr<void>(ReadonlyBytes)> caller_body = move(options.on_body);
        bool is_redirect = false;
        options.on_head = [&](ResponseHead const& response) -> ErrorOr<void> {
            is_redirect = options.follow_redirects && response.status >= 300 && response.status < 400 && response.header("Location"_sv).has_value();
            if (caller_head && !is_redirect)
                return caller_head(response);
            return {};
        };
        options.on_body = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
            if (caller_body && !is_redirect)
                return caller_body(bytes);
            return {};
        };
        deliver = true;
        bool const ok = fetch_once(request, options, deadline, this_head, deliver, failure);
        options.on_head = move(caller_head);
        options.on_body = move(caller_body);
        if (!ok)
            return false;
        head = move(this_head);
        if (!is_redirect)
            return true;

        if (redirects >= options.largest_redirects) {
            StringBuilder message;
            message.appendff("Maximum ({}) redirects followed", options.largest_redirects);
            failure = make_failure(FailureCode::TooManyRedirects, message.view());
            return false;
        }
        auto next = url.resolve(head.header("Location"_sv).value());
        if (next.is_error()) {
            failure = make_failure(FailureCode::BadURL, next.error());
            return false;
        }
        // From https down to plain http would give away what was asked for in private.
        if (url.is_https() && !next.value().is_https()) {
            failure = make_failure(FailureCode::Other, "refusing a redirect from https to http"_sv);
            return false;
        }
        if (options.on_verbose) {
            StringBuilder line;
            line.appendff("Issue another request to this URL: '{}'", next.value().to_string().view());
            options.on_verbose('*', line.view());
        }
        // As browsers do: 303, and POST on 301 and 302, turn into GET.
        if (head.status == 303 || ((head.status == 301 || head.status == 302) && method == "POST"_sv)) {
            method = "GET"_sv;
            body = {};
        }
        url = next.release_value();
    }
}

}
