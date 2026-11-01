/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/termios.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef vulture_tcflag_t tcflag_t;
typedef vulture_cc_t cc_t;
typedef vulture_speed_t speed_t;

// The kernel's vulture_termios, under its standard name.
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

int tcgetattr(int fd, struct termios*);
int tcsetattr(int fd, int action, struct termios const*);
int tcflush(int fd, int queue);
int tcdrain(int fd);
int tcsendbreak(int fd, int duration);
void cfmakeraw(struct termios*);
speed_t cfgetispeed(struct termios const*);
speed_t cfgetospeed(struct termios const*);
int cfsetispeed(struct termios*, speed_t);
int cfsetospeed(struct termios*, speed_t);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t);
pid_t tcgetsid(int fd);

__END_DECLS
