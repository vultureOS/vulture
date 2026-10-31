/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Terminal settings (termios) and the terminal ioctls, shared by the
// kernel and LibC. The values follow the common Unix numbering.

typedef unsigned int vulture_tcflag_t;
typedef unsigned char vulture_cc_t;
typedef unsigned int vulture_speed_t;

#define NCCS 32

struct vulture_termios {
    vulture_tcflag_t c_iflag;
    vulture_tcflag_t c_oflag;
    vulture_tcflag_t c_cflag;
    vulture_tcflag_t c_lflag;
    vulture_cc_t c_line;
    vulture_cc_t c_cc[NCCS];
    vulture_speed_t c_ispeed;
    vulture_speed_t c_ospeed;
};

struct vulture_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// c_cc indices.
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

// c_iflag
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXANY 0004000
#define IXOFF 0010000
#define IMAXBEL 0020000
#define IUTF8 0040000

// c_oflag
#define OPOST 0000001
#define ONLCR 0000004
#define OCRNL 0000010
#define ONOCR 0000020
#define ONLRET 0000040

// c_cflag
#define CSIZE 0000060
#define CS5 0000000
#define CS6 0000020
#define CS7 0000040
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL 0002000
#define CLOCAL 0004000
#define B0 0
#define B9600 0000015
#define B38400 0000017
#define B115200 0010002

// c_lflag
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE 0004000
#define IEXTEN 0100000

// tcsetattr() actions.
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

// tcflush() queues.
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

// ioctl() requests.
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCFLSH 0x540B
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define TIOCNOTTY 0x5422
#define TIOCGSID 0x5429
#define TIOCGPTN 0x80045430
#define TIOCSPTLCK 0x40045431
#define FIONBIO 0x5421
