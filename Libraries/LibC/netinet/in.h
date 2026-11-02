/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

#define INET_ADDRSTRLEN 16

__BEGIN_DECLS

static inline uint16_t htons(uint16_t value) { return __builtin_bswap16(value); }
static inline uint16_t ntohs(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t htonl(uint32_t value) { return __builtin_bswap32(value); }
static inline uint32_t ntohl(uint32_t value) { return __builtin_bswap32(value); }

__END_DECLS
