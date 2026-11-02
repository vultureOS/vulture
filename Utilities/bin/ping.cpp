/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ping [-c count] [-i seconds] [-s size] [-t timeout] [-q] host: echoes, with macOS's output.

namespace {

volatile sig_atomic_t s_interrupted = 0;

double now_milliseconds()
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) * 1000.0 + static_cast<double>(time.tv_nsec) / 1e6;
}

constexpr size_t largest_data = 1400;

struct Statistics {
    long sent { 0 };
    long received { 0 };
    double smallest { 0 };
    double largest { 0 };
    double sum { 0 };
    double squares { 0 };
};

}

int main(int argc, char** argv)
{
    long count = -1;
    double interval = 1;
    long data_size = 56;
    long timeout_seconds = 0;
    bool quiet = false;
    int option;
    while ((option = getopt(argc, argv, "c:i:qs:t:")) != -1) {
        switch (option) {
        case 'c':
            count = strtol(optarg, nullptr, 10);
            break;
        case 'i':
            interval = atof(optarg);
            break;
        case 'q':
            quiet = true;
            break;
        case 's':
            data_size = strtol(optarg, nullptr, 10);
            break;
        case 't':
            timeout_seconds = strtol(optarg, nullptr, 10);
            break;
        default:
            fprintf(stderr, "usage: ping [-c count] [-i wait] [-q] [-s packetsize] [-t timeout] host\n");
            return 64;
        }
    }
    if (optind != argc - 1 || data_size < 8 || data_size > static_cast<long>(largest_data)) {
        fprintf(stderr, "usage: ping [-c count] [-i wait] [-q] [-s packetsize] [-t timeout] host\n");
        return 64;
    }
    char const* host = argv[optind];

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    int resolved = getaddrinfo(host, nullptr, &hints, &results);
    if (resolved != 0) {
        fprintf(stderr, "ping: cannot resolve %s: %s\n", host, gai_strerror(resolved));
        return 68;
    }
    sockaddr_in destination = *reinterpret_cast<sockaddr_in*>(results->ai_addr);
    freeaddrinfo(results);
    char address_text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &destination.sin_addr, address_text, sizeof(address_text));

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping: socket");
        return 71;
    }
    signal(SIGINT, [](int) { s_interrupted = 1; });

    printf("PING %s (%s): %ld data bytes\n", host, address_text, data_size);
    fflush(stdout);

    uint16_t const identifier = static_cast<uint16_t>(getpid());
    Statistics statistics;
    uint16_t sequence = 0;
    double next_send = now_milliseconds();
    double const started = next_send;
    double last_sent = next_send;
    for (;;) {
        if (s_interrupted)
            break;
        double now = now_milliseconds();
        if (timeout_seconds > 0 && now - started >= static_cast<double>(timeout_seconds) * 1000)
            break;
        if (count >= 0 && statistics.sent >= count && (statistics.received >= count || now - last_sent >= 1000))
            break;
        if ((count < 0 || statistics.sent < count) && now >= next_send) {
            uint8_t packet[sizeof(icmp_echo_header) + largest_data];
            auto* header = reinterpret_cast<icmp_echo_header*>(packet);
            header->type = ICMP_ECHO;
            header->code = 0;
            header->checksum = 0;
            header->identifier = htons(identifier);
            header->sequence = htons(sequence);
            // The time it left, so that a reply says how long it took.
            memcpy(packet + sizeof(icmp_echo_header), &now, sizeof(now));
            for (long i = 8; i < data_size; ++i)
                packet[sizeof(icmp_echo_header) + static_cast<size_t>(i)] = static_cast<uint8_t>(i);
            size_t size = sizeof(icmp_echo_header) + static_cast<size_t>(data_size);
            if (sendto(fd, packet, size, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) < 0) {
                fprintf(stderr, "ping: sendto: %s\n", strerror(errno));
            }
            ++statistics.sent;
            ++sequence;
            last_sent = now;
            next_send = now + interval * 1000;
        }

        double wait = next_send - now;
        if (count >= 0 && statistics.sent >= count)
            wait = 1000 - (now - last_sent);
        pollfd poller { fd, POLLIN, 0 };
        int ready = poll(&poller, 1, wait > 0 ? static_cast<int>(wait) + 1 : 0);
        if (ready <= 0)
            continue;
        uint8_t reply[2048];
        sockaddr_in from {};
        socklen_t from_size = sizeof(from);
        ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (got < static_cast<ssize_t>(sizeof(ip_header) + sizeof(icmp_echo_header)))
            continue;
        auto const* ip = reinterpret_cast<ip_header const*>(reply);
        size_t const ip_size = (ip->version_and_length & 0xf) * 4u;
        if (got < static_cast<ssize_t>(ip_size + sizeof(icmp_echo_header)))
            continue;
        auto const* header = reinterpret_cast<icmp_echo_header const*>(reply + ip_size);
        if (header->type != ICMP_ECHOREPLY || ntohs(header->identifier) != identifier)
            continue;
        size_t const data = static_cast<size_t>(got) - ip_size - sizeof(icmp_echo_header);
        double sent_at = 0;
        if (data >= sizeof(sent_at))
            memcpy(&sent_at, reply + ip_size + sizeof(icmp_echo_header), sizeof(sent_at));
        double const round_trip = now_milliseconds() - sent_at;
        ++statistics.received;
        if (statistics.received == 1 || round_trip < statistics.smallest)
            statistics.smallest = round_trip;
        if (round_trip > statistics.largest)
            statistics.largest = round_trip;
        statistics.sum += round_trip;
        statistics.squares += round_trip * round_trip;
        if (!quiet) {
            char from_text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, from_text, sizeof(from_text));
            printf("%zu bytes from %s: icmp_seq=%u ttl=%u time=%.3f ms\n", data + sizeof(icmp_echo_header), from_text, ntohs(header->sequence), ip->time_to_live, round_trip);
            fflush(stdout);
        }
    }

    printf("\n--- %s ping statistics ---\n", host);
    double const loss = statistics.sent > 0 ? 100.0 * static_cast<double>(statistics.sent - statistics.received) / static_cast<double>(statistics.sent) : 0;
    printf("%ld packets transmitted, %ld packets received, %.1f%% packet loss\n", statistics.sent, statistics.received, loss);
    if (statistics.received > 0) {
        double const average = statistics.sum / static_cast<double>(statistics.received);
        double const variance = statistics.squares / static_cast<double>(statistics.received) - average * average;
        printf("round-trip min/avg/max/stddev = %.3f/%.3f/%.3f/%.3f ms\n", statistics.smallest, average, statistics.largest, variance > 0 ? sqrt(variance) : 0.0);
    }
    return statistics.received > 0 ? 0 : 2;
}
