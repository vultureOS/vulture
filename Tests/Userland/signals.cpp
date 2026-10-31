/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int volatile s_usr1_count = 0;
static int volatile s_last_signal = 0;
static void* volatile s_fault_address = nullptr;
static int volatile s_children_exited = 0;
static jmp_buf s_recover;

int main()
{
    // A plain handler runs, and the program carries on after it.
    signal(SIGUSR1, [](int signal) {
        s_usr1_count = s_usr1_count + 1;
        s_last_signal = signal;
    });
    CHECK(raise(SIGUSR1) == 0);
    CHECK(s_usr1_count == 1 && s_last_signal == SIGUSR1);

    // Blocked signals wait until unblocked.
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigset_t old_mask;
    CHECK(sigprocmask(SIG_BLOCK, &block, &old_mask) == 0);
    raise(SIGUSR1);
    CHECK(s_usr1_count == 1);
    CHECK(sigprocmask(SIG_SETMASK, &old_mask, nullptr) == 0);
    CHECK(s_usr1_count == 2);

    // Ignored signals vanish.
    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);

    // A SA_SIGINFO handler learns where a fault happened, and can escape it.
    struct sigaction action {};
    action.sa_sigaction = [](int signal, siginfo_t* info, void*) {
        s_last_signal = signal;
        s_fault_address = info->si_addr;
        longjmp(s_recover, 1);
    };
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    CHECK(sigaction(SIGSEGV, &action, nullptr) == 0);
    if (setjmp(s_recover) == 0) {
        *reinterpret_cast<int volatile*>(0x1234) = 1;
        CHECK(false);
    }
    CHECK(s_last_signal == SIGSEGV);
    CHECK(s_fault_address == reinterpret_cast<void*>(0x1234));

    // SIGCHLD reports a child's end; SIGKILL cannot be caught.
    signal(SIGCHLD, [](int) { s_children_exited = s_children_exited + 1; });
    CHECK(signal(SIGKILL, SIG_IGN) == SIG_ERR);
    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            sleep(1);
    }
    usleep(10'000);
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(s_children_exited == 1);

    // A child that terminates on SIGTERM (default action).
    pid = fork();
    if (pid == 0) {
        for (;;)
            sleep(1);
    }
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(kill(99999, SIGTERM) == -1);
    return finish();
}
