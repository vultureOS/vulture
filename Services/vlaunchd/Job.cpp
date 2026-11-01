/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Job.h"
#include <sys/wait.h>

namespace Launchd {

namespace {

Optional<String> optional_string(JSON::Value const& plist, StringView key)
{
    auto value = plist.get_string(key);
    if (!value.has_value())
        return {};
    return MUST(String::from_view(value.value()));
}

}

OwnPtr<Job> Job::from_plist(JSON::Value const& plist, char const*& error)
{
    if (!plist.is_object()) {
        error = "the property list is not a dictionary";
        return {};
    }
    auto label = plist.get_string("Label"_sv);
    if (!label.has_value() || label->is_empty()) {
        error = "no Label";
        return {};
    }
    auto job = adopt_own(*new (Base::nothrow) Job);
    job->label = MUST(String::from_view(label.value()));

    if (auto const* arguments = plist.get_array("ProgramArguments"_sv)) {
        for (size_t i = 0; i < arguments->size(); ++i) {
            if (!arguments->at(i).is_string()) {
                error = "ProgramArguments holds something other than strings";
                return {};
            }
            job->arguments.append(MUST(String::from_view(arguments->at(i).as_string())));
        }
    }
    if (auto program = plist.get_string("Program"_sv); program.has_value())
        job->program = MUST(String::from_view(program.value()));
    else if (!job->arguments.is_empty())
        job->program = job->arguments[0];
    if (job->program.is_empty()) {
        error = "neither Program nor ProgramArguments";
        return {};
    }
    if (job->arguments.is_empty())
        job->arguments.append(job->program);

    job->run_at_load = plist.get_boolean("RunAtLoad"_sv).value_or(false);
    if (auto keep_alive = plist.get_boolean("KeepAlive"_sv); keep_alive.has_value()) {
        job->keep_alive = keep_alive.value() ? KeepAlive::Always : KeepAlive::Never;
    } else if (auto const* conditions = plist.get_object("KeepAlive"_sv)) {
        if (auto successful = conditions->get_boolean("SuccessfulExit"_sv); successful.has_value())
            job->keep_alive = successful.value() ? KeepAlive::OnSuccessfulExit : KeepAlive::OnFailedExit;
    }
    if (auto interval = plist.get_integer("ThrottleInterval"_sv); interval.has_value() && interval.value() >= 0)
        job->throttle_interval = static_cast<u32>(interval.value());
    if (auto interval = plist.get_integer("StartInterval"_sv); interval.has_value() && interval.value() > 0)
        job->start_interval = static_cast<u32>(interval.value());
    if (auto timeout = plist.get_integer("ExitTimeOut"_sv); timeout.has_value() && timeout.value() >= 0)
        job->exit_timeout = static_cast<u32>(timeout.value());
    if (auto mask = plist.get_integer("Umask"_sv); mask.has_value())
        job->umask = static_cast<mode_t>(mask.value() & 0777);
    if (auto session = plist.get_string("LimitLoadToSessionType"_sv); session.has_value())
        job->only_in_graphical_session = session.value() == "Aqua"_sv;
    job->user_name = optional_string(plist, "UserName"_sv);
    job->group_name = optional_string(plist, "GroupName"_sv);
    job->working_directory = optional_string(plist, "WorkingDirectory"_sv);
    job->standard_in = optional_string(plist, "StandardInPath"_sv);
    job->standard_out = optional_string(plist, "StandardOutPath"_sv);
    job->standard_error = optional_string(plist, "StandardErrorPath"_sv);
    if (auto const* services = plist.get_object("MachServices"_sv)) {
        for (size_t i = 0; i < services->size(); ++i) {
            if (services->at(i).is_boolean() && services->at(i).as_boolean())
                job->mach_services.append(MUST(String::from_view(services->key_at(i))));
        }
    }
    if (auto const* variables = plist.get_object("EnvironmentVariables"_sv)) {
        for (size_t i = 0; i < variables->size(); ++i) {
            if (!variables->at(i).is_string())
                continue;
            StringBuilder entry;
            entry.append(variables->key_at(i));
            entry.append('=');
            entry.append(variables->at(i).as_string());
            job->environment.append(MUST(entry.to_string()));
        }
    }
    return job;
}

bool Job::should_restart_after(int status) const
{
    bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    switch (keep_alive) {
    case KeepAlive::Never:
        return false;
    case KeepAlive::Always:
        return true;
    case KeepAlive::OnSuccessfulExit:
        return success;
    case KeepAlive::OnFailedExit:
        return !success;
    }
    return false;
}

}
