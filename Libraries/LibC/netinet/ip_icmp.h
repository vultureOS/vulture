/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <netinet/in.h>

#define ICMP_ECHOREPLY 0
#define ICMP_UNREACH 3
#define ICMP_ECHO 8

struct icmp_echo_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
};

struct ip_header {
    uint8_t version_and_length;
    uint8_t type_of_service;
    uint16_t total_length;
    uint16_t identification;
    uint16_t fragment;
    uint8_t time_to_live;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t source;
    uint32_t destination;
};
