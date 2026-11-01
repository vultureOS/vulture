# System Calls

How programs ask the kernel for things. The list itself lives in one place,
[`Kernel/API/Syscalls.def`](../Kernel/API/Syscalls.def); the kernel builds
its dispatcher and LibSystem its numbers from it, so they cannot disagree.

## Calling convention (AArch64)

- `SVC #0`, with the call number in `x8` and up to six arguments in
  `x0`-`x5`.
- The result comes back in `x0`. A value between -4095 and -1 is a negated
  `errno`; LibC turns it into `-1` and sets `errno`.
- Every other register is preserved.

Numbers are positions in `Syscalls.def`: only ever append to it.

## Rules the kernel keeps

- **User pointers are checked.** An address outside user space, or memory
  the program cannot access in that way, fails with `EFAULT`; it never makes
  the kernel touch kernel memory on a program's behalf, and never crashes
  it. Copies go through `copy_from_user`/`copy_to_user`, whose faults the
  exception handler recovers from (the `__ex_table` section).
- **Signals interrupt sleeps.** `nanosleep` and `waitpid` return `EINTR`
  when a signal that is not ignored arrives.
- **Ignored signals are discarded when sent** (POSIX), so they interrupt
  nothing.

## The calls

| Call | Arguments | Notes |
|---|---|---|
| `exit` | status | Ends the process; the parent's `waitpid` sees the status |
| `fork` | | The child shares every page copy-on-write |
| `execve` | path, argv, envp | ELF programs: position-independent ones (`ET_DYN`, loaded at 0x400000) with the dynamic linker their `PT_INTERP` names, or static ones (`ET_EXEC`); `#!` scripts run their interpreter. The auxiliary vector gives `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`, the user and group IDs, `AT_SECURE` and `AT_EXECFN` |
| `waitpid` | pid (-1: any; 0 or -pgid: a group), status*, options | `WNOHANG`, `WUNTRACED`, `WCONTINUED` |
| `getpid`, `getppid`, `gettid` | | |
| `kill` | pid (0, -1, -pgid: groups), signal | Only to processes the caller may signal |
| `sigaction`, `sigprocmask`, `sigreturn` | | LibC always supplies a restorer (`SA_RESTORER`) |
| `sigsuspend`, `sigpending`, `alarm` | | |
| `setpgid`, `getpgid`, `setsid`, `getsid` | | Job control |
| `read`, `write` | fd, buffer, size | |
| `pread`, `pwrite` | fd, buffer, size, offset | At the offset given; the file's own offset stays |
| `open` | path, flags, mode | `O_NOFOLLOW` refuses a symbolic link (`ELOOP`); a session leader without a terminal gets the first it opens |
| `close`, `dup`, `dup2`, `fcntl` | | |
| `lseek` | fd, offset, origin | `ESPIPE` on pipes and terminals |
| `fstat`, `stat`, `lstat` | fd or path, `vulture_stat*` | |
| `pipe` | fds*, flags | |
| `poll` | pollfd*, count, milliseconds | Files, pipes, terminals |
| `ioctl` | fd, request, argument | Terminals: termios, window size, foreground group, controlling terminal, pseudo-terminals |
| `mkdir`, `rmdir`, `unlink`, `rename`, `link`, `symlink`, `readlink` | | |
| `getcwd`, `chdir`, `getdents` | | |
| `fsync`, `ftruncate`, `sync` | | |
| `access`, `umask`, `chmod`, `fchmod`, `chown`, `fchown` | | Permissions are checked as POSIX says |
| `utimensat`, `futimens` | | |
| `getuid`, `geteuid`, `getgid`, `getegid`, `setuid`, `setgid`, `seteuid`, `setegid`, `getgroups`, `setgroups` | | Set-ID programs run as the file's owner and group |
| `mmap` | address, length, protection, flags, fd, offset | Anonymous and file mappings, private or shared; writable and executable at once is refused (W^X) |
| `munmap`, `mprotect` | address, length (, protection) | |
| `sched_yield`, `clock_gettime`, `nanosleep` | | |
| `uname`, `getrandom`, `sysctl` | | `sysctl` by name: `kern.*` (including `kern.proc.all`, every process), `hw.*`, `vfs.mounts` |
| `reboot` | how | Root: restart or power off (vlaunchd does it, after stopping every job) |
| `power_off` | | Process 1 only, for the test boots |

The shared constants and structures (errno values, signal numbers, `mmap`
flags, `vulture_stat`, the signal frame) are in
[`Kernel/API/POSIX/`](../Kernel/API/POSIX).

## Signal delivery

On the way back to user mode the kernel delivers the lowest pending signal
that is not blocked. For a handler it pushes a `vulture_signal_frame` (all
registers, the old signal mask, a `siginfo`) on the user stack and calls
`handler(signal, &info, &frame)` with `x19` pointing at the frame and the
link register at the restorer, which calls `sigreturn`. `sigreturn`
restores the registers from the frame, keeping only the condition flags of
the saved processor state, so a forged frame cannot raise privilege.
Default actions terminate (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGABRT`,
`SIGFPE`, `SIGTRAP` and `SIGQUIT` with a crash report) or ignore
(`SIGCHLD`, `SIGCONT`, `SIGURG`, `SIGWINCH`).
