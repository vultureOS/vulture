/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/EventLoop.h>
#include <LibBase/Atomic.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

namespace Core {

namespace {

// Each thread has its own loop, if it wants one.
thread_local EventLoop* s_current = nullptr;

// The pipe signals come through: the handler writes the signal's number. One
// loop reads it: the first that asked for a signal.
int s_wake_pipe[2] = { -1, -1 };
Atomic<EventLoop*> s_signal_loop { nullptr };
struct sigaction s_previous_actions[NSIG];
bool s_caught[NSIG];

void catch_signal(int signal)
{
    int saved = errno;
    u8 number = static_cast<u8>(signal);
    (void)write(s_wake_pipe[1], &number, 1);
    errno = saved;
}

u64 now_milliseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1'000'000;
}

}

EventLoop::EventLoop()
{
    VERIFY(!s_current);
    s_current = this;
    [[maybe_unused]] static bool const pipe_is_made = [] {
        VERIFY(pipe2(s_wake_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
        return true;
    }();
}

EventLoop::~EventLoop()
{
    if (s_signal_loop.load() == this) {
        for (int signal = 1; signal < NSIG; ++signal) {
            if (s_caught[signal])
                restore_signal(signal);
        }
        s_signal_loop.store(nullptr);
    }
    s_current = nullptr;
}

EventLoop& EventLoop::current()
{
    VERIFY(s_current);
    return *s_current;
}

int EventLoop::exec()
{
    m_quitting = false;
    while (!m_quitting)
        pump();
    return m_exit_code;
}

void EventLoop::quit(int code)
{
    m_quitting = true;
    m_exit_code = code;
}

int EventLoop::watch(int fd, Interest interest, Function<void()> callback)
{
    int id = m_next_id++;
    m_watches.append(adopt_own(*new (Base::nothrow) Watch { id, fd, interest, move(callback) }));
    return id;
}

void EventLoop::unwatch(int id)
{
    m_watches.remove_all_matching([id](auto const& watch) { return watch->id == id; });
}

int EventLoop::add_timer(u64 milliseconds, bool repeating, Function<void()> callback)
{
    int id = m_next_id++;
    m_timers.append(adopt_own(*new (Base::nothrow) Timer { id, milliseconds, now_milliseconds() + milliseconds, repeating, move(callback) }));
    return id;
}

void EventLoop::cancel_timer(int id)
{
    m_timers.remove_all_matching([id](auto const& timer) { return timer->id == id; });
}

void EventLoop::install_signal(int signal)
{
    struct sigaction action {};
    action.sa_handler = catch_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, &s_previous_actions[signal]);
    s_caught[signal] = true;
}

void EventLoop::restore_signal(int signal)
{
    sigaction(signal, &s_previous_actions[signal], nullptr);
    s_caught[signal] = false;
}

int EventLoop::on_signal(int signal, Function<void()> callback)
{
    VERIFY(signal > 0 && signal < NSIG);
    EventLoop* expected = nullptr;
    if (!s_signal_loop.compare_exchange_strong(expected, this))
        VERIFY(expected == this);
    int id = m_next_id++;
    m_signal_handlers.append(adopt_own(*new (Base::nothrow) SignalHandler { id, signal, move(callback) }));
    if (!s_caught[signal])
        install_signal(signal);
    return id;
}

void EventLoop::remove_signal_handler(int id)
{
    int signal = 0;
    for (auto const& handler : m_signal_handlers) {
        if (handler->id == id)
            signal = handler->signal;
    }
    m_signal_handlers.remove_all_matching([id](auto const& handler) { return handler->id == id; });
    if (!signal)
        return;
    for (auto const& handler : m_signal_handlers) {
        if (handler->signal == signal)
            return;
    }
    restore_signal(signal);
}

void EventLoop::deferred_invoke(Function<void()> callback)
{
    m_deferred.append(move(callback));
}

void EventLoop::dispatch_signals()
{
    u8 numbers[64];
    for (;;) {
        ssize_t got = read(s_wake_pipe[0], numbers, sizeof(numbers));
        if (got <= 0)
            return;
        for (ssize_t i = 0; i < got; ++i) {
            // By id: a callback may remove handlers, its own included.
            Vector<int> ids;
            for (auto const& handler : m_signal_handlers) {
                if (handler->signal == numbers[i])
                    ids.append(handler->id);
            }
            for (int id : ids) {
                for (auto const& handler : m_signal_handlers) {
                    if (handler->id == id) {
                        auto callback = move(handler->callback);
                        callback();
                        // Put it back, unless the handler was removed.
                        for (auto const& again : m_signal_handlers) {
                            if (again->id == id)
                                again->callback = move(callback);
                        }
                        break;
                    }
                }
            }
        }
    }
}

void EventLoop::dispatch_timers()
{
    u64 now = now_milliseconds();
    // The ones due, earliest first.
    Vector<int> due;
    for (auto const& timer : m_timers) {
        if (timer->deadline <= now)
            due.append(timer->id);
    }
    for (size_t i = 1; i < due.size(); ++i) {
        for (size_t j = i; j > 0; --j) {
            u64 a = 0, b = 0;
            for (auto const& timer : m_timers) {
                if (timer->id == due[j - 1])
                    a = timer->deadline;
                if (timer->id == due[j])
                    b = timer->deadline;
            }
            if (a <= b)
                break;
            swap(due[j - 1], due[j]);
        }
    }
    for (int id : due) {
        Function<void()> callback;
        bool repeating = false;
        for (auto const& timer : m_timers) {
            if (timer->id != id)
                continue;
            repeating = timer->repeating;
            if (repeating) {
                timer->deadline += timer->interval;
                // Far behind (the program was stopped): from now on.
                if (timer->deadline <= now)
                    timer->deadline = now + timer->interval;
            }
            callback = move(timer->callback);
            break;
        }
        if (!callback)
            continue; // Cancelled by an earlier callback.
        if (!repeating)
            cancel_timer(id);
        callback();
        if (repeating) {
            for (auto const& timer : m_timers) {
                if (timer->id == id)
                    timer->callback = move(callback);
            }
        }
    }
}

void EventLoop::pump(bool wait)
{
    // Deferred calls made by these run next round.
    auto deferred = move(m_deferred);
    for (auto& callback : deferred)
        callback();

    int timeout = -1;
    if (!wait || !m_deferred.is_empty() || m_quitting) {
        timeout = 0;
    } else if (!m_timers.is_empty()) {
        u64 now = now_milliseconds();
        u64 nearest = m_timers[0]->deadline;
        for (auto const& timer : m_timers)
            nearest = min(nearest, timer->deadline);
        timeout = nearest <= now ? 0 : static_cast<int>(min<u64>(nearest - now, 24 * 60 * 60 * 1000));
    }

    Vector<pollfd> pollers;
    Vector<int> ids;
    pollers.append({ s_signal_loop.load() == this ? s_wake_pipe[0] : -1, POLLIN, 0 });
    ids.append(0);
    for (auto const& watch : m_watches) {
        short events = watch->interest == Interest::Read ? POLLIN : POLLOUT;
        pollers.append({ watch->fd, events, 0 });
        ids.append(watch->id);
    }
    int ready = poll(pollers.data(), pollers.size(), timeout);
    if (ready < 0 && errno != EINTR)
        ready = 0;

    if (ready > 0 && pollers[0].revents)
        dispatch_signals();
    for (size_t i = 1; ready > 0 && i < pollers.size(); ++i) {
        if (!pollers[i].revents)
            continue;
        for (auto const& watch : m_watches) {
            if (watch->id != ids[i])
                continue;
            auto callback = move(watch->callback);
            int id = watch->id;
            callback();
            for (auto const& again : m_watches) {
                if (again->id == id)
                    again->callback = move(callback);
            }
            break;
        }
    }
    dispatch_timers();
}

}
