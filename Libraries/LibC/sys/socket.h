/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/socket.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef unsigned int socklen_t;
typedef unsigned char sa_family_t;

int socket(int domain, int type, int protocol);
int bind(int fd, struct sockaddr const* address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* address, socklen_t* length);
int connect(int fd, struct sockaddr const* address, socklen_t length);
ssize_t send(int fd, void const* buffer, size_t length, int flags);
ssize_t sendto(int fd, void const* buffer, size_t length, int flags, struct sockaddr const* to, socklen_t to_length);
ssize_t recv(int fd, void* buffer, size_t length, int flags);
ssize_t recvfrom(int fd, void* buffer, size_t length, int flags, struct sockaddr* from, socklen_t* from_length);
int getsockname(int fd, struct sockaddr* address, socklen_t* length);
int getpeername(int fd, struct sockaddr* address, socklen_t* length);
int setsockopt(int fd, int level, int name, void const* value, socklen_t length);
int getsockopt(int fd, int level, int name, void* value, socklen_t* length);
int shutdown(int fd, int how);
int socketpair(int domain, int type, int protocol, int fds[2]);

// A message in several pieces, and what comes with it: open files, in a control message of type SCM_RIGHTS.
#define msghdr vulture_msghdr
#define cmsghdr vulture_cmsghdr
#define iovec vulture_iovec
ssize_t sendmsg(int fd, struct msghdr const*, int flags);
ssize_t recvmsg(int fd, struct msghdr*, int flags);
int getpeereid(int fd, uid_t*, gid_t*);

#define CMSG_ALIGN(size) (((size) + 3u) & ~3u)
#define CMSG_DATA(control) ((unsigned char*)(control) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_SPACE(size) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(size))
#define CMSG_LEN(size) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (size))
#define CMSG_FIRSTHDR(message) ((message)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(message)->msg_control : (struct cmsghdr*)0)
#define CMSG_NXTHDR(message, control)                                                                                                                       \
    (((unsigned char*)(control) + CMSG_ALIGN((control)->cmsg_len) + CMSG_ALIGN(sizeof(struct cmsghdr)) > (unsigned char*)(message)->msg_control + (message)->msg_controllen) \
            ? (struct cmsghdr*)0                                                                                                                            \
            : (struct cmsghdr*)((unsigned char*)(control) + CMSG_ALIGN((control)->cmsg_len)))

__END_DECLS
