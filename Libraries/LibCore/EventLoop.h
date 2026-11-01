/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>

namespace Core {

// A program's run loop, over poll(): it waits for file descriptors to be
// ready, timers to come due and signals to arrive, and calls back for
// each. Signals are caught and handed over through a pipe, so their
// callbacks run like any other (not inside a signal handler). Callbacks
// may add or remove anything, the one running included.
//
// One per process: signals have nowhere else to go.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(EventLoop const&) = delete;
    EventLoop& operator=(EventLoop const&) = delete;

    // The process's loop (there must be one).
    static EventLoop& current();

    // Runs until quit(); returns the code given to it.
    int exec();
    void quit(int code = 0);
    [[nodiscard]] bool is_quitting() const { return m_quitting; }
    // One round: waits (unless told not to) until something happens, then
    // handles everything that has.
    void pump(bool wait = true);

    enum class Interest : u8 {
        Read = 1,
        Write = 2,
    };
    // Called while `fd` is ready (readable, or writable); also at end of
    // file and on errors, so the callback can find out by reading.
    int watch(int fd, Interest, Function<void()>);
    void unwatch(int id);

    // Called after `milliseconds`, once or every time.
    int add_timer(u64 milliseconds, bool repeating, Function<void()>);
    void cancel_timer(int id);

    // Called when `signal` arrives; while any are set, the signal is
    // caught, and afterwards it does what it did before.
    int on_signal(int signal, Function<void()>);
    void remove_signal_handler(int id);

    // Called at the start of the next round, in the order given.
    void deferred_invoke(Function<void()>);

private:
    struct Watch {
        int id;
        int fd;
        Interest interest;
        Function<void()> callback;
    };
    struct Timer {
        int id;
        u64 interval;
        u64 deadline;
        bool repeating;
        Function<void()> callback;
    };
    struct SignalHandler {
        int id;
        int signal;
        Function<void()> callback;
    };

    void dispatch_signals();
    void dispatch_timers();
    void install_signal(int signal);
    void restore_signal(int signal);

    Vector<OwnPtr<Watch>> m_watches;
    Vector<OwnPtr<Timer>> m_timers;
    Vector<OwnPtr<SignalHandler>> m_signal_handlers;
    Vector<Function<void()>> m_deferred;
    int m_next_id { 1 };
    bool m_quitting { false };
    int m_exit_code { 0 };
};

}
