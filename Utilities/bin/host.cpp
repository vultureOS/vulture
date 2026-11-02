/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>

// host name: what the name resolves to.

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: host name\n");
        return 1;
    }
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    int error = getaddrinfo(argv[1], nullptr, &hints, &results);
    if (error != 0) {
        fprintf(stderr, "Host %s not found: %s\n", argv[1], gai_strerror(error));
        return 1;
    }
    for (auto* info = results; info; info = info->ai_next) {
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr, text, sizeof(text));
        printf("%s has address %s\n", argv[1], text);
    }
    freeaddrinfo(results);
    return 0;
}
