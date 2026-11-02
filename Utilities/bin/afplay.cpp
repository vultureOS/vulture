/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <AudioEndpoint.h>
#include <LibAudio/Wav.h>
#include <LibIPC/Bootstrap.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// afplay FILE: plays a WAV file, and returns once it has been heard.

namespace {

constexpr size_t bytes_per_piece = 16 * 1024;
// Ahead of the speakers by this much is plenty, and little for the server to hold.
constexpr u32 most_milliseconds_queued = 250;

void pause_for(u32 milliseconds)
{
    timespec time { static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000 };
    nanosleep(&time, nullptr);
}

ErrorOr<Vector<u8>> read_whole_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    for (;;) {
        u8 chunk[65536];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            int code = errno;
            close(fd);
            return Error::from_errno(code);
        }
        if (got == 0)
            break;
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        __builtin_memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: afplay file.wav\n");
        return 2;
    }
    auto bytes = read_whole_file(argv[1]);
    if (bytes.is_error()) {
        fprintf(stderr, "afplay: %s: %s\n", argv[1], strerror(bytes.error().code()));
        return 1;
    }
    auto sound = Audio::decode_wav(bytes.value().span());
    if (sound.is_error()) {
        fprintf(stderr, "afplay: %s: %s\n", argv[1], strerror(sound.error().code()));
        return 1;
    }
    auto port = IPC::look_up("com.vulture.audioserver"_sv);
    if (port.is_error()) {
        fprintf(stderr, "afplay: no sound server: %s\n", strerror(port.error().code()));
        return 1;
    }
    AudioProxy server(port.release_value());

    auto samples = sound.value()->samples();
    auto const* data = reinterpret_cast<u8 const*>(samples.data());
    size_t total = samples.size() * sizeof(i16);
    u32 queued = 0;
    for (size_t at = 0; at < total; at += bytes_per_piece) {
        size_t length = min(bytes_per_piece, total - at);
        auto answer = server.play_samples(ReadonlyBytes { data + at, length });
        if (answer.is_error()) {
            fprintf(stderr, "afplay: %s\n", strerror(answer.error().code()));
            return 1;
        }
        queued = answer.value();
        if (queued > most_milliseconds_queued)
            pause_for(queued - most_milliseconds_queued);
    }
    pause_for(queued);
    return 0;
}
