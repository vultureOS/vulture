# POSIX Conformance

vultureOS follows POSIX.1-2017 where it has the interface, as macOS does,
and says plainly where it does not yet. What it has is checked by our POSIX
conformance subset, `Tests/Userland/posix.cpp`, which runs in
`ninja check` (`test-userland`): each check is one requirement POSIX makes
of an interface, and a failure names it.

```
posix: processes     26 of  26
posix: signals       19 of  19
posix: files         50 of  50
posix: directories   15 of  15
posix: pipes         10 of  10
posix: terminals     12 of  12
posix: time          15 of  15
posix: memory        15 of  15
posix: environment   16 of  16
posix: library       30 of  30
posix: at calls      44 of  44
posix: descriptors   29 of  29
posix: vectored io   18 of  18
posix: unix sockets  65 of  65
posix: socket pairs  24 of  24
posix: passing files  18 of  18
posix: kqueue        54 of  54
posix: select        13 of  13
posix: fifos         22 of  22
posix: shared memory  26 of  26
posix: interval timers  23 of  23
posix: restarting    22 of  22
posix: cancellation  28 of  28
posix: shared locks  25 of  25
posix: numbers as text  99 of  99
posix: dynamic loading  17 of  17
posix: wide characters  47 of  47
posix: floating point  21 of  21
posix: character sets  12 of  12
posix: 871 of 871 requirements met: PASS
```

The rest of `test-userland` checks the same interfaces from other sides:
the shell language (`shell`), the command-line tools against macOS's
output (`tools`), the interactive shell and job control through a
pseudo-terminal (`interactive`), terminals and sessions (`terminals`),
credentials and permissions (`permissions`), threads (`threads`), BSD sockets
(`sockets`), and more.

## What the subset covers

| Area | Interfaces |
|---|---|
| Processes | `fork`, `_exit`, `exit`, `execve` and the `exec` family, `waitpid` (`WNOHANG`, `WUNTRACED`, `WCONTINUED`), `getpid`, `getppid`, `setpgid`, `getpgid`, `getpgrp`, `setsid`, `getsid`, `kill`, `posix_spawn`, `posix_spawnp` (file actions; `SETPGROUP`, `SETSIGMASK`, `SETSIGDEF`, `RESETIDS`), `getrlimit`, `setrlimit` |
| Signals | `sigaction` (with `SA_SIGINFO`), `sigprocmask`, `sigpending`, `sigsuspend`, `raise`, `alarm`, `pause`, `signal`, the default actions, `SIGCHLD`, `SIGPIPE`; ignored signals stay ignored across `exec`, caught ones return to their default |
| Files | `open` (`O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_NOFOLLOW`, `O_DIRECTORY`, `O_CLOEXEC`, `O_NONBLOCK`), `read`, `write`, `pread`, `pwrite`, `lseek`, `close`, `dup`, `dup2`, `fcntl` (`F_DUPFD`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL`), close-on-exec, `ftruncate`, `truncate`, `fstat`, `stat`, `lstat`, `link`, `unlink`, `rename`, `symlink`, `readlink`, `access`, `umask`, `chmod`, `fchmod`, `chown`, `fchown`, `utimensat`, `futimens`, `fsync`, `fdatasync`, `NAME_MAX` |
| Directories | `mkdir`, `rmdir`, `opendir`, `readdir`, `rewinddir`, `closedir`, `chdir`, `getcwd` |
| Pipes | `pipe`, `pipe2`, `poll`, end of file, `EAGAIN`, `EPIPE`, `ESPIPE` |
| Terminals | `posix_openpt`, `grantpt`, `unlockpt`, `ptsname`, `isatty`, `ttyname`, `tcgetattr`, `tcsetattr`, `tcflush`, `tcdrain`, `tcsendbreak`, `cfgetispeed`/`cfgetospeed`, canonical and non-canonical input, `tcgetpgrp`/`tcsetpgrp` and job control |
| Time | `time`, `clock_gettime`, `clock_getres` (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`), `nanosleep`, `gmtime`, `localtime`, `mktime`, `timegm`, `strftime`, `strptime`, `difftime` |
| Memory | `mmap` (anonymous and file mappings, private and shared), `munmap`, `mprotect`, `msync`, `madvise`, `posix_madvise`, `shm_open`, `shm_unlink`, `sysconf(_SC_PAGESIZE)` |
| Environment | `getenv`, `setenv`, `unsetenv`, `environ`, `getuid` and friends, `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid`, `uname`, `gethostname`, `sysconf` |
| C library | `printf` family, `scanf` family, `strto*`, `atoi`, `qsort`, `bsearch`, the string functions, `strtok_r`, `strerror`, `strsignal`, `stdio` streams, `tmpfile`, `atexit`, `getopt`, `regcomp`/`regexec`, `fnmatch`, `glob` |
| Relative paths | `openat`, `fstatat`, `mkdirat`, `unlinkat`, `renameat`, `readlinkat`, `symlinkat`, `linkat`, `faccessat`, `fchmodat`, `fchownat`, `utimensat`, `fchdir`, `fcntl(F_GETPATH)` |
| Descriptors | `getrlimit`, `setrlimit` and `RLIMIT_NOFILE` as they're enforced, `sysconf(_SC_OPEN_MAX)`, `getdtablesize`, `F_DUPFD`, `F_DUPFD_CLOEXEC`; `readv`, `writev` |
| Sockets between processes | `AF_UNIX` streams and datagrams, `bind`, `listen`, `accept`, `connect`, `socketpair`, `sendmsg`, `recvmsg`, `SCM_RIGHTS`, `LOCAL_PEERCRED`, `getpeereid`, `SO_NOSIGPIPE`; `mkfifo` and what opening one waits for |
| Waiting | `kqueue`, `kevent` (`EVFILT_READ`, `WRITE`, `VNODE`, `PROC`, `SIGNAL`, `TIMER`, `USER`), `select`, `pselect`, and system calls restarting after a signal, or not, as `SA_RESTART` says |
| Timers | `setitimer`, `getitimer` (`ITIMER_REAL`, `ITIMER_VIRTUAL`, `ITIMER_PROF`) |
| Threads | Cancellation (`pthread_cancel`, `pthread_setcancelstate`, `pthread_setcanceltype`, `pthread_testcancel`, and the cancellation points); `PTHREAD_PROCESS_SHARED` mutexes and conditions; `sem_open`, `sem_close`, `sem_unlink` |
| Numbers | `printf` of `%f %e %g %a` and `strtod` exactly: what's printed is the true value correctly rounded, and what's read is the nearest number there is |
| Text | `wchar.h` and `wctype.h`, `iconv`, `fenv.h` |
| Programs | `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr`, including thread-local data, unloading, and failing cleanly |

## Threads

`Tests/Userland/threads.cpp` checks them, with the process as a whole:

| Area | Interfaces |
|---|---|
| Threads | `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_exit`, `pthread_self`, `pthread_equal`, `pthread_once`, `pthread_atfork`, `pthread_attr_*` (detach state, stack size and address, guard size); macOS's `pthread_threadid_np`, `pthread_setname_np`, `pthread_getname_np`, `pthread_main_np`, `pthread_get_stackaddr_np`, `pthread_get_stacksize_np`, `pthread_yield_np`; `sched_yield` |
| Synchronization | `pthread_mutex_*` (normal, recursive, error-checking, timed), `pthread_cond_*` (timed, on either clock), `pthread_rwlock_*`, `pthread_barrier_*`, `pthread_spin_*`, unnamed `sem_*` |
| Thread-specific data | `pthread_key_*`, cleanup handlers, C and C++ `thread_local` with destructors, thread-safe function-local statics; `errno` per thread |
| Signals | `pthread_sigmask`, `pthread_kill`, `sigwait`, `sigwaitinfo`, `sigtimedwait`, `sigaltstack` |
| The process | `exit` and `exec` from any thread end the others, however they are occupied; a fault in any thread ends the process; stopping stops them all; `fork` makes a child of one thread, with `malloc` and stdio usable; `flockfile` and the `_unlocked` stdio calls |

## Not yet

These POSIX interfaces are not there yet, and nothing here needs them so far:

- `timer_create`, `waitid`, `sigqueue` and real-time signals.
- Message queues (`mq_*`) and asynchronous I/O (`aio_*`).
- `pthread_setschedparam` and the rest of POSIX's scheduling. What a thread's work is for is said a Mac's way, with `pthread_set_qos_class_self_np`.

Of the resource limits, `RLIMIT_NOFILE` and `RLIMIT_CORE` do what they say. The others are kept, and reported, but nothing enforces them yet.

## Locales

`setlocale` knows the locales in `/usr/share/locale`, and `strftime`, `localeconv` and `nl_langinfo` go by them. Text is UTF-8 whichever it is, and `strcoll` goes by bytes, as a Mac's does for UTF-8; for anything better, there's LibLocale. `iconv` has Unicode's own forms, ASCII, the ISO-8859 sets, the Windows and Mac code pages, KOI8, Shift_JIS, EUC-JP, EUC-KR, GBK, GB18030 and Big5, from the Encoding Standard's tables.

## Compared with a real Mac

The same checks also run on the Mac this is built on, compiled for it from the same source, and every difference has to be explained: `ninja test-oracle` fails otherwise, and also if an explanation no longer applies. See `Tests/Parity/posix-differences.json`. On macOS 26.4, of the 865 checks that run on both, this system meets all of them and a Mac meets 856. Of the 9 it doesn't, 2 are checks assuming this system (they run as root, and look up its account), 4 are where macOS itself differs from POSIX (`SIGBUS` rather than `SIGSEGV`, `ENODEV` rather than `ENOTTY`, `fcntl` with no such command, and a thread that goes on running through `PTHREAD_CANCEL_ASYNCHRONOUS`), 2 are choices (the system's name, and 4 KiB pages everywhere, where Apple silicon has 16 KiB), and 1 is something to change here: `/tmp` should be a symbolic link to `/private/tmp`, as it comes with M19's volumes.

What commands say gets the same treatment: `Tests/Userland/commands.cpp` runs 90 of them, on the same files, on both, and of those 83 say exactly the same. The other 7 are in `Tests/Parity/command-differences.json`, each with why.
