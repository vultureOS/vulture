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

## The calls (M4)

| Call | Arguments | Notes |
|---|---|---|
| `exit` | status | Ends the process; the parent's `waitpid` sees the status |
| `write` | fd, buffer, size | The console (fds 0-2) writes to the kernel log |
| `read` | fd, buffer, size | Boot archive files; the console reads as end of file until M6 |
| `open` | path, flags, mode | Read-only boot archive (writes fail with `EROFS`) |
| `close` | fd | |
| `lseek` | fd, offset, origin | `ESPIPE` on the console |
| `fstat`, `stat` | fd or path, `vulture_stat*` | |
| `getpid`, `getppid`, `gettid` | | |
| `fork` | | The child shares every page copy-on-write |
| `execve` | path, argv, envp | Static ELF executables (`ET_EXEC`) from the boot archive |
| `waitpid` | pid (-1: any), status*, options (`WNOHANG`) | |
| `kill` | pid, signal | Process groups arrive with job control (M6) |
| `sigaction` | signal, new*, old* | LibC always supplies a restorer (`SA_RESTORER`) |
| `sigprocmask` | how, set*, old* | |
| `sigreturn` | | Called by the restorer when a handler returns |
| `mmap` | address, length, protection, flags, fd, offset | Anonymous private memory; writable+executable is refused (W^X) |
| `munmap`, `mprotect` | address, length (, protection) | |
| `sched_yield` | | |
| `clock_gettime` | clock, timespec* | `CLOCK_REALTIME` counts from boot until there is an RTC driver |
| `nanosleep` | timespec* | |
| `power_off` | | Process 1 only |

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
