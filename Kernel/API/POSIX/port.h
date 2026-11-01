/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Ports: vultureOS's message queues between processes, after Mach's. A
// port has one receive right and any number of send rights; both are file
// descriptors (so poll() and the run loop watch them). A message carries
// bytes, file descriptors (rights to ports, or any open file), a send right
// to reply to, and out-of-line memory (shared copy-on-write, so large data
// moves without copying). Receivers learn who sent it.

#define VULTURE_PORT_MAX_INLINE (64 * 1024)
#define VULTURE_PORT_MAX_RIGHTS 16
#define VULTURE_PORT_MAX_REGIONS 8
#define VULTURE_PORT_QUEUE_LIMIT 64

// port_send / port_receive / port_call flags.
#define VULTURE_PORT_NONBLOCK 0x1
// Out-of-line memory and descriptors are taken from the sender (unmapped,
// closed) rather than shared with it.
#define VULTURE_PORT_MOVE 0x2

struct vulture_port_region {
    void* address;      // Page-aligned.
    unsigned long size; // A multiple of the page size.
};

struct vulture_port_message {
    unsigned id;
    unsigned flags;
    void const* bytes;
    unsigned long size;
    int const* rights;
    unsigned right_count;
    int reply; // A send right to put in the message for the answer, or -1.
    struct vulture_port_region const* regions;
    unsigned region_count;
};

struct vulture_port_received {
    unsigned id;
    unsigned flags;
    void* bytes;
    unsigned long capacity; // In: room at `bytes`.
    unsigned long size;     // Out: the message's size (also when too big).
    int* rights;
    unsigned right_capacity;
    unsigned right_count;
    int reply; // A send right to answer through, or -1. (port_call failing
               // with EMSGSIZE: the receive right the answer waits on.)
    struct vulture_port_region* regions; // Out: where the memory now is.
    unsigned region_capacity;
    unsigned region_count;
    int sender_pid;
    unsigned sender_uid;
    unsigned sender_gid;
};
