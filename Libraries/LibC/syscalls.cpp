/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/port.h>
#include <sys/stat.h>
#include <sys/vulture.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using System::argument;
using System::Syscall;

// A negative result is -errno: set errno and report failure the POSIX way.
template<typename T = long>
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

static void to_stat(vulture_stat const& source, struct stat* destination)
{
    destination->st_dev = source.st_dev;
    destination->st_ino = source.st_ino;
    destination->st_mode = source.st_mode;
    destination->st_nlink = source.st_nlink;
    destination->st_uid = source.st_uid;
    destination->st_gid = source.st_gid;
    destination->st_size = source.st_size;
    destination->st_blksize = source.st_blksize;
    destination->st_blocks = source.st_blocks;
    destination->st_rdev = source.st_rdev;
    destination->st_atim = { source.st_atime_seconds, source.st_atime_nanoseconds };
    destination->st_mtim = { source.st_mtime_seconds, source.st_mtime_nanoseconds };
    destination->st_ctim = { source.st_ctime_seconds, source.st_ctime_nanoseconds };
    destination->st_birthtimespec = { source.st_birthtime_seconds, source.st_birthtime_nanoseconds };
}

extern "C" {

int execve(char const* path, char* const argv[], char* const envp[])
{
    return check<int>(call(Syscall::execve, path, argv, envp));
}

int execv(char const* path, char* const argv[])
{
    return execve(path, argv, environ);
}

int execvp(char const* file, char* const argv[])
{
    if (strchr(file, '/'))
        return execv(file, argv);
    static char const* const directories[] = { "/bin/", "/usr/bin/", "/sbin/" };
    char path[256];
    for (char const* directory : directories) {
        size_t directory_length = strlen(directory);
        size_t file_length = strlen(file);
        if (directory_length + file_length + 1 > sizeof(path))
            continue;
        memcpy(path, directory, directory_length);
        memcpy(path + directory_length, file, file_length + 1);
        execv(path, argv);
        if (errno != ENOENT)
            return -1;
    }
    errno = ENOENT;
    return -1;
}

pid_t getpid()
{
    return static_cast<pid_t>(call(Syscall::getpid));
}

pid_t getppid()
{
    return static_cast<pid_t>(call(Syscall::getppid));
}

pid_t gettid()
{
    return static_cast<pid_t>(call(Syscall::gettid));
}

ssize_t read(int fd, void* buffer, size_t count)
{
    return check<ssize_t>(call(Syscall::read, fd, buffer, count));
}

int port_create(int fds[2], int flags)
{
    return check<int>(call(Syscall::port_create, fds, flags));
}

int port_make_send(int receive_right, int flags)
{
    return check<int>(call(Syscall::port_make_send, receive_right, flags));
}

int port_send(int send_right, struct vulture_port_message const* message, int flags)
{
    return check<int>(call(Syscall::port_send, send_right, message, flags));
}

int port_receive(int receive_right, struct vulture_port_received* received, int flags)
{
    return check<int>(call(Syscall::port_receive, receive_right, received, flags));
}

int port_call(int send_right, struct vulture_port_message const* message, struct vulture_port_received* answer, int flags)
{
    return check<int>(call(Syscall::port_call, send_right, message, answer, flags));
}

int bootstrap_port_get(int flags)
{
    return check<int>(call(Syscall::bootstrap_port, -1, flags));
}

int bootstrap_port_set(int send_right)
{
    return check<int>(call(Syscall::bootstrap_port, send_right, 0));
}

ssize_t pread(int fd, void* buffer, size_t count, off_t offset)
{
    return check<ssize_t>(call(Syscall::pread, fd, buffer, count, offset));
}

ssize_t pwrite(int fd, void const* buffer, size_t count, off_t offset)
{
    return check<ssize_t>(call(Syscall::pwrite, fd, buffer, count, offset));
}

ssize_t write(int fd, void const* buffer, size_t count)
{
    return check<ssize_t>(call(Syscall::write, fd, buffer, count));
}

int open(char const* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return check<int>(call(Syscall::open, path, flags, mode));
}

int close(int fd)
{
    return check<int>(call(Syscall::close, fd));
}

off_t lseek(int fd, off_t offset, int origin)
{
    return check<off_t>(call(Syscall::lseek, fd, offset, origin));
}

int dup(int fd)
{
    return check<int>(call(Syscall::dup, fd));
}

int dup2(int fd, int new_fd)
{
    return check<int>(call(Syscall::dup2, fd, new_fd));
}

int pipe2(int fds[2], int flags)
{
    return check<int>(call(Syscall::pipe, fds, flags));
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

int fsync(int fd)
{
    return check<int>(call(Syscall::fsync, fd));
}

int ftruncate(int fd, off_t length)
{
    return check<int>(call(Syscall::ftruncate, fd, length));
}

void sync()
{
    call(Syscall::sync);
}

int unlink(char const* path)
{
    return check<int>(call(Syscall::unlink, path));
}

int remove(char const* path)
{
    if (unlink(path) == 0)
        return 0;
    if (errno == EISDIR)
        return rmdir(path);
    return -1;
}

int rmdir(char const* path)
{
    return check<int>(call(Syscall::rmdir, path));
}

int mkdir(char const* path, mode_t mode)
{
    return check<int>(call(Syscall::mkdir, path, mode));
}

int rename(char const* old_path, char const* new_path)
{
    return check<int>(call(Syscall::rename, old_path, new_path));
}

int chdir(char const* path)
{
    return check<int>(call(Syscall::chdir, path));
}

char* getcwd(char* buffer, size_t size)
{
    if (check<long>(call(Syscall::getcwd, buffer, size)) < 0)
        return nullptr;
    return buffer;
}

ssize_t readlink(char const* path, char* buffer, size_t size)
{
    return check<ssize_t>(call(Syscall::readlink, path, buffer, size));
}

int symlink(char const* target, char const* link_path)
{
    return check<int>(call(Syscall::symlink, target, link_path));
}

int access(char const* path, int mode)
{
    return check<int>(call(Syscall::access, path, mode));
}

int fcntl(int fd, int command, ...)
{
    va_list arguments;
    va_start(arguments, command);
    unsigned long argument = va_arg(arguments, unsigned long);
    va_end(arguments);
    return check<int>(call(Syscall::fcntl, fd, command, argument));
}

int lstat(char const* path, struct stat* result)
{
    vulture_stat raw {};
    if (check<int>(call(Syscall::lstat, path, &raw)) < 0)
        return -1;
    to_stat(raw, result);
    return 0;
}

int fstat(int fd, struct stat* result)
{
    vulture_stat raw {};
    if (check<int>(call(Syscall::fstat, fd, &raw)) < 0)
        return -1;
    to_stat(raw, result);
    return 0;
}

int stat(char const* path, struct stat* result)
{
    vulture_stat raw {};
    if (check<int>(call(Syscall::stat, path, &raw)) < 0)
        return -1;
    to_stat(raw, result);
    return 0;
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return check<pid_t>(call(Syscall::waitpid, pid, status, options));
}

pid_t wait(int* status)
{
    return waitpid(-1, status, 0);
}

int kill(pid_t pid, int signal)
{
    return check<int>(call(Syscall::kill, pid, signal));
}

int raise(int signal)
{
    // The calling thread takes it, so a crash report shows where abort() was.
    return check<int>(call(Syscall::thread_kill, call(Syscall::gettid), signal));
}

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset)
{
    long result = call(Syscall::mmap, address, length, protection, flags, fd, offset);
    if (result < 0 && result > -4096) {
        errno = static_cast<int>(-result);
        return MAP_FAILED;
    }
    return reinterpret_cast<void*>(result);
}

int munmap(void* address, size_t length)
{
    return check<int>(call(Syscall::munmap, address, length));
}

int mprotect(void* address, size_t length, int protection)
{
    return check<int>(call(Syscall::mprotect, address, length, protection));
}

// The kernel reads and writes timespecs directly (it checks the pointers).
static_assert(sizeof(timespec) == sizeof(vulture_timespec) && __builtin_offsetof(timespec, tv_nsec) == __builtin_offsetof(vulture_timespec, tv_nsec));

int clock_gettime(clockid_t clock, struct timespec* result)
{
    return check<int>(call(Syscall::clock_gettime, clock, result));
}

int nanosleep(struct timespec const* duration, struct timespec* remaining)
{
    if (check<int>(call(Syscall::nanosleep, duration)) < 0)
        return -1;
    if (remaining) {
        remaining->tv_sec = 0;
        remaining->tv_nsec = 0;
    }
    return 0;
}

time_t time(time_t* result)
{
    struct timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    if (result)
        *result = now.tv_sec;
    return now.tv_sec;
}

unsigned sleep(unsigned seconds)
{
    struct timespec duration { static_cast<time_t>(seconds), 0 };
    nanosleep(&duration, nullptr);
    return 0;
}

int usleep(useconds_t microseconds)
{
    struct timespec duration { static_cast<time_t>(microseconds / 1'000'000), static_cast<long>(microseconds % 1'000'000) * 1000 };
    return nanosleep(&duration, nullptr);
}

int sched_yield()
{
    return check<int>(call(Syscall::sched_yield));
}

int vulture_power_off()
{
    return check<int>(call(Syscall::power_off));
}
}
