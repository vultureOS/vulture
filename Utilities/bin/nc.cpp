/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// nc [-l] [-u] [-z] [-w seconds] host port: copies standard input to a connection and the
// connection to standard output. With -l it waits for one instead; with -z it only tries.

namespace {

int copy_both_ways(int fd, int wait_seconds)
{
    pollfd pollers[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
    bool input_open = true;
    for (;;) {
        pollers[0].fd = input_open ? 0 : -1;
        int ready = poll(pollers, 2, wait_seconds > 0 ? wait_seconds * 1000 : -1);
        if (ready == 0)
            return 0;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("nc: poll");
            return 1;
        }
        char buffer[8192];
        if (pollers[0].revents) {
            ssize_t got = read(0, buffer, sizeof(buffer));
            if (got <= 0) {
                input_open = false;
                shutdown(fd, SHUT_WR);
            } else {
                for (ssize_t done = 0; done < got;) {
                    ssize_t sent = write(fd, buffer + done, static_cast<size_t>(got - done));
                    if (sent <= 0)
                        return 1;
                    done += sent;
                }
            }
        }
        if (pollers[1].revents) {
            ssize_t got = read(fd, buffer, sizeof(buffer));
            if (got <= 0)
                return 0;
            for (ssize_t done = 0; done < got;) {
                ssize_t sent = write(1, buffer + done, static_cast<size_t>(got - done));
                if (sent <= 0)
                    return 1;
                done += sent;
            }
        }
    }
}

}

int main(int argc, char** argv)
{
    bool listening = false;
    bool udp = false;
    bool scan = false;
    int wait_seconds = 0;
    int option;
    while ((option = getopt(argc, argv, "luzw:")) != -1) {
        switch (option) {
        case 'l':
            listening = true;
            break;
        case 'u':
            udp = true;
            break;
        case 'z':
            scan = true;
            break;
        case 'w':
            wait_seconds = atoi(optarg);
            break;
        default:
            fprintf(stderr, "usage: nc [-luz] [-w seconds] [host] port\n");
            return 1;
        }
    }
    int const remaining = argc - optind;
    if ((listening && remaining != 1 && remaining != 2) || (!listening && remaining != 2)) {
        fprintf(stderr, "usage: nc [-luz] [-w seconds] [host] port\n");
        return 1;
    }
    char const* host = remaining == 2 ? argv[optind] : nullptr;
    char const* port = argv[optind + remaining - 1];

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = listening ? AI_PASSIVE : 0;
    addrinfo* results = nullptr;
    int resolved = getaddrinfo(host, port, &hints, &results);
    if (resolved != 0) {
        fprintf(stderr, "nc: getaddrinfo: %s\n", gai_strerror(resolved));
        return 1;
    }
    int fd = socket(AF_INET, hints.ai_socktype, 0);
    if (fd < 0) {
        perror("nc: socket");
        return 1;
    }
    if (listening) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd, results->ai_addr, results->ai_addrlen) != 0) {
            perror("nc: bind");
            return 1;
        }
        if (!udp) {
            if (listen(fd, 1) != 0) {
                perror("nc: listen");
                return 1;
            }
            int connection = accept(fd, nullptr, nullptr);
            if (connection < 0) {
                perror("nc: accept");
                return 1;
            }
            close(fd);
            fd = connection;
        }
    } else if (connect(fd, results->ai_addr, results->ai_addrlen) != 0) {
        fprintf(stderr, "nc: connect to %s port %s (%s) failed: %s\n", host, port, udp ? "udp" : "tcp", strerror(errno));
        return 1;
    }
    freeaddrinfo(results);
    if (scan) {
        printf("Connection to %s port %s [%s] succeeded!\n", host ? host : "*", port, udp ? "udp" : "tcp");
        return 0;
    }
    return copy_both_ways(fd, wait_seconds);
}
