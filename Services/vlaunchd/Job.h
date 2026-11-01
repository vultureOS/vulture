/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibJSON/JSON.h>
#include <sys/types.h>

namespace Launchd {

// A job, as a property list in /System/Library/LaunchDaemons (or
// /Library/LaunchDaemons) describes it, with launchd's keys:
//
//   Label                 the job's name (required)
//   Program               the program; ProgramArguments[0] if absent
//   ProgramArguments      its arguments, argv[0] first
//   Disabled              not loaded at all
//   RunAtLoad             started as soon as it is loaded
//   KeepAlive             true: started again whenever it ends; or
//                         { "SuccessfulExit": true / false }: only when it
//                         exits with 0 / with anything else
//   ThrottleInterval      at least this many seconds between starts (10)
//   StartInterval         started every this many seconds
//   UserName, GroupName   whom it runs as (root)
//   WorkingDirectory      where it starts (/)
//   EnvironmentVariables  added to its environment
//   StandardInPath, StandardOutPath, StandardErrorPath  (/dev/null)
//   Umask                 its file creation mask
//   ExitTimeOut           seconds between SIGTERM and SIGKILL when stopped (20)
//   MachServices          { "name": true, ... }: services the job provides.
//                         Found through the bootstrap port; the job is
//                         started when a message arrives for one, and
//                         takes its receive right with check_in
struct Job {
    enum class KeepAlive {
        Never,
        Always,
        OnSuccessfulExit,
        OnFailedExit,
    };

    // A job from a plist; nothing (and `error` says why) if it is not one.
    static OwnPtr<Job> from_plist(JSON::Value const&, char const*& error);

    String label;
    String program;
    Vector<String> arguments;
    bool run_at_load { false };
    KeepAlive keep_alive { KeepAlive::Never };
    u32 throttle_interval { 10 };
    u32 start_interval { 0 };
    u32 exit_timeout { 20 };
    Optional<String> user_name;
    Optional<String> group_name;
    Optional<String> working_directory;
    Vector<String> environment; // "NAME=value"
    Optional<String> standard_in;
    Optional<String> standard_out;
    Optional<String> standard_error;
    Optional<mode_t> umask;
    Vector<String> mach_services;

    // While it runs.
    pid_t pid { 0 };
    u64 started_at { 0 }; // Milliseconds of the monotonic clock.
    u64 last_started_at { 0 };
    int last_status { 0 };
    u32 runs { 0 };
    int restart_timer { 0 };
    int interval_timer { 0 };

    [[nodiscard]] bool should_restart_after(int status) const;
};

}
