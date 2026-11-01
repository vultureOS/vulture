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
posix: 208 of 208 requirements met: PASS
```

The rest of `test-userland` checks the same interfaces from other sides:
the shell language (`shell`), the command-line tools against macOS's
output (`tools`), the interactive shell and job control through a
pseudo-terminal (`interactive`), terminals and sessions (`terminals`),
credentials and permissions (`permissions`), and more.

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
| Memory | `mmap` (anonymous; private and shared file mappings), `munmap`, `mprotect`, `msync`, `madvise`, `posix_madvise`, `sysconf(_SC_PAGESIZE)` |
| Environment | `getenv`, `setenv`, `unsetenv`, `environ`, `getuid` and friends, `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid`, `uname`, `gethostname`, `sysconf` |
| C library | `printf` family, `scanf` family, `strto*`, `atoi`, `qsort`, `bsearch`, the string functions, `strtok_r`, `strerror`, `strsignal`, `stdio` streams, `tmpfile`, `atexit`, `getopt`, `regcomp`/`regexec`, `fnmatch`, `glob` |

## Not yet

These POSIX interfaces are not there yet, and arrive with the milestones
that need them:

- Threads: `pthread_*`, thread-local storage, `sigaltstack` (M7, with the
  services that need them).
- Sockets and networking: `socket` and friends, `getaddrinfo` (M12; Unix
  domain sockets with M7's IPC).
- The `*at` family (`openat`, `fstatat`, `unlinkat`, ...) and `fchdir`.
- Interval timers (`setitimer`, `getitimer`, `timer_create`), `waitid`,
  `sigqueue` and real-time signals.
- Message queues, semaphores and shared memory objects (`mq_*`, `sem_*`,
  `shm_open`), asynchronous I/O (`aio_*`).
- `dlopen` and `dlsym` (dyld loads only what programs are linked with).
- Locales beyond "C"/"POSIX", wide characters and `iconv`.

Resource limits are reported and may be lowered, but are not enforced
beyond the kernel's own (64 open files per process, an 8 MiB stack).
