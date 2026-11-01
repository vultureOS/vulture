/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Supervisor.h"
#include "Log.h"
#include <LibCore/Account.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace Launchd {

namespace {

u64 now_milliseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1'000'000;
}

// How launchd words a job's end in the log, or nothing for a clean exit:
// "Service exited with abnormal code: 1", "Service exited due to SIGSEGV".
void describe_end(int status, char* text, size_t size)
{
    text[0] = '\0';
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        snprintf(text, size, "Service exited with abnormal code: %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        char name[16] = {};
        char const* lower = sys_signame[WTERMSIG(status)];
        for (size_t i = 0; lower[i] && i < sizeof(name) - 1; ++i)
            name[i] = static_cast<char>(lower[i] >= 'a' && lower[i] <= 'z' ? lower[i] - 'a' + 'A' : lower[i]);
        snprintf(text, size, "Service exited due to SIG%s", name);
    }
}

// In the child: the job's world, then its program.
[[noreturn]] void become_job(Job const& job)
{
    // Its own session, with the signals as a fresh program has them.
    setsid();
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    for (int signal = 1; signal < NSIG; ++signal) {
        if (signal != SIGKILL && signal != SIGSTOP)
            ::signal(signal, SIG_DFL);
    }
    umask(job.umask.value_or(022));

    auto open_standard = [](Optional<String> const& path, int target, int flags) {
        char const* name = path.has_value() ? path->characters() : "/dev/null";
        int fd = open(name, flags, 0644);
        if (fd < 0)
            fd = open("/dev/null", O_RDWR);
        if (fd >= 0 && fd != target) {
            dup2(fd, target);
            close(fd);
        }
    };
    open_standard(job.standard_in, STDIN_FILENO, O_RDONLY);
    open_standard(job.standard_out, STDOUT_FILENO, O_WRONLY | O_CREAT | O_APPEND);
    open_standard(job.standard_error, STDERR_FILENO, O_WRONLY | O_CREAT | O_APPEND);

    clearenv();
    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    if (job.user_name.has_value()) {
        auto account = Core::Account::from_name(job.user_name->view());
        if (account.is_error()) {
            log("(%s) Unknown user: %s", job.label.characters(), job.user_name->characters());
            _exit(78);
        }
        auto const& user = account.value();
        auto set = [](char const* key, StringView value) {
            char text[1100];
            snprintf(text, sizeof(text), "%.*s", static_cast<int>(value.length()), value.characters_without_null_termination());
            setenv(key, text, 1);
        };
        set("HOME", user.home());
        set("USER", user.name());
        set("LOGNAME", user.name());
        set("SHELL", user.shell());
        if (job.group_name.has_value()) {
            if (struct group* group = getgrnam(job.group_name->characters()))
                setgid(group->gr_gid);
        }
        if (getuid() == 0 && user.become().is_error())
            _exit(78);
    } else if (job.group_name.has_value()) {
        if (struct group* group = getgrnam(job.group_name->characters()))
            setgid(group->gr_gid);
    }
    for (auto const& variable : job.environment)
        putenv(const_cast<char*>(variable.characters()));
    if (chdir(job.working_directory.has_value() ? job.working_directory->characters() : "/") < 0)
        (void)chdir("/");

    auto** argv = static_cast<char**>(calloc(job.arguments.size() + 1, sizeof(char*)));
    for (size_t i = 0; i < job.arguments.size(); ++i)
        argv[i] = const_cast<char*>(job.arguments[i].characters());
    execv(job.program.characters(), argv);
    log("(%s) Could not run %s: %s", job.label.characters(), job.program.characters(), strerror(errno));
    _exit(78);
}

}

Supervisor::Supervisor(Core::EventLoop& loop)
    : m_loop(loop)
{
    m_loop.on_signal(SIGCHLD, [this] { reap(); });
}

void Supervisor::load_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log("Could not read %s: %s", path, strerror(errno));
        return;
    }
    Vector<char> text;
    char buffer[4096];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < got; ++i)
            text.append(buffer[i]);
    }
    close(fd);
    JSON::ParseError where;
    auto plist = JSON::parse(StringView(text.data(), text.size()), &where);
    if (plist.is_error()) {
        log("%s:%zu:%zu: %s", path, where.line, where.column, where.message);
        return;
    }
    if (plist.value().get_boolean("Disabled"_sv).value_or(false))
        return;
    char const* error = "";
    auto job = Job::from_plist(plist.value(), error);
    if (!job) {
        log("%s: %s", path, error);
        return;
    }
    for (auto const& existing : m_jobs) {
        if (existing->label == job->label) {
            log("%s: a job named %s is loaded already", path, job->label.characters());
            return;
        }
    }
    m_jobs.append(move(job));
}

void Supervisor::load_directory(char const* directory)
{
    DIR* listing = opendir(directory);
    if (!listing)
        return;
    Vector<String> names;
    while (struct dirent* entry = readdir(listing)) {
        StringView name(entry->d_name);
        if (name.ends_with(".plist"_sv) && !name.starts_with('.'))
            names.append(MUST(String::from_view(name)));
    }
    closedir(listing);
    for (size_t i = 1; i < names.size(); ++i) {
        for (size_t j = i; j > 0 && strcmp(names[j - 1].characters(), names[j].characters()) > 0; --j)
            swap(names[j - 1], names[j]);
    }
    for (auto const& name : names) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", directory, name.characters());
        load_file(path);
    }
}

void Supervisor::start()
{
    for (auto& job : m_jobs) {
        Job* pointer = job.ptr();
        // KeepAlive of any kind implies RunAtLoad: whether to run again
        // depends on how a run ended.
        if (job->run_at_load || job->keep_alive != Job::KeepAlive::Never)
            spawn(*job);
        if (job->start_interval > 0) {
            job->interval_timer = m_loop.add_timer(static_cast<u64>(job->start_interval) * 1000, true, [this, pointer] {
                if (!pointer->pid && !m_stopping)
                    spawn(*pointer);
            });
        }
    }
}

void Supervisor::spawn(Job& job)
{
    if (job.pid || m_stopping)
        return;
    job.restart_timer = 0;
    pid_t pid = fork();
    if (pid < 0) {
        log("(%s) Could not start: %s", job.label.characters(), strerror(errno));
        return;
    }
    if (pid == 0)
        become_job(job);
    job.pid = pid;
    job.started_at = now_milliseconds();
    job.last_started_at = job.started_at;
    ++job.runs;
}

void Supervisor::reap()
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        for (auto& job : m_jobs) {
            if (job->pid == pid) {
                job_ended(*job, status);
                break;
            }
        }
        // Anyone else was an orphan handed to us: reaped, nothing more.
    }
    if (m_stopping)
        check_stopped();
}

void Supervisor::job_ended(Job& job, int status)
{
    char text[128];
    describe_end(status, text, sizeof(text));
    if (text[0] && !m_stopping)
        log("(%s[%d]) %s", job.label.characters(), job.pid, text);
    job.pid = 0;
    job.last_status = status;
    if (m_stopping || !job.should_restart_after(status))
        return;
    // Not more often than ThrottleInterval allows.
    u64 ran_for = now_milliseconds() - job.started_at;
    u64 throttle = static_cast<u64>(job.throttle_interval) * 1000;
    Job* pointer = &job;
    if (ran_for >= throttle) {
        m_loop.deferred_invoke([this, pointer] { spawn(*pointer); });
        return;
    }
    u64 delay = throttle - ran_for;
    if (job.throttle_interval >= 2)
        log("(%s) Service only ran for %llu seconds. Pushing respawn out by %llu seconds.", job.label.characters(), static_cast<unsigned long long>(ran_for / 1000), static_cast<unsigned long long>((delay + 999) / 1000));
    job.restart_timer = m_loop.add_timer(delay, false, [this, pointer] { spawn(*pointer); });
}

void Supervisor::stop_everything(Function<void()> then)
{
    m_stopping = true;
    m_after_stopping = move(then);
    for (auto& job : m_jobs) {
        if (job->restart_timer)
            m_loop.cancel_timer(job->restart_timer);
        if (job->interval_timer)
            m_loop.cancel_timer(job->interval_timer);
        job->restart_timer = job->interval_timer = 0;
        if (!job->pid)
            continue;
        // The whole session the job leads, then SIGKILL after its own
        // ExitTimeOut if it is still there.
        kill(-job->pid, SIGTERM);
        kill(job->pid, SIGTERM);
        kill(-job->pid, SIGCONT);
        Job* pointer = job.ptr();
        pid_t pid = job->pid;
        m_loop.add_timer(static_cast<u64>(job->exit_timeout) * 1000, false, [pointer, pid] {
            if (pointer->pid != pid)
                return;
            log("(%s[%d]) Exited with SIGKILL after not stopping in time", pointer->label.characters(), pid);
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        });
    }
    // Nothing running: done at once.
    m_loop.deferred_invoke([this] { check_stopped(); });
}

void Supervisor::check_stopped()
{
    for (auto const& job : m_jobs) {
        if (job->pid)
            return;
    }
    if (m_after_stopping) {
        auto then = move(m_after_stopping);
        then();
    }
}

}
