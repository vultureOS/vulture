/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static_assert(sizeof(struct termios) == sizeof(vulture_termios));
static_assert(sizeof(struct winsize) == sizeof(vulture_winsize));

extern "C" {

int ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    va_start(arguments, request);
    void* argument = va_arg(arguments, void*);
    va_end(arguments);
    long result = System::syscall(System::Syscall::ioctl, static_cast<long>(fd), static_cast<long>(request), reinterpret_cast<long>(argument));
    if (result < 0 && result > -4096) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<int>(result);
}

int tcgetattr(int fd, struct termios* settings)
{
    return ioctl(fd, TCGETS, settings);
}

int tcsetattr(int fd, int action, struct termios const* settings)
{
    unsigned long request;
    switch (action) {
    case TCSANOW:
        request = TCSETS;
        break;
    case TCSADRAIN:
        request = TCSETSW;
        break;
    case TCSAFLUSH:
        request = TCSETSF;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, request, settings);
}

int tcflush(int fd, int queue)
{
    return ioctl(fd, TCFLSH, reinterpret_cast<void*>(static_cast<long>(queue)));
}

int tcdrain(int fd)
{
    // Output reaches the device as it is written.
    return isatty(fd) ? 0 : -1;
}

void cfmakeraw(struct termios* settings)
{
    settings->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    settings->c_oflag &= ~OPOST;
    settings->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    settings->c_cflag &= ~(CSIZE | PARENB);
    settings->c_cflag |= CS8;
    settings->c_cc[VMIN] = 1;
    settings->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(struct termios const* settings) { return settings->c_ispeed; }
speed_t cfgetospeed(struct termios const* settings) { return settings->c_ospeed; }

int cfsetispeed(struct termios* settings, speed_t speed)
{
    settings->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios* settings, speed_t speed)
{
    settings->c_ospeed = speed;
    return 0;
}

pid_t tcgetpgrp(int fd)
{
    int group = 0;
    if (ioctl(fd, TIOCGPGRP, &group) < 0)
        return -1;
    return group;
}

int tcsetpgrp(int fd, pid_t group)
{
    int value = group;
    return ioctl(fd, TIOCSPGRP, &value);
}

pid_t tcgetsid(int fd)
{
    int session = 0;
    if (ioctl(fd, TIOCGSID, &session) < 0)
        return -1;
    return session;
}

// 1, or 0 with errno ENOTTY (or EBADF), as macOS's does.
int isatty(int fd)
{
    struct termios settings;
    return tcgetattr(fd, &settings) == 0;
}

int posix_openpt(int flags)
{
    return open("/dev/ptmx", flags);
}

// Pseudo-terminals are ready as made; this checks that `fd` is a master.
int grantpt(int fd)
{
    unsigned number = 0;
    if (ioctl(fd, TIOCGPTN, &number) < 0) {
        if (errno == ENOTTY)
            errno = EINVAL;
        return -1;
    }
    return 0;
}

int unlockpt(int fd)
{
    int unlock = 0;
    return ioctl(fd, TIOCSPTLCK, &unlock);
}

int ptsname_r(int fd, char* buffer, size_t size)
{
    unsigned number = 0;
    if (ioctl(fd, TIOCGPTN, &number) < 0)
        return errno;
    if (static_cast<size_t>(snprintf(buffer, size, "/dev/ttys%03u", number)) >= size)
        return ERANGE;
    return 0;
}

char* ptsname(int fd)
{
    static char name[32];
    int error = ptsname_r(fd, name, sizeof(name));
    if (error) {
        errno = error;
        return nullptr;
    }
    return name;
}

int ttyname_r(int fd, char* buffer, size_t size)
{
    if (!isatty(fd))
        return ENOTTY;
    // A pseudo-terminal's master answers TIOCGPTN; a slave is found by
    // comparing device numbers with /dev entries.
    struct stat information;
    if (fstat(fd, &information) < 0)
        return errno;
    unsigned major = static_cast<unsigned>(information.st_rdev >> 8);
    unsigned minor = static_cast<unsigned>(information.st_rdev & 0xff);
    int written;
    if (major == 16)
        written = snprintf(buffer, size, "/dev/ttys%03u", minor);
    else if (major == 5 && minor == 1)
        written = snprintf(buffer, size, "/dev/console");
    else
        return ENOTTY;
    return static_cast<size_t>(written) >= size ? ERANGE : 0;
}

char* ttyname(int fd)
{
    static char name[32];
    int error = ttyname_r(fd, name, sizeof(name));
    if (error) {
        errno = error;
        return nullptr;
    }
    return name;
}

pid_t setsid(void)
{
    long result = System::syscall(System::Syscall::setsid);
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<pid_t>(result);
}

pid_t getsid(pid_t pid)
{
    long result = System::syscall(System::Syscall::getsid, static_cast<long>(pid));
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<pid_t>(result);
}

int setpgid(pid_t pid, pid_t group)
{
    long result = System::syscall(System::Syscall::setpgid, static_cast<long>(pid), static_cast<long>(group));
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return 0;
}

pid_t getpgid(pid_t pid)
{
    long result = System::syscall(System::Syscall::getpgid, static_cast<long>(pid));
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<pid_t>(result);
}

pid_t getpgrp(void)
{
    return getpgid(0);
}
}
