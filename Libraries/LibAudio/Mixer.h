/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAudio/Sound.h>

// What is playing, added together. Sounds play at once; the pieces of a stream play one
// after another, for a program that sends its audio in parts.

namespace Audio {

class Mixer {
public:
    static constexpr size_t largest_voice_count = 16;
    static constexpr int no_stream = -1;

    // On top of whatever else is playing. Past the limit, the oldest stops.
    void play(NonnullRefPtr<Sound const>);
    // After the last piece of this stream, or at once if it has none waiting.
    void append_to_stream(int stream, NonnullRefPtr<Sound const>);
    // How much of a stream is yet to play.
    [[nodiscard]] size_t frames_left_in_stream(int stream) const;

    // 0 to 1, as a slider would say. Sound is heard on a curve, so this is the square of
    // it as a gain. A change takes effect over one call to mix(), which keeps it from
    // clicking.
    void set_volume(float);
    [[nodiscard]] float volume() const { return m_volume; }

    [[nodiscard]] bool is_idle() const { return m_voices.is_empty(); }
    [[nodiscard]] size_t voice_count() const { return m_voices.size(); }

    // The next `frames` frames, into `output`, silence past the end of what there is. Too
    // loud together, samples stop at the limit: they do not wrap around.
    void mix(i16* output, size_t frames);

private:
    struct Voice {
        Vector<NonnullRefPtr<Sound const>> pieces;
        size_t piece { 0 };
        size_t position { 0 };
        int stream { no_stream };
    };

    float m_volume { 1 };
    float m_gain { 1 };
    Vector<Voice> m_voices;
};

}
