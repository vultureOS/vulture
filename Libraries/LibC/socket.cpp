/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/socket.h>
#include <LibSystem/Syscall.h>
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

using System::argument;
using System::Syscall;

template<typename T = int>
static T check(long result)
{
    if (result < 0 && result > -4096) {
        errno = static_cast<int>(-result);
        return static_cast<T>(-1);
    }
    return static_cast<T>(result);
}

template<typename... Args>
static long call(Syscall number, Args... arguments)
{
    return System::syscall(number, argument(arguments)...);
}

extern "C" {

int socket(int domain, int type, int protocol)
{
    return check(call(Syscall::socket, domain, type, protocol));
}

int bind(int fd, struct sockaddr const* address, socklen_t length)
{
    return check(call(Syscall::bind, fd, address, length));
}

int listen(int fd, int backlog)
{
    return check(call(Syscall::listen, fd, backlog));
}

int accept(int fd, struct sockaddr* address, socklen_t* length)
{
    return check(call(Syscall::accept, fd, address, length, 0));
}

int connect(int fd, struct sockaddr const* address, socklen_t length)
{
    return check(call(Syscall::connect, fd, address, length));
}

ssize_t sendto(int fd, void const* buffer, size_t length, int flags, struct sockaddr const* to, socklen_t to_length)
{
    return check<ssize_t>(call(Syscall::sendto, fd, buffer, length, flags, to, to_length));
}

ssize_t send(int fd, void const* buffer, size_t length, int flags)
{
    return sendto(fd, buffer, length, flags, nullptr, 0);
}

ssize_t recvfrom(int fd, void* buffer, size_t length, int flags, struct sockaddr* from, socklen_t* from_length)
{
    return check<ssize_t>(call(Syscall::recvfrom, fd, buffer, length, flags, from, from_length));
}

ssize_t recv(int fd, void* buffer, size_t length, int flags)
{
    return recvfrom(fd, buffer, length, flags, nullptr, nullptr);
}

int getsockname(int fd, struct sockaddr* address, socklen_t* length)
{
    return check(call(Syscall::getsockname, fd, address, length));
}

int getpeername(int fd, struct sockaddr* address, socklen_t* length)
{
    return check(call(Syscall::getpeername, fd, address, length));
}

int setsockopt(int fd, int level, int name, void const* value, socklen_t length)
{
    return check(call(Syscall::setsockopt, fd, level, name, value, length));
}

int getsockopt(int fd, int level, int name, void* value, socklen_t* length)
{
    return check(call(Syscall::getsockopt, fd, level, name, value, length));
}

int shutdown(int fd, int how)
{
    return check(call(Syscall::shutdown, fd, how));
}

int inet_pton(int family, char const* text, void* address)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned parts[4];
    int part = 0;
    char const* at = text;
    for (;;) {
        if (*at < '0' || *at > '9')
            return 0;
        unsigned value = 0;
        int digits = 0;
        while (*at >= '0' && *at <= '9') {
            value = value * 10 + static_cast<unsigned>(*at - '0');
            ++digits;
            ++at;
            if (value > 255 || digits > 3)
                return 0;
        }
        parts[part++] = value;
        if (part == 4)
            break;
        if (*at != '.')
            return 0;
        ++at;
    }
    if (*at != '\0')
        return 0;
    uint32_t value = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
    value = htonl(value);
    memcpy(address, &value, sizeof(value));
    return 1;
}

char const* inet_ntop(int family, void const* address, char* buffer, socklen_t size)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    uint8_t const* bytes = static_cast<uint8_t const*>(address);
    char text[INET_ADDRSTRLEN];
    int length = snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    if (static_cast<socklen_t>(length) >= size) {
        errno = ENOSPC;
        return nullptr;
    }
    memcpy(buffer, text, static_cast<size_t>(length) + 1);
    return buffer;
}

in_addr_t inet_addr(char const* text)
{
    in_addr_t value;
    if (inet_pton(AF_INET, text, &value) != 1)
        return INADDR_NONE;
    return value;
}

int inet_aton(char const* text, struct in_addr* address)
{
    return inet_pton(AF_INET, text, address) == 1 ? 1 : 0;
}

char* inet_ntoa(struct in_addr address)
{
    static char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address, buffer, sizeof(buffer));
    return buffer;
}

unsigned long getauxval(unsigned long type)
{
    if (type != AT_HWCAP)
        return 0;
    static unsigned long capabilities = 0;
    static bool known = false;
    if (!known) {
        size_t size = sizeof(capabilities);
        if (sysctlbyname("hw.optional.hwcap", &capabilities, &size, nullptr, 0) != 0)
            capabilities = 0;
        known = true;
    }
    return capabilities;
}

unsigned int if_nametoindex(char const* name)
{
    vulture_interface_info infos[16];
    size_t size = sizeof(infos);
    if (sysctlbyname("net.interfaces", infos, &size, nullptr, 0) != 0)
        return 0;
    for (size_t i = 0; i < size / sizeof(infos[0]); ++i) {
        if (strcmp(infos[i].name, name) == 0)
            return static_cast<unsigned>(i + 1);
    }
    return 0;
}

char* if_indextoname(unsigned int index, char* name)
{
    vulture_interface_info infos[16];
    size_t size = sizeof(infos);
    if (sysctlbyname("net.interfaces", infos, &size, nullptr, 0) != 0 || index == 0 || index > size / sizeof(infos[0])) {
        errno = ENXIO;
        return nullptr;
    }
    strncpy(name, infos[index - 1].name, IF_NAMESIZE);
    return name;
}

}
