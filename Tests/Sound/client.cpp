/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <AudioEndpoint.h>
#include <LibAudio/Sounds.h>
#include <LibIPC/Bootstrap.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

// Sends a tone to the sound server in pieces, as a program playing a file would, then one of
// the system's sounds, waits for it all to be heard and switches the machine off. Whether it
// was heard is for the recording to say (Meta/sound-test.sh).

namespace {

constexpr double tone_seconds = 1.2;
constexpr double tone_frequency = 440;
constexpr size_t bytes_per_piece = 8 * 1024;

void pause_for(u32 milliseconds)
{
    timespec time { static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000 };
    nanosleep(&time, nullptr);
}

// How much of the processor the sound server has had so far.
u64 server_cpu_nanoseconds()
{
    static vulture_process_info processes[256];
    size_t size = sizeof(processes);
    if (sysctlbyname("kern.proc.all", processes, &size, nullptr, 0) < 0)
        return 0;
    for (size_t i = 0; i < size / sizeof(processes[0]); ++i) {
        if (strcmp(processes[i].name, "AudioServer") == 0)
            return processes[i].cpu_nanoseconds;
    }
    return 0;
}

}

int main()
{
    ErrorOr<IPC::Port> port = Error::from_errno(1);
    for (int attempt = 0; attempt < 100; ++attempt) {
        port = IPC::look_up("com.vulture.audioserver"_sv);
        if (!port.is_error())
            break;
        pause_for(50);
    }
    if (port.is_error()) {
        printf("sound: no server: %s\n", strerror(port.error().code()));
        return 1;
    }
    AudioProxy server(port.release_value());

    auto set = server.set_volume(100);
    auto volume = server.volume();
    if (set.is_error() || volume.is_error() || volume.value() != 100) {
        printf("sound: the volume did not take\n");
        return 1;
    }

    auto tone = Audio::make_tone(tone_frequency, tone_seconds, 0.5f);
    auto samples = tone->samples();
    auto const* data = reinterpret_cast<u8 const*>(samples.data());
    size_t total = samples.size() * sizeof(i16);
    u32 queued = 0;
    for (size_t at = 0; at < total; at += bytes_per_piece) {
        size_t length = min(bytes_per_piece, total - at);
        auto answer = server.play_samples(ReadonlyBytes { data + at, length });
        if (answer.is_error()) {
            printf("sound: %s\n", strerror(answer.error().code()));
            return 1;
        }
        queued = answer.value();
        if (queued > 300)
            pause_for(queued - 300);
    }
    printf("sound: sent %u bytes\n", static_cast<unsigned>(total));
    pause_for(queued + 200);

    auto named = server.play_sound("notification"_sv);
    if (named.is_error()) {
        printf("sound: %s\n", strerror(named.error().code()));
        return 1;
    }
    pause_for(800);

    // With nothing left to play, the server should be asleep: not looking at a device
    // that has room, over and over.
    u64 before = server_cpu_nanoseconds();
    if (before == 0) {
        printf("sound: FAIL: the server can't be found among the processes\n");
        return 1;
    }
    pause_for(500);
    u64 used = server_cpu_nanoseconds() - before;
    if (used > 50'000'000) {
        printf("sound: FAIL: the server used %u ms in 500 ms of silence\n", static_cast<unsigned>(used / 1000000));
        return 1;
    }

    printf("sound: played\n");
    fflush(stdout);
    reboot(RB_POWEROFF);
    return 0;
}
