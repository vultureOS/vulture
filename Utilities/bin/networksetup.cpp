/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/socket.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>

// networksetup -listallhardwareports | -listallnetworkservices | -getinfo SERVICE | -getdnsservers SERVICE: how the network is
// set up, as a Mac says it. Changing it comes with M32.

namespace {

char const* service_name(char const* interface_name)
{
    return strcmp(interface_name, "lo0") == 0 ? nullptr : "Ethernet";
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: networksetup -listallhardwareports | -listallnetworkservices | -getinfo service\n");
        return 1;
    }
    vulture_interface_info interfaces[16];
    size_t size = sizeof(interfaces);
    if (sysctlbyname("net.interfaces", interfaces, &size, nullptr, 0) != 0) {
        warn("networksetup", "net.interfaces");
        return 1;
    }
    size_t const count = size / sizeof(interfaces[0]);
    if (strcmp(argv[1], "-listallhardwareports") == 0) {
        putchar('\n');
        for (size_t i = 0; i < count; ++i) {
            char const* service = service_name(interfaces[i].name);
            if (!service)
                continue;
            auto const* mac = interfaces[i].mac;
            printf("Hardware Port: %s\nDevice: %s\nEthernet Address: %02x:%02x:%02x:%02x:%02x:%02x\n\n", service, interfaces[i].name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        return 0;
    }
    if (strcmp(argv[1], "-listallnetworkservices") == 0) {
        puts("An asterisk (*) denotes that a network service is disabled.");
        for (size_t i = 0; i < count; ++i) {
            if (char const* service = service_name(interfaces[i].name))
                puts(service);
        }
        return 0;
    }
    if (strcmp(argv[1], "-getinfo") == 0 && argc == 3) {
        for (size_t i = 0; i < count; ++i) {
            char const* service = service_name(interfaces[i].name);
            if (!service || strcmp(service, argv[2]) != 0)
                continue;
            auto const print_address = [](char const* label, unsigned address) {
                in_addr value {};
                value.s_addr = address;
                printf("%s: %s\n", label, address ? inet_ntoa(value) : "none");
            };
            puts(interfaces[i].address ? "DHCP Configuration" : "Manual Configuration");
            print_address("IP address", interfaces[i].address);
            print_address("Subnet mask", interfaces[i].netmask);
            print_address("Router", interfaces[i].gateway);
            auto const* mac = interfaces[i].mac;
            printf("Ethernet Address: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return 0;
        }
        printf("%s is not a recognized network service.\n", argv[2]);
        return 4;
    }
    fprintf(stderr, "usage: networksetup -listallhardwareports | -listallnetworkservices | -getinfo service\n");
    return 1;
}
