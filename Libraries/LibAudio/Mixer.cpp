/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAudio/Mixer.h>

namespace Audio {

void Mixer::play(NonnullRefPtr<Sound const> sound)
{
    if (m_voices.size() >= largest_voice_count)
        m_voices.remove(0);
    Voice voice;
    voice.pieces.append(move(sound));
    m_voices.append(move(voice));
}

void Mixer::append_to_stream(int stream, NonnullRefPtr<Sound const> sound)
{
    for (auto& voice : m_voices) {
        if (voice.stream == stream) {
            voice.pieces.append(move(sound));
            return;
        }
    }
    if (m_voices.size() >= largest_voice_count)
        m_voices.remove(0);
    Voice voice;
    voice.stream = stream;
    voice.pieces.append(move(sound));
    m_voices.append(move(voice));
}

size_t Mixer::frames_left_in_stream(int stream) const
{
    for (auto const& voice : m_voices) {
        if (voice.stream != stream)
            continue;
        size_t left = 0;
        for (size_t i = voice.piece; i < voice.pieces.size(); ++i)
            left += voice.pieces[i]->frame_count();
        return left - voice.position;
    }
    return 0;
}

void Mixer::set_volume(float volume)
{
    m_volume = volume < 0 ? 0 : (volume > 1 ? 1 : volume);
}

void Mixer::mix(i16* output, size_t frames)
{
    // In pieces small enough for a sum on the stack.
    constexpr size_t chunk_frames = 512;
    float target = m_volume * m_volume;
    float step = frames > 0 ? (target - m_gain) / static_cast<float>(frames) : 0;
    size_t done = 0;
    while (done < frames) {
        size_t count = min(frames - done, chunk_frames);
        i32 sum[chunk_frames * channel_count] {};
        for (auto& voice : m_voices) {
            size_t filled = 0;
            while (filled < count && voice.piece < voice.pieces.size()) {
                auto samples = voice.pieces[voice.piece]->samples();
                size_t available = samples.size() / channel_count - voice.position;
                size_t take = min(available, count - filled);
                i16 const* from = samples.data() + voice.position * channel_count;
                i32* to = sum + filled * channel_count;
                for (size_t i = 0; i < take * channel_count; ++i)
                    to[i] += from[i];
                filled += take;
                voice.position += take;
                if (voice.position * channel_count >= samples.size()) {
                    ++voice.piece;
                    voice.position = 0;
                }
            }
        }
        for (size_t frame = 0; frame < count; ++frame) {
            m_gain += step;
            for (size_t channel = 0; channel < channel_count; ++channel) {
                float value = static_cast<float>(sum[frame * channel_count + channel]) * m_gain;
                if (value > 32767)
                    value = 32767;
                else if (value < -32768)
                    value = -32768;
                output[(done + frame) * channel_count + channel] = static_cast<i16>(value);
            }
        }
        done += count;
    }
    m_gain = target;
    m_voices.remove_all_matching([](Voice const& voice) { return voice.piece >= voice.pieces.size(); });
}

}
