/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Supervisor.h"
#include "Log.h"
#include <LibCore/Account.h>
#include <dirent.h>
#include <sandbox.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/port.h>
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

    bool debug_logging = getenv("VULTURE_DEBUG");
    clearenv();
    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    if (debug_logging)
        setenv("VULTURE_DEBUG", "1", 1);
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
    if (job.sandbox_profile.has_value()) {
        char* problem = nullptr;
        if (sandbox_init_for_program(job.sandbox_profile->characters(), SANDBOX_NAMED, job.program.characters(), &problem) != 0) {
            log("(%s) Could not start it in the sandbox %s: %s", job.label.characters(), job.sandbox_profile->characters(), problem ? problem : "?");
            _exit(78);
        }
    }
    execv(job.program.characters(), argv);
    log("(%s) Could not run %s: %s", job.label.characters(), job.program.characters(), strerror(errno));
    _exit(78);
}

}

Supervisor::Supervisor(Core::EventLoop& loop)
    : m_loop(loop)
{
    m_loop.on_signal(SIGCHLD, [this] { reap(); });
    // Our bootstrap port becomes everyone's we start.
    auto bootstrap = IPC::create_port();
    if (bootstrap.is_error()) {
        log("Could not make the bootstrap port: %s", strerror(bootstrap.error().code()));
        return;
    }
    if (bootstrap_port_set(bootstrap.value().send.fd()) < 0)
        log("Could not set the bootstrap port: %s", strerror(errno));
    m_bootstrap = adopt_own(*new (Base::nothrow) IPC::Listener(m_loop, move(bootstrap.value().receive), [this](IPC::ReceivedMessage& message) {
        if (auto result = handle(message); result.is_error())
            log("Bootstrap: a malformed message from process %d", message.sender.pid);
    }));
}

Supervisor::Service* Supervisor::find_service(StringView name)
{
    for (auto& service : m_services) {
        if (service->name.view() == name)
            return service.ptr();
    }
    return nullptr;
}

void Supervisor::watch_for_demand(Service& service)
{
    if (service.watch || !service.job || !service.receive.is_valid() || m_stopping)
        return;
    Service* pointer = &service;
    service.watch = m_loop.watch(service.receive.fd(), Core::EventLoop::Interest::Read, [this, pointer] {
        m_loop.unwatch(pointer->watch);
        pointer->watch = 0;
        Job& job = *pointer->job;
        if (job.pid || job.restart_timer || m_stopping)
            return;
        // On demand, but no more often than the throttle allows.
        u64 since = now_milliseconds() - job.last_started_at;
        u64 throttle = static_cast<u64>(job.throttle_interval) * 1000;
        if (job.runs > 0 && since < throttle) {
            Job* waiting = &job;
            job.restart_timer = m_loop.add_timer(throttle - since, false, [this, waiting] { spawn(*waiting); });
            return;
        }
        spawn(job);
    });
}

void Supervisor::stop_watching(Job& job)
{
    for (auto& service : m_services) {
        if (service->job == &job && service->watch) {
            m_loop.unwatch(service->watch);
            service->watch = 0;
        }
    }
}

BootstrapEndpoint::LookUpAnswer Supervisor::look_up(IPC::Sender const& sender, String name)
{
    auto* service = find_service(name.view());
    if (!service)
        return { ENOENT, {} };
    // A sandbox says which services it may reach.
    if (sandbox_check_service(sender.pid, name.characters()) != 0 && errno == EPERM)
        return { EPERM, {} };
    // A registered service whose port is gone is gone too.
    if (!service->job) {
        struct pollfd poller { service->send.fd(), POLLOUT, 0 };
        if (poll(&poller, 1, 0) == 1 && (poller.revents & POLLHUP)) {
            m_services.remove_all_matching([&](auto const& candidate) { return candidate.ptr() == service; });
            return { ENOENT, {} };
        }
    }
    auto copy = service->send.duplicate();
    if (copy.is_error())
        return { copy.error().code(), {} };
    return { 0, copy.release_value() };
}

BootstrapEndpoint::CheckInAnswer Supervisor::check_in(IPC::Sender const& sender, String name)
{
    auto* service = find_service(name.view());
    if (!service || !service->receive.is_valid())
        return { ENOENT, {} };
    if (!service->job || service->job->pid != sender.pid)
        return { EPERM, {} };
    auto copy = service->receive.duplicate();
    if (copy.is_error())
        return { copy.error().code(), {} };
    return { 0, copy.release_value() };
}

i32 Supervisor::register_service(IPC::Sender const&, String name, IPC::Port service_port)
{
    if (find_service(name.view())) {
        // Taken, unless by a registered service that is gone.
        Supervisor::Service* existing = find_service(name.view());
        struct pollfd poller { existing->send.fd(), POLLOUT, 0 };
        if (existing->job || !(poll(&poller, 1, 0) == 1 && (poller.revents & POLLHUP)))
            return EEXIST;
        m_services.remove_all_matching([&](auto const& candidate) { return candidate.ptr() == existing; });
    }
    if (!service_port.is_valid())
        return EINVAL;
    auto* service = new (Base::nothrow) Service { move(name), {}, move(service_port), nullptr, 0 };
    if (!service)
        return ENOMEM;
    m_services.append(adopt_own(*service));
    return 0;
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
    if (plist.value().get_boolean("Disabled"_sv).value_or(false)) {
        // Whichever directory it would have come from.
        if (auto label = plist.value().get_string("Label"_sv); label.has_value())
            m_disabled_labels.append(MUST(String::from_view(label.value())));
        return;
    }
    char const* error = "";
    auto job = Job::from_plist(plist.value(), error);
    if (!job) {
        log("%s: %s", path, error);
        return;
    }
    if (job->only_in_graphical_session && !m_graphical_session)
        return;
    for (auto const& disabled : m_disabled_labels) {
        if (disabled.view() == job->label.view())
            return;
    }
    for (auto const& existing : m_jobs) {
        if (existing->label == job->label) {
            // From a directory before this one is how a job is replaced. In the same one, it's a mistake.
            if (existing->loaded_from_directory == m_directories_loaded)
                log("%s: a job named %s is loaded already", path, job->label.characters());
            return;
        }
    }
    // Its services exist from now on; messages to them wait for the job.
    for (auto const& name : job->mach_services) {
        if (find_service(name.view())) {
            log("(%s) The service %s is provided already", job->label.characters(), name.characters());
            continue;
        }
        auto port = IPC::create_port();
        if (port.is_error()) {
            log("(%s) Could not make the port for %s", job->label.characters(), name.characters());
            continue;
        }
        auto* service = new (Base::nothrow) Service { name, move(port.value().receive), move(port.value().send), job.ptr(), 0 };
        m_services.append(adopt_own(*service));
    }
    job->loaded_from_directory = m_directories_loaded;
    m_jobs.append(move(job));
}

void Supervisor::load_directory(char const* directory)
{
    ++m_directories_loaded;
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

void Supervisor::start_after_loading(Job& job)
{
    Job* pointer = &job;
    // KeepAlive of any kind implies RunAtLoad: whether to run again
    // depends on how a run ended.
    if (job.run_at_load || job.keep_alive != Job::KeepAlive::Never)
        spawn(job);
    if (job.start_interval > 0) {
        job.interval_timer = m_loop.add_timer(static_cast<u64>(job.start_interval) * 1000, true, [this, pointer] {
            if (!pointer->pid && !m_stopping)
                spawn(*pointer);
        });
    }
}

void Supervisor::start()
{
    // Services whose jobs wait for demand.
    for (auto& service : m_services)
        watch_for_demand(*service);
    for (auto& job : m_jobs)
        start_after_loading(*job);
}

Job* Supervisor::find_job(StringView label)
{
    for (auto& job : m_jobs) {
        if (job->label.view() == label)
            return job.ptr();
    }
    return nullptr;
}

void Supervisor::ask_to_stop(Job& job)
{
    if (!job.pid)
        return;
    kill(-job.pid, SIGTERM);
    kill(job.pid, SIGTERM);
    kill(-job.pid, SIGCONT);
    // By its label: it could have been unloaded, and gone, by then.
    pid_t pid = job.pid;
    auto label = job.label;
    m_loop.add_timer(static_cast<u64>(job.exit_timeout) * 1000, false, [this, label = move(label), pid] {
        auto* still_there = find_job(label.view());
        if (!still_there || still_there->pid != pid)
            return;
        log("(%s[%d]) Exited with SIGKILL after not stopping in time", label.characters(), pid);
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    });
}

void Supervisor::forget_job(Job& job)
{
    stop_watching(job);
    if (job.restart_timer)
        m_loop.cancel_timer(job.restart_timer);
    if (job.interval_timer)
        m_loop.cancel_timer(job.interval_timer);
    // Its services go with it: whoever had a right to them hears that nobody's there.
    for (size_t i = m_services.size(); i > 0; --i) {
        if (m_services[i - 1]->job == &job)
            m_services.remove(i - 1);
    }
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].ptr() == &job) {
            m_jobs.remove(i);
            return;
        }
    }
}

Optional<String> Supervisor::label_in_plist(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    Vector<char> text;
    char buffer[4096];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < got; ++i)
            text.append(buffer[i]);
    }
    close(fd);
    auto plist = JSON::parse(StringView(text.data(), text.size()));
    if (plist.is_error())
        return {};
    auto label = plist.value().get_string("Label"_sv);
    if (!label.has_value())
        return {};
    return MUST(String::from_view(label.value()));
}

String Supervisor::list_jobs(IPC::Sender const&)
{
    StringBuilder listing;
    listing.append("PID\tStatus\tLabel\n"_sv);
    for (auto const& job : m_jobs) {
        char line[512];
        char pid[16] = "-";
        if (job->pid)
            snprintf(pid, sizeof(pid), "%d", job->pid);
        // How it last ended: its exit status, or the signal as a negative number, as a Mac says it.
        int status = 0;
        if (WIFEXITED(job->last_status))
            status = WEXITSTATUS(job->last_status);
        else if (WIFSIGNALED(job->last_status))
            status = -WTERMSIG(job->last_status);
        snprintf(line, sizeof(line), "%s\t%d\t%s\n", pid, status, job->label.characters());
        listing.append(StringView(line, strlen(line)));
    }
    return MUST(listing.to_string());
}

i32 Supervisor::control_job(IPC::Sender const& sender, String verb, String what)
{
    if (sender.uid != 0)
        return EPERM;
    if (verb.view() == "start"_sv) {
        auto* job = find_job(what.view());
        if (!job)
            return ESRCH;
        if (!job->pid)
            spawn(*job);
        return 0;
    }
    if (verb.view() == "stop"_sv) {
        auto* job = find_job(what.view());
        if (!job)
            return ESRCH;
        ask_to_stop(*job);
        return 0;
    }
    if (verb.view() == "load"_sv) {
        auto label = label_in_plist(what.characters());
        if (!label.has_value())
            return EINVAL;
        if (find_job(label->view()))
            return EEXIST;
        load_file(what.characters());
        auto* job = find_job(label->view());
        if (!job)
            return EINVAL;
        for (auto& service : m_services) {
            if (service->job == job)
                watch_for_demand(*service);
        }
        start_after_loading(*job);
        return 0;
    }
    if (verb.view() == "unload"_sv) {
        auto label = what.view().starts_with("/"_sv) ? label_in_plist(what.characters()) : Optional<String>(MUST(String::from_view(what.view())));
        if (!label.has_value())
            return EINVAL;
        auto* job = find_job(label->view());
        if (!job)
            return ESRCH;
        if (job->pid) {
            job->unloading = true;
            ask_to_stop(*job);
            return 0;
        }
        forget_job(*job);
        return 0;
    }
    return EINVAL;
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
    debug("(%s[%d]) Started: %s", job.label.characters(), pid, job.program.characters());
    job.started_at = now_milliseconds();
    job.last_started_at = job.started_at;
    ++job.runs;
    stop_watching(job);
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
    else
        debug("(%s[%d]) Exited normally", job.label.characters(), job.pid);
    job.pid = 0;
    job.last_status = status;
    if (m_stopping)
        return;
    if (job.unloading) {
        Job* pointer = &job;
        m_loop.deferred_invoke([this, pointer] { forget_job(*pointer); });
        return;
    }
    if (!job.should_restart_after(status)) {
        // Until a message comes for one of its services.
        for (auto& service : m_services) {
            if (service->job == &job)
                watch_for_demand(*service);
        }
        return;
    }
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
    for (auto& job : m_jobs)
        stop_watching(*job);
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
