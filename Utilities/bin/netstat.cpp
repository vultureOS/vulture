/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

// netstat [-an]: every socket with an address. (-a shows listening sockets too, and -n is what
// it always does: it never looks names up.)

namespace {

void format_endpoint(unsigned address, unsigned short port, char* out, size_t size)
{
    char text[INET_ADDRSTRLEN];
    if (address == 0) {
        strcpy(text, "*");
    } else {
        inet_ntop(AF_INET, &address, text, sizeof(text));
    }
    if (port == 0)
        snprintf(out, size, "%s.*", text);
    else
        snprintf(out, size, "%s.%u", text, ntohs(port));
}

char const* state_name(unsigned state)
{
    static char const* const names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    return state < 11 ? names[state] : "?";
}

}

int main(int argc, char** argv)
{
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && strchr(argv[i], 'a'))
            all = true;
    }
    size_t size = 0;
    if (sysctlbyname("net.connections", nullptr, &size, nullptr, 0) != 0) {
        perror("netstat");
        return 1;
    }
    size += 16 * sizeof(vulture_connection_info);
    auto* rows = static_cast<vulture_connection_info*>(malloc(size));
    if (!rows || sysctlbyname("net.connections", rows, &size, nullptr, 0) != 0) {
        perror("netstat");
        return 1;
    }
    printf("Active Internet connections%s\n", all ? " (including servers)" : "");
    printf("%-6s %6s %6s  %-23s %-23s %s\n", "Proto", "Recv-Q", "Send-Q", "Local Address", "Foreign Address", "(state)");
    for (size_t i = 0; i < size / sizeof(rows[0]); ++i) {
        auto const& row = rows[i];
        bool const is_tcp = row.protocol == IPPROTO_TCP;
        if (!all && (!is_tcp || row.state == VULTURE_TCP_LISTEN))
            continue;
        char local[32];
        char remote[32];
        format_endpoint(row.local_address, row.local_port, local, sizeof(local));
        format_endpoint(row.remote_address, row.remote_port, remote, sizeof(remote));
        printf("%-6s %6u %6u  %-23s %-23s %s\n", is_tcp ? "tcp4" : "udp4", row.receive_queue, row.send_queue, local, remote, is_tcp ? state_name(row.state) : "");
    }
    return 0;
}
