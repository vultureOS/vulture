/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// BSD sockets, with the numbers and structures macOS has for them.

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_UNIX
#define PF_INET AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
// With socket()'s type: set on the new descriptor at once.
#define SOCK_NONBLOCK 0x10000000
#define SOCK_CLOEXEC 0x20000000

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_BROADCAST 0x0020
#define SO_REUSEPORT 0x0200
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008
#define SO_NOSIGPIPE 0x1022

#define TCP_NODELAY 0x01

// With IPPROTO_IP.
#define IP_TTL 4
#define IP_BOUND_IF 25

#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_EOR 0x8
#define MSG_TRUNC 0x10
#define MSG_CTRUNC 0x20
#define MSG_WAITALL 0x40
#define MSG_DONTWAIT 0x80
#define MSG_NOSIGNAL 0x80000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOMAXCONN 128

#define INADDR_ANY 0x00000000u
#define INADDR_LOOPBACK 0x7f000001u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_NONE 0xffffffffu

struct sockaddr {
    unsigned char sa_len;
    unsigned char sa_family;
    char sa_data[14];
};

struct in_addr {
    unsigned int s_addr; // Network order.
};

struct sockaddr_in {
    unsigned char sin_len;
    unsigned char sin_family;
    unsigned short sin_port; // Network order.
    struct in_addr sin_addr;
    char sin_zero[8];
};

// A Unix-domain socket's name: a path.
struct vulture_sockaddr_un {
    unsigned char sun_len;
    unsigned char sun_family;
    char sun_path[104];
};

// getsockopt's, at level SOL_LOCAL.
#define VULTURE_SOL_LOCAL 0
#define VULTURE_LOCAL_PEERCRED 1

struct vulture_xucred {
    unsigned int cr_version;
    unsigned int cr_uid;
    short cr_ngroups;
    unsigned int cr_groups[16];
};

struct vulture_iovec {
    void* iov_base;
    unsigned long iov_len;
};

struct vulture_msghdr {
    void* msg_name;
    unsigned int msg_namelen;
    struct vulture_iovec* msg_iov;
    int msg_iovlen;
    void* msg_control;
    unsigned int msg_controllen;
    int msg_flags;
};

struct vulture_cmsghdr {
    unsigned int cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

// Open files, in a control message at level SOL_SOCKET.
#define SCM_RIGHTS 1

#define VULTURE_IOV_MAX 1024

struct sockaddr_storage {
    unsigned char ss_len;
    unsigned char ss_family;
    char ss_pad[126];
};

// Interfaces, for ioctl on any socket. Names are as macOS has them: en0, lo0.
#define VULTURE_IFNAMSIZ 16
#define VULTURE_IFF_UP 0x1
#define VULTURE_IFF_BROADCAST 0x2
#define VULTURE_IFF_LOOPBACK 0x8
#define VULTURE_IFF_RUNNING 0x40
#define VULTURE_IFF_MULTICAST 0x8000

// sysctl "net.interfaces": one for each. Addresses are in network order.
struct vulture_interface_info {
    char name[VULTURE_IFNAMSIZ];
    unsigned char mac[6];
    unsigned short mtu;
    unsigned int flags;
    unsigned int address;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int reserved;
    unsigned long long received_packets;
    unsigned long long received_bytes;
    unsigned long long sent_packets;
    unsigned long long sent_bytes;
    unsigned long long dropped_packets;
};

// sysctl "net.connections": every socket with an address. Network order, as above.
struct vulture_connection_info {
    unsigned int protocol;
    unsigned int local_address;
    unsigned int remote_address;
    unsigned short local_port;
    unsigned short remote_port;
    unsigned int state; // One of the below, for TCP.
    unsigned int receive_queue;
    unsigned int send_queue;
};

#define VULTURE_TCP_CLOSED 0
#define VULTURE_TCP_LISTEN 1
#define VULTURE_TCP_SYN_SENT 2
#define VULTURE_TCP_SYN_RECEIVED 3
#define VULTURE_TCP_ESTABLISHED 4
#define VULTURE_TCP_FIN_WAIT_1 5
#define VULTURE_TCP_FIN_WAIT_2 6
#define VULTURE_TCP_CLOSE_WAIT 7
#define VULTURE_TCP_CLOSING 8
#define VULTURE_TCP_LAST_ACK 9
#define VULTURE_TCP_TIME_WAIT 10

// ioctl(socket, VULTURE_SIOCSIFCONFIG, struct vulture_interface_config*), for root: sets an
// interface's address, netmask and default gateway. All zero takes the address away.
#define VULTURE_SIOCSIFCONFIG 0x8000692c

struct vulture_interface_config {
    char name[VULTURE_IFNAMSIZ];
    unsigned int address;
    unsigned int netmask;
    unsigned int gateway;
};
