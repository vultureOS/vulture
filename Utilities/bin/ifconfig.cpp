/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

// ifconfig [interface [address netmask [gateway]]]: what the network interfaces are, and for
// root, what they should be.

namespace {

void print_interface(vulture_interface_info const& info)
{
    printf("%s: flags=%x<UP,%s,RUNNING> mtu %u\n", info.name, info.flags, (info.flags & VULTURE_IFF_LOOPBACK) ? "LOOPBACK" : "BROADCAST", info.mtu);
    if (!(info.flags & VULTURE_IFF_LOOPBACK))
        printf("\tether %02x:%02x:%02x:%02x:%02x:%02x\n", info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
    if (info.address != 0) {
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &info.address, address, sizeof(address));
        printf("\tinet %s netmask 0x%08x", address, ntohl(info.netmask));
        if (!(info.flags & VULTURE_IFF_LOOPBACK)) {
            in_addr_t broadcast = info.address | ~info.netmask;
            char broadcast_text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &broadcast, broadcast_text, sizeof(broadcast_text));
            printf(" broadcast %s", broadcast_text);
        }
        printf("\n");
    }
    if (info.gateway != 0) {
        char gateway[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &info.gateway, gateway, sizeof(gateway));
        printf("\tgateway %s\n", gateway);
    }
    printf("\tpackets: %llu in, %llu out, %llu dropped; bytes: %llu in, %llu out\n", info.received_packets, info.sent_packets, info.dropped_packets, info.received_bytes, info.sent_bytes);
    printf("\tstatus: %s\n", (info.flags & VULTURE_IFF_LOOPBACK) || info.address != 0 ? "active" : "inactive");
}

}

int main(int argc, char** argv)
{
    vulture_interface_info interfaces[16];
    size_t size = sizeof(interfaces);
    if (sysctlbyname("net.interfaces", interfaces, &size, nullptr, 0) != 0) {
        perror("ifconfig");
        return 1;
    }
    size_t const count = size / sizeof(interfaces[0]);

    if (argc >= 4) {
        vulture_interface_config config {};
        strncpy(config.name, argv[1], sizeof(config.name) - 1);
        if (inet_pton(AF_INET, argv[2], &config.address) != 1 || inet_pton(AF_INET, argv[3], &config.netmask) != 1
            || (argc >= 5 && inet_pton(AF_INET, argv[4], &config.gateway) != 1)) {
            fprintf(stderr, "ifconfig: bad address\n");
            return 1;
        }
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0 || ioctl(fd, VULTURE_SIOCSIFCONFIG, &config) != 0) {
            fprintf(stderr, "ifconfig: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        return 0;
    }

    bool found = argc < 2;
    for (size_t i = 0; i < count; ++i) {
        if (argc < 2 || strcmp(argv[1], interfaces[i].name) == 0) {
            print_interface(interfaces[i]);
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "ifconfig: interface %s does not exist\n", argv[1]);
        return 1;
    }
    return 0;
}
