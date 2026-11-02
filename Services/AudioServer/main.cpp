/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <AudioEndpoint.h>
#include <LibAudio/Mixer.h>
#include <LibAudio/Sounds.h>
#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <Kernel/API/POSIX/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// com.vulture.audioserver: everyone's sound goes through here. The device takes a period
// at a time and says when it has room; with nothing playing, nothing is written and the
// device runs out and stops being fed, so a quiet system does no work for sound.
//
// With no sound device (there may be none), the requests are still answered and nothing
// is heard: a program should not have to care.

namespace {

constexpr StringView preferences_path = "/Library/Preferences/com.vulture.audio.json"_sv;
constexpr size_t frames_per_period = VULTURE_AUDIO_PERIOD_BYTES / VULTURE_AUDIO_BYTES_PER_FRAME;
constexpr u32 default_volume_percent = 75;

class Server final : public AudioStub {
public:
    explicit Server(Core::EventLoop& loop)
        : m_loop(loop)
        , m_log(os_log_create("com.vulture.audioserver", "server"))
    {
        m_device = open("/dev/audio0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_device < 0)
            os_log_info(m_log, "No sound device: playing nothing");
        else
            os_log_info(m_log, "Playing to /dev/audio0");
        load_volume();
    }

    void play_sound(IPC::Sender const& sender, String name) override
    {
        auto sound = named_sound(name.view());
        if (!sound) {
            os_log_error(m_log, "Process %d asked for a sound there isn't: %s", static_cast<int>(sender.pid), name.characters());
            return;
        }
        os_log_debug(m_log, "Process %d plays %s", static_cast<int>(sender.pid), name.characters());
        m_mixer.play(sound.release_nonnull());
        pump();
    }

    u32 play_samples(IPC::Sender const& sender, Vector<u8> samples) override
    {
        size_t frames = samples.size() / VULTURE_AUDIO_BYTES_PER_FRAME;
        if (frames > 0) {
            Vector<i16> converted;
            MUST(converted.try_resize(frames * VULTURE_AUDIO_CHANNELS));
            __builtin_memcpy(converted.data(), samples.data(), frames * VULTURE_AUDIO_BYTES_PER_FRAME);
            m_mixer.append_to_stream(static_cast<int>(sender.pid), Audio::Sound::create(move(converted)));
            pump();
        }
        return static_cast<u32>(m_mixer.frames_left_in_stream(static_cast<int>(sender.pid)) * 1000 / VULTURE_AUDIO_SAMPLE_RATE);
    }

    void set_volume(IPC::Sender const& sender, u32 percent) override
    {
        if (percent > 100)
            percent = 100;
        os_log_info(m_log, "Process %d sets the volume to %u", static_cast<int>(sender.pid), percent);
        m_percent = percent;
        m_mixer.set_volume(static_cast<float>(percent) / 100.0f);
        // Only those who may are able to change what stays after a reboot.
        if (sender.uid == 0 || sender.uid >= 501)
            save_volume();
    }

    u32 volume(IPC::Sender const&) override { return m_percent; }

private:
    RefPtr<Audio::Sound const> named_sound(StringView name)
    {
        if (auto* cached = m_sounds.get(name))
            return *cached;
        auto made = Audio::make_named_sound(name);
        if (!made)
            return nullptr;
        MUST(m_sounds.try_set(MUST(String::from_view(name)), *made));
        return made;
    }

    // Mixes and writes for as long as the device has room. A period that didn't fit waits
    // here, whole, for the device to say it has room for it.
    void pump()
    {
        if (m_device < 0) {
            discard_what_is_playing();
            return;
        }
        for (;;) {
            if (m_period_left == 0) {
                if (m_mixer.is_idle()) {
                    stop_watching();
                    return;
                }
                m_mixer.mix(m_period, frames_per_period);
                m_period_left = sizeof(m_period);
            }
            ssize_t written = write(m_device, reinterpret_cast<u8 const*>(m_period) + (sizeof(m_period) - m_period_left), m_period_left);
            if (written < 0) {
                if (errno == EAGAIN) {
                    start_watching();
                    return;
                }
                if (errno == EINTR)
                    continue;
                os_log_error(m_log, "Writing to the device: %s", strerror(errno));
                stop_watching();
                close(m_device);
                m_device = -1;
                discard_what_is_playing();
                return;
            }
            m_period_left -= static_cast<size_t>(written);
        }
    }

    // Everything happens at once, as far as anyone can tell.
    void discard_what_is_playing()
    {
        while (!m_mixer.is_idle())
            m_mixer.mix(m_period, frames_per_period);
    }

    void start_watching()
    {
        if (m_watch >= 0)
            return;
        m_watch = m_loop.watch(m_device, Core::EventLoop::Interest::Write, [this] { pump(); });
    }

    void stop_watching()
    {
        if (m_watch < 0)
            return;
        m_loop.unwatch(m_watch);
        m_watch = -1;
    }

    void load_volume()
    {
        u32 percent = default_volume_percent;
        int fd = open(preferences_path.characters_without_null_termination(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char text[256];
            ssize_t got = read(fd, text, sizeof(text));
            close(fd);
            if (got > 0) {
                auto parsed = JSON::parse(StringView { text, static_cast<size_t>(got) });
                if (!parsed.is_error()) {
                    if (auto value = parsed.value().get_number("volume"_sv); value.has_value() && value.value() >= 0 && value.value() <= 100)
                        percent = static_cast<u32>(value.value());
                }
            }
        }
        m_percent = percent;
        m_mixer.set_volume(static_cast<float>(percent) / 100.0f);
    }

    void save_volume()
    {
        auto object = JSON::Value::object();
        MUST(object.set("volume"_sv, JSON::Value(static_cast<i64>(m_percent))));
        auto text = MUST(object.serialize());
        // The whole file or none of it, however the machine stops.
        char const* temporary = "/Library/Preferences/com.vulture.audio.json.new";
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            os_log_error(m_log, "Could not save the volume: %s", strerror(errno));
            return;
        }
        bool written = write(fd, text.characters(), text.length()) == static_cast<ssize_t>(text.length());
        fsync(fd);
        close(fd);
        if (!written || rename(temporary, preferences_path.characters_without_null_termination()) < 0) {
            os_log_error(m_log, "Could not save the volume: %s", strerror(errno));
            unlink(temporary);
        }
    }

    Core::EventLoop& m_loop;
    os_log_t m_log;
    int m_device { -1 };
    // While it has something to play and the device has no room.
    int m_watch { -1 };
    u32 m_percent { default_volume_percent };
    Audio::Mixer m_mixer;
    HashMap<String, NonnullRefPtr<Audio::Sound const>> m_sounds;
    i16 m_period[frames_per_period * VULTURE_AUDIO_CHANNELS];
    size_t m_period_left { 0 };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.audioserver"_sv);
    if (port.is_error()) {
        fprintf(stderr, "AudioServer: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Server server(loop);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)server.handle(message); });
    return loop.exec();
}
