/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <LibBase/StdLibExtras.h>
#include <LibBase/Types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// BSD sockets over loopback, which goes through all of IP, UDP and TCP: only the Ethernet
// below them is left out. The test of that is Meta/network-test.sh.

namespace {

sockaddr_in loopback(u16 port)
{
    sockaddr_in address {};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

sockaddr* as_sockaddr(sockaddr_in& address)
{
    return reinterpret_cast<sockaddr*>(&address);
}

// A listening socket on a port of the system's choosing; the port comes back.
int make_listener(u16& port, int backlog = 8)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    auto address = loopback(0);
    CHECK(bind(fd, as_sockaddr(address), sizeof(address)) == 0);
    CHECK(listen(fd, backlog) == 0);
    socklen_t length = sizeof(address);
    CHECK(getsockname(fd, as_sockaddr(address), &length) == 0);
    port = ntohs(address.sin_port);
    CHECK(port >= 49152);
    return fd;
}

int connect_to(u16 port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    auto address = loopback(port);
    CHECK(connect(fd, as_sockaddr(address), sizeof(address)) == 0);
    return fd;
}

void test_names()
{
    in_addr_t value;
    CHECK(inet_pton(AF_INET, "10.0.2.15", &value) == 1);
    CHECK(ntohl(value) == 0x0a00020f);
    CHECK(inet_pton(AF_INET, "10.0.2", &value) == 0);
    CHECK(inet_pton(AF_INET, "10.0.2.256", &value) == 0);
    CHECK(inet_pton(AF_INET, "10.0.2.15.1", &value) == 0);
    in_addr address { htonl(0xc0a80101) };
    CHECK(strcmp(inet_ntoa(address), "192.168.1.1") == 0);

    addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    CHECK(getaddrinfo("localhost", "80", &hints, &results) == 0);
    CHECK(results != nullptr);
    if (results) {
        auto* found = reinterpret_cast<sockaddr_in*>(results->ai_addr);
        CHECK(found->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(found->sin_port == htons(80));
        CHECK(results->ai_next == nullptr);
        freeaddrinfo(results);
    }
    CHECK(getaddrinfo("127.0.0.1", "https", &hints, &results) == 0);
    if (results) {
        CHECK(reinterpret_cast<sockaddr_in*>(results->ai_addr)->sin_port == htons(443));
        freeaddrinfo(results);
    }
    // From /etc/hosts.
    CHECK(getaddrinfo("broadcasthost", nullptr, &hints, &results) == 0);
    if (results) {
        CHECK(reinterpret_cast<sockaddr_in*>(results->ai_addr)->sin_addr.s_addr == htonl(INADDR_BROADCAST));
        freeaddrinfo(results);
    }
    CHECK(getaddrinfo("127.0.0.1", "not-a-service", &hints, &results) == EAI_SERVICE);
}

void test_interfaces()
{
    vulture_interface_info interfaces[8];
    size_t size = sizeof(interfaces);
    CHECK(sysctlbyname("net.interfaces", interfaces, &size, nullptr, 0) == 0);
    CHECK(size >= sizeof(interfaces[0]));
    CHECK(strcmp(interfaces[0].name, "lo0") == 0);
    CHECK(interfaces[0].flags & VULTURE_IFF_LOOPBACK);
    CHECK(ntohl(interfaces[0].address) == INADDR_LOOPBACK);
}

void test_tcp_basics()
{
    u16 port;
    int listener = make_listener(port);
    int client = connect_to(port);
    sockaddr_in peer {};
    socklen_t peer_length = sizeof(peer);
    int server = accept(listener, as_sockaddr(peer), &peer_length);
    CHECK(server >= 0);
    CHECK(peer.sin_family == AF_INET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    // Each side knows the other.
    sockaddr_in name {};
    socklen_t name_length = sizeof(name);
    CHECK(getpeername(client, as_sockaddr(name), &name_length) == 0);
    CHECK(ntohs(name.sin_port) == port);
    CHECK(getsockname(client, as_sockaddr(name), &name_length) == 0);
    CHECK(name.sin_port == peer.sin_port);

    char const* message = "hello from the client";
    CHECK(write(client, message, strlen(message)) == static_cast<ssize_t>(strlen(message)));
    char buffer[64] = {};
    CHECK(read(server, buffer, sizeof(buffer)) == static_cast<ssize_t>(strlen(message)));
    CHECK(strcmp(buffer, message) == 0);

    CHECK(send(server, "pong", 4, 0) == 4);
    memset(buffer, 0, sizeof(buffer));
    // Peeking leaves it there.
    CHECK(recv(client, buffer, sizeof(buffer), MSG_PEEK) == 4);
    CHECK(recv(client, buffer, sizeof(buffer), 0) == 4);
    CHECK(memcmp(buffer, "pong", 4) == 0);

    // One way closed: reading gets the end, the other way still works.
    CHECK(shutdown(client, SHUT_WR) == 0);
    CHECK(read(server, buffer, sizeof(buffer)) == 0);
    CHECK(write(server, "more", 4) == 4);
    CHECK(read(client, buffer, sizeof(buffer)) == 4);
    close(server);
    CHECK(read(client, buffer, sizeof(buffer)) == 0);
    close(client);
    close(listener);
}

void test_tcp_refused()
{
    // A port nobody is on.
    u16 port;
    int listener = make_listener(port);
    close(listener);
    // (The close is finished on another thread: give it a moment.)
    timespec moment { 0, 50'000'000 };
    nanosleep(&moment, nullptr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    auto address = loopback(port);
    CHECK(connect(fd, as_sockaddr(address), sizeof(address)) == -1);
    CHECK(errno == ECONNREFUSED);
    close(fd);
}

void test_port_in_use()
{
    u16 port;
    int listener = make_listener(port);
    int other = socket(AF_INET, SOCK_STREAM, 0);
    auto address = loopback(port);
    CHECK(bind(other, as_sockaddr(address), sizeof(address)) == -1);
    CHECK(errno == EADDRINUSE);
    close(other);
    close(listener);
}

void test_nonblocking_and_poll()
{
    u16 port;
    int listener = make_listener(port);

    // Nobody has connected.
    pollfd poller { listener, POLLIN, 0 };
    CHECK(poll(&poller, 1, 0) == 0);
    int flags = fcntl(listener, F_GETFL);
    CHECK(fcntl(listener, F_SETFL, flags | O_NONBLOCK) == 0);
    CHECK(accept(listener, nullptr, nullptr) == -1 && errno == EAGAIN);

    // A connection that doesn't wait: EINPROGRESS, then writable.
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(client >= 0);
    auto address = loopback(port);
    CHECK(connect(client, as_sockaddr(address), sizeof(address)) == -1 && errno == EINPROGRESS);
    pollfd writable { client, POLLOUT, 0 };
    CHECK(poll(&writable, 1, 2000) == 1 && (writable.revents & POLLOUT));
    int error = -1;
    socklen_t size = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0);

    CHECK(poll(&poller, 1, 2000) == 1 && (poller.revents & POLLIN));
    int server = accept(listener, nullptr, nullptr);
    CHECK(server >= 0);

    char buffer[32];
    CHECK(recv(client, buffer, sizeof(buffer), 0) == -1 && errno == EAGAIN);
    CHECK(recv(server, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    pollfd readable { server, POLLIN, 0 };
    CHECK(poll(&readable, 1, 0) == 0);
    CHECK(send(client, "x", 1, 0) == 1);
    CHECK(poll(&readable, 1, 2000) == 1);
    CHECK(read(server, buffer, sizeof(buffer)) == 1);

    // The other side going away makes it readable: end of file.
    close(client);
    CHECK(poll(&readable, 1, 2000) == 1);
    CHECK(read(server, buffer, sizeof(buffer)) == 0);
    close(server);
    close(listener);
}

void test_receive_timeout()
{
    u16 port;
    int listener = make_listener(port);
    int client = connect_to(port);
    timeval limit { 0, 100'000 };
    CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) == 0);
    char buffer[8];
    CHECK(recv(client, buffer, sizeof(buffer), 0) == -1);
    CHECK(errno == EAGAIN);
    close(client);
    close(listener);
}

void test_sigpipe()
{
    u16 port;
    int listener = make_listener(port);
    int client = connect_to(port);
    int server = accept(listener, nullptr, nullptr);
    close(server);
    char buffer[8];
    CHECK(read(client, buffer, sizeof(buffer)) == 0);
    // The first write is answered by a reset, and after that it's over.
    int off = 1;
    CHECK(setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &off, sizeof(off)) == 0);
    (void)send(client, "x", 1, 0);
    timespec moment { 0, 50'000'000 };
    nanosleep(&moment, nullptr);
    ssize_t result = send(client, "x", 1, 0);
    CHECK(result == -1 && (errno == EPIPE || errno == ECONNRESET));
    close(client);
    close(listener);
}

// A number the same on both sides: what each byte should be, from where it is.
u8 pattern_at(size_t offset)
{
    u32 x = static_cast<u32>(offset / 251) * 2654435761u + static_cast<u32>(offset);
    return static_cast<u8>(x ^ (x >> 11));
}

// Sends `total` bytes one way over a real connection, checking every byte arrives, in order.
void transfer(size_t total, size_t piece, bool slow_reader)
{
    u16 port;
    int listener = make_listener(port);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(listener);
        int fd = connect_to(port);
        static u8 buffer[64 * 1024];
        size_t sent = 0;
        while (sent < total) {
            size_t size = min(min(piece, sizeof(buffer)), total - sent);
            for (size_t i = 0; i < size; ++i)
                buffer[i] = pattern_at(sent + i);
            ssize_t written = write(fd, buffer, size);
            if (written <= 0)
                _exit(3);
            sent += static_cast<size_t>(written);
        }
        close(fd);
        _exit(0);
    }
    int server = accept(listener, nullptr, nullptr);
    CHECK(server >= 0);
    static u8 buffer[64 * 1024];
    size_t received = 0;
    bool intact = true;
    for (;;) {
        ssize_t got = read(server, buffer, sizeof(buffer));
        CHECK(got >= 0);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; ++i) {
            if (buffer[i] != pattern_at(received + static_cast<size_t>(i)))
                intact = false;
        }
        received += static_cast<size_t>(got);
        if (slow_reader && received < total / 8) {
            timespec moment { 0, 2'000'000 };
            nanosleep(&moment, nullptr);
        }
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(received == total);
    CHECK(intact);
    close(server);
    close(listener);
}

void test_tcp_with_loss()
{
    // One packet in 23 lost, in each direction: retransmission has to put it all right.
    u32 rate = 23;
    if (geteuid() != 0)
        return;
    CHECK(sysctlbyname("net.inet.debug.drop_one_in", nullptr, nullptr, &rate, sizeof(rate)) == 0);
    transfer(4 * 1024 * 1024, 64 * 1024, false);
    rate = 0;
    CHECK(sysctlbyname("net.inet.debug.drop_one_in", nullptr, nullptr, &rate, sizeof(rate)) == 0);
}

void test_udp()
{
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(receiver >= 0);
    auto address = loopback(0);
    CHECK(bind(receiver, as_sockaddr(address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    CHECK(getsockname(receiver, as_sockaddr(address), &length) == 0);
    u16 port = ntohs(address.sin_port);
    CHECK(port != 0);

    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    auto destination = loopback(port);
    CHECK(sendto(sender, "first", 5, 0, as_sockaddr(destination), sizeof(destination)) == 5);
    CHECK(sendto(sender, "second one", 10, 0, as_sockaddr(destination), sizeof(destination)) == 10);

    // Datagrams stay whole and apart, and say who sent them.
    char buffer[32] = {};
    sockaddr_in from {};
    socklen_t from_length = sizeof(from);
    CHECK(recvfrom(receiver, buffer, sizeof(buffer), 0, as_sockaddr(from), &from_length) == 5);
    CHECK(memcmp(buffer, "first", 5) == 0);
    CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port != 0);
    // Too small a buffer gets the first part; the rest of that datagram is gone.
    CHECK(recv(receiver, buffer, 6, 0) == 6);
    CHECK(memcmp(buffer, "second", 6) == 0);

    // Answer whoever it was.
    CHECK(sendto(receiver, "back", 4, 0, as_sockaddr(from), sizeof(from)) == 4);
    CHECK(recv(sender, buffer, sizeof(buffer), 0) == 4);

    // Connected, to nobody: the port-unreachable comes back as an error.
    close(receiver);
    timespec moment { 0, 50'000'000 };
    nanosleep(&moment, nullptr);
    CHECK(connect(sender, as_sockaddr(destination), sizeof(destination)) == 0);
    CHECK(send(sender, "anyone?", 7, 0) == 7);
    pollfd poller { sender, POLLIN, 0 };
    CHECK(poll(&poller, 1, 2000) == 1);
    CHECK(recv(sender, buffer, sizeof(buffer), 0) == -1 && errno == ECONNREFUSED);

    // Broadcasting takes asking.
    int broadcaster = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in everyone = loopback(9);
    everyone.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    CHECK(sendto(broadcaster, "x", 1, 0, as_sockaddr(everyone), sizeof(everyone)) == -1 && errno == EACCES);
    close(broadcaster);
    close(sender);
}

void test_not_a_socket()
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    sockaddr_in address = loopback(1);
    CHECK(bind(fds[0], as_sockaddr(address), sizeof(address)) == -1 && errno == ENOTSOCK);
    CHECK(listen(fds[0], 1) == -1 && errno == ENOTSOCK);
    close(fds[0]);
    close(fds[1]);
    CHECK(socket(99, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
    CHECK(socket(AF_INET, 99, 0) == -1 && errno == EPROTONOSUPPORT);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    char buffer[4];
    CHECK(recv(fd, buffer, 4, 0) == -1 && errno == ENOTCONN);
    CHECK(lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE);
    close(fd);
    // Low ports are for root.
    if (geteuid() != 0) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        address = loopback(80);
        CHECK(bind(fd, as_sockaddr(address), sizeof(address)) == -1 && errno == EACCES);
        close(fd);
    }
}

void test_ping_self()
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    CHECK(fd >= 0);
    struct [[gnu::packed]] {
        u8 type;
        u8 code;
        u16 checksum;
        u16 identifier;
        u16 sequence;
        char data[8];
    } echo {};
    echo.type = 8;
    echo.identifier = htons(0x1234);
    echo.sequence = htons(1);
    memcpy(echo.data, "payload!", 8);
    auto address = loopback(0);
    CHECK(sendto(fd, &echo, sizeof(echo), 0, as_sockaddr(address), sizeof(address)) == sizeof(echo));
    pollfd poller { fd, POLLIN, 0 };
    CHECK(poll(&poller, 1, 2000) == 1);
    u8 reply[128];
    ssize_t got = recv(fd, reply, sizeof(reply), 0);
    CHECK(got == static_cast<ssize_t>(20 + sizeof(echo)));
    // After the IP header, as it arrived.
    CHECK(reply[0] == 0x45);
    CHECK(reply[20] == 0);
    CHECK(memcmp(reply + 28, "payload!", 8) == 0);
    close(fd);
}

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    puts("sockets: names");
    test_names();
    puts("sockets: interfaces");
    test_interfaces();
    puts("sockets: tcp_basics");
    test_tcp_basics();
    puts("sockets: tcp_refused");
    test_tcp_refused();
    puts("sockets: port_in_use");
    test_port_in_use();
    puts("sockets: nonblocking_and_poll");
    test_nonblocking_and_poll();
    puts("sockets: receive_timeout");
    test_receive_timeout();
    puts("sockets: sigpipe");
    test_sigpipe();
    puts("sockets: udp");
    test_udp();
    puts("sockets: not_a_socket");
    test_not_a_socket();
    puts("sockets: ping_self");
    test_ping_self();
    // More than the buffers hold, in pieces large and small.
    puts("sockets: transfers");
    transfer(8 * 1024 * 1024, 64 * 1024, false);
    transfer(600 * 1024, 113, false);
    transfer(2 * 1024 * 1024, 16 * 1024, true);
    puts("sockets: tcp_with_loss");
    test_tcp_with_loss();
    return finish();
}
