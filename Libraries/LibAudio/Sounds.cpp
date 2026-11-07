/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAudio/Sounds.h>
#include <LibBase/Math.h>

namespace Audio {

namespace {

constexpr double two_pi = 2 * Base::Math::pi;

// One sound being made, in floating point, mono until the end.
class Canvas {
public:
    explicit Canvas(double seconds)
    {
        MUST(m_samples.try_resize(static_cast<size_t>(seconds * sample_rate)));
        for (auto& sample : m_samples)
            sample = 0;
    }

    // A struck note: a fundamental and its overtones, ringing away. `attack` softens the
    // start; each overtone is `frequency * ratio`, at this gain, and dies away faster.
    void add_note(double start, double frequency, double gain, double decay, double attack = 0.002)
    {
        struct Partial {
            double ratio;
            double gain;
            double decay;
        };
        Partial const partials[] = { { 1, 1, 1 }, { 2.0, 0.28, 1.6 }, { 3.01, 0.09, 2.4 } };
        for (auto const& partial : partials) {
            double envelope = gain * partial.gain;
            double per_sample = 1 - decay * partial.decay / sample_rate;
            auto first = static_cast<size_t>(start * sample_rate);
            for (size_t i = first; i < m_samples.size(); ++i) {
                double time = static_cast<double>(i - first) / sample_rate;
                double opening = time < attack ? time / attack : 1;
                double phase = two_pi * frequency * partial.ratio * time;
                m_samples[i] += static_cast<float>(Base::Math::sine(phase) * envelope * opening);
                envelope *= per_sample;
            }
        }
    }

    // A burst of filtered noise. The same every time: the noise is a fixed sequence.
    void add_noise(double start, double gain, double decay, double brightness)
    {
        u32 state = 0x2545f491;
        double smoothed = 0;
        double envelope = gain;
        double per_sample = 1 - decay / sample_rate;
        for (auto i = static_cast<size_t>(start * sample_rate); i < m_samples.size(); ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            double white = static_cast<double>(state) / 2147483648.0 - 1;
            smoothed += (white - smoothed) * brightness;
            m_samples[i] += static_cast<float>(smoothed * envelope);
            envelope *= per_sample;
        }
    }

    // To stereo samples: as loud as asked at its loudest, without a click at either end,
    // and the right channel a moment behind the left, which makes it sound less like a
    // point in the middle of one's head.
    [[nodiscard]] NonnullRefPtr<Sound const> finish(float loudness)
    {
        float peak = 0;
        for (auto sample : m_samples) {
            float size = sample < 0 ? -sample : sample;
            if (size > peak)
                peak = size;
        }
        float scale = peak > 0 ? loudness * 32767 / peak : 0;
        constexpr size_t fade_in = sample_rate / 500;
        constexpr size_t fade_out = sample_rate / 100;
        constexpr size_t delay = sample_rate * 3 / 10000;
        size_t count = m_samples.size();
        auto faded = [&](size_t index) {
            if (index >= count)
                return 0.f;
            float value = m_samples[index] * scale;
            if (index < fade_in)
                value *= static_cast<float>(index) / static_cast<float>(fade_in);
            if (count - index < fade_out)
                value *= static_cast<float>(count - index) / static_cast<float>(fade_out);
            return value;
        };
        Vector<i16> samples;
        MUST(samples.try_resize((count + delay) * channel_count));
        for (size_t i = 0; i < count + delay; ++i) {
            samples[i * channel_count] = static_cast<i16>(faded(i));
            samples[i * channel_count + 1] = i >= delay ? static_cast<i16>(faded(i - delay)) : 0;
        }
        return Sound::create(move(samples));
    }

private:
    Vector<float> m_samples;
};

}

Span<StringView const> named_sounds()
{
    static StringView const names[] = { "notification"_sv, "alert"_sv, "trash"_sv, "login"_sv, "volume"_sv, "screenshot"_sv };
    return { names, array_size(names) };
}

RefPtr<Sound const> make_named_sound(StringView name)
{
    if (name == "notification"_sv) {
        // Two soft notes going up, a fourth apart.
        Canvas canvas(0.7);
        canvas.add_note(0, 987.77, 0.8, 7);
        canvas.add_note(0.11, 1318.51, 1, 7);
        return canvas.finish(0.45f);
    }
    if (name == "alert"_sv) {
        // Short and low, that something wants a look.
        Canvas canvas(0.32);
        canvas.add_note(0, 392, 1, 14);
        canvas.add_note(0, 587.33, 0.6, 18);
        return canvas.finish(0.5f);
    }
    if (name == "screenshot"_sv) {
        // A shutter: open and shut, two quick bright clicks.
        Canvas canvas(0.22);
        canvas.add_noise(0, 0.9, 60, 0.85);
        canvas.add_noise(0.085, 0.7, 70, 0.9);
        return canvas.finish(0.45f);
    }
    if (name == "trash"_sv) {
        // Paper, crumpling: three scrunches, each duller.
        Canvas canvas(0.42);
        canvas.add_noise(0, 1, 26, 0.55);
        canvas.add_noise(0.07, 0.8, 22, 0.4);
        canvas.add_noise(0.16, 0.6, 16, 0.25);
        return canvas.finish(0.4f);
    }
    if (name == "login"_sv) {
        // A warm major chord, opening upward.
        Canvas canvas(1.8);
        canvas.add_note(0, 220, 0.8, 2.2, 0.03);
        canvas.add_note(0.05, 329.63, 0.7, 2.2, 0.03);
        canvas.add_note(0.1, 440, 0.7, 2.4, 0.03);
        canvas.add_note(0.15, 554.37, 0.6, 2.6, 0.03);
        return canvas.finish(0.45f);
    }
    if (name == "volume"_sv) {
        // A tick, at the level the slider has just been set to.
        Canvas canvas(0.08);
        canvas.add_note(0, 1046.5, 1, 45);
        return canvas.finish(0.4f);
    }
    return nullptr;
}

NonnullRefPtr<Sound const> make_tone(double frequency, double seconds, float amplitude)
{
    auto frames = static_cast<size_t>(seconds * sample_rate);
    Vector<i16> samples;
    MUST(samples.try_resize(frames * channel_count));
    constexpr size_t fade = sample_rate / 200;
    for (size_t i = 0; i < frames; ++i) {
        double value = Base::Math::sine(two_pi * frequency * static_cast<double>(i) / sample_rate) * static_cast<double>(amplitude) * 32767;
        if (i < fade)
            value *= static_cast<double>(i) / fade;
        if (frames - i < fade)
            value *= static_cast<double>(frames - i) / fade;
        samples[i * channel_count] = static_cast<i16>(value);
        samples[i * channel_count + 1] = static_cast<i16>(value);
    }
    return Sound::create(move(samples));
}

}
