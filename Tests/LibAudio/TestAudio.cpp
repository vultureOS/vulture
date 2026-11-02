/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAudio/Mixer.h>
#include <LibAudio/Sounds.h>
#include <LibAudio/Wav.h>
#include <LibTest/TestCase.h>

using namespace Audio;

namespace {

NonnullRefPtr<Sound const> constant(i16 value, size_t frames)
{
    Vector<i16> samples;
    for (size_t i = 0; i < frames * channel_count; ++i)
        samples.append(value);
    return Sound::create(move(samples));
}

Vector<i16> mix_out(Mixer& mixer, size_t frames)
{
    Vector<i16> output;
    MUST(output.try_resize(frames * channel_count));
    mixer.mix(output.data(), frames);
    return output;
}

}

TEST_CASE(silence_when_nothing_plays)
{
    Mixer mixer;
    EXPECT(mixer.is_idle());
    auto output = mix_out(mixer, 100);
    for (size_t i = 0; i < output.size(); ++i)
        EXPECT_EQ(output[i], static_cast<i16>(0));
}

TEST_CASE(a_sound_plays_then_stops)
{
    Mixer mixer;
    mixer.play(constant(1000, 10));
    EXPECT(!mixer.is_idle());
    auto output = mix_out(mixer, 20);
    EXPECT_EQ(output[0], static_cast<i16>(1000));
    EXPECT_EQ(output[19], static_cast<i16>(1000));
    EXPECT_EQ(output[20], static_cast<i16>(0));
    EXPECT(mixer.is_idle());
}

TEST_CASE(sounds_add_up_and_stop_at_the_limit)
{
    Mixer mixer;
    mixer.play(constant(1000, 4));
    mixer.play(constant(2000, 4));
    auto output = mix_out(mixer, 4);
    EXPECT_EQ(output[0], static_cast<i16>(3000));

    mixer.play(constant(30000, 4));
    mixer.play(constant(30000, 4));
    output = mix_out(mixer, 4);
    EXPECT_EQ(output[0], static_cast<i16>(32767));
    mixer.play(constant(-30000, 4));
    mixer.play(constant(-30000, 4));
    output = mix_out(mixer, 4);
    EXPECT_EQ(output[0], static_cast<i16>(-32768));
}

TEST_CASE(a_long_sound_spans_many_mixes)
{
    Mixer mixer;
    mixer.play(constant(500, 5000));
    size_t heard = 0;
    for (int i = 0; i < 20; ++i) {
        auto output = mix_out(mixer, 512);
        for (size_t j = 0; j < output.size(); j += channel_count) {
            if (output[j] != 0)
                ++heard;
        }
    }
    EXPECT_EQ(heard, static_cast<size_t>(5000));
    EXPECT(mixer.is_idle());
}

TEST_CASE(volume_is_heard_on_a_curve_without_a_click)
{
    Mixer mixer;
    mixer.set_volume(0.5f);
    // The change goes smoothly across the first mix, from full.
    mixer.play(constant(10000, 2000));
    auto first = mix_out(mixer, 1000);
    EXPECT(first[0] > 9900);
    EXPECT(first[1998] < 2600);
    // Then it holds: a quarter, the square of a half.
    auto second = mix_out(mixer, 1000);
    EXPECT_EQ(second[0], static_cast<i16>(2500));
    mixer.set_volume(5);
    EXPECT_EQ(mixer.volume(), 1.f);
    mixer.set_volume(-1);
    EXPECT_EQ(mixer.volume(), 0.f);
}

TEST_CASE(streams_play_their_pieces_in_turn)
{
    Mixer mixer;
    mixer.append_to_stream(7, constant(100, 4));
    mixer.append_to_stream(7, constant(200, 4));
    EXPECT_EQ(mixer.frames_left_in_stream(7), static_cast<size_t>(8));
    EXPECT_EQ(mixer.voice_count(), static_cast<size_t>(1));
    auto output = mix_out(mixer, 6);
    EXPECT_EQ(output[0], static_cast<i16>(100));
    EXPECT_EQ(output[8], static_cast<i16>(200));
    EXPECT_EQ(mixer.frames_left_in_stream(7), static_cast<size_t>(2));
    (void)mix_out(mixer, 2);
    EXPECT(mixer.is_idle());
    EXPECT_EQ(mixer.frames_left_in_stream(7), static_cast<size_t>(0));
}

TEST_CASE(too_many_voices_lose_the_oldest)
{
    Mixer mixer;
    for (size_t i = 0; i < Mixer::largest_voice_count + 4; ++i)
        mixer.play(constant(1, 100));
    EXPECT_EQ(mixer.voice_count(), Mixer::largest_voice_count);
}

TEST_CASE(the_named_sounds_are_real_sounds)
{
    for (auto name : named_sounds()) {
        auto sound = make_named_sound(name);
        EXPECT(sound);
        if (!sound)
            continue;
        auto samples = sound->samples();
        EXPECT(sound->seconds() > 0.05 && sound->seconds() < 3);
        i32 peak = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            i32 size = samples[i] < 0 ? -static_cast<i32>(samples[i]) : samples[i];
            if (size > peak)
                peak = size;
        }
        // Heard, and with room to spare for two at once.
        EXPECT(peak > 8000);
        EXPECT(peak < 20000);
        // Beginning and ending at nothing: no click either way.
        EXPECT_EQ(samples[0], static_cast<i16>(0));
        EXPECT(samples[samples.size() - 1] > -200 && samples[samples.size() - 1] < 200);
    }
    EXPECT(!make_named_sound("nonsense"_sv));
}

TEST_CASE(the_same_sound_every_time)
{
    auto a = make_named_sound("trash"_sv);
    auto b = make_named_sound("trash"_sv);
    EXPECT(a && b);
    EXPECT_EQ(a->samples().size(), b->samples().size());
    bool same = true;
    for (size_t i = 0; i < a->samples().size(); ++i)
        same = same && a->samples()[i] == b->samples()[i];
    EXPECT(same);
}

TEST_CASE(a_tone_is_at_its_frequency)
{
    auto tone = make_tone(440, 1, 0.5f);
    EXPECT_EQ(tone->frame_count(), static_cast<size_t>(48000));
    // Crossing zero twice a cycle.
    size_t crossings = 0;
    auto samples = tone->samples();
    for (size_t i = channel_count; i < samples.size(); i += channel_count) {
        if ((samples[i - channel_count] < 0) != (samples[i] < 0))
            ++crossings;
    }
    EXPECT(crossings >= 870 && crossings <= 890);
}

TEST_CASE(wav_round_trip)
{
    auto tone = make_tone(1000, 0.1, 0.3f);
    auto file = MUST(encode_wav(*tone));
    auto decoded = MUST(decode_wav(file.span()));
    EXPECT_EQ(decoded->samples().size(), tone->samples().size());
    bool same = true;
    for (size_t i = 0; i < tone->samples().size(); ++i)
        same = same && decoded->samples()[i] == tone->samples()[i];
    EXPECT(same);
}

TEST_CASE(wav_is_brought_to_the_device_format)
{
    // 24 kHz, mono, 8-bit: two frames at the top, two at the bottom.
    static constexpr u8 file[] = {
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0xc0, 0x5d, 0, 0, 0xc0, 0x5d, 0, 0, 1, 0, 8, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0, 228, 228, 28, 28
    };
    auto sound = MUST(decode_wav({ file, sizeof(file) }));
    // Twice as many frames, both channels the same.
    EXPECT_EQ(sound->frame_count(), static_cast<size_t>(8));
    auto samples = sound->samples();
    EXPECT_EQ(samples[0], static_cast<i16>(25600));
    EXPECT_EQ(samples[1], static_cast<i16>(25600));
    EXPECT_EQ(samples[12], static_cast<i16>(-25600));
}

TEST_CASE(wav_refuses_what_is_not_right)
{
    EXPECT(decode_wav(ReadonlyBytes {}).is_error());
    auto good = MUST(encode_wav(*make_tone(1000, 0.01, 0.3f)));
    // Not RIFF.
    auto bad = good;
    bad[0] = 'X';
    EXPECT(decode_wav(bad.span()).is_error());
    // Floating point.
    bad = good;
    bad[20] = 3;
    EXPECT(decode_wav(bad.span()).is_error());
    // No channels.
    bad = good;
    bad[22] = 0;
    EXPECT(decode_wav(bad.span()).is_error());
    // 12 bits.
    bad = good;
    bad[34] = 12;
    EXPECT(decode_wav(bad.span()).is_error());
    // A format chunk claiming to go on and on.
    bad = good;
    bad[16] = 0xff;
    bad[17] = 0xff;
    bad[18] = 0xff;
    bad[19] = 0x7f;
    EXPECT(decode_wav(bad.span()).is_error());
    // Cut off anywhere: refused, or what there is; never past the end (the sanitizers
    // watch that).
    for (size_t size = 0; size < good.size(); size += 7)
        (void)decode_wav({ good.data(), size });
    // A data chunk that says it is longer than it is: what there is.
    bad = good;
    bad[40] = 0xff;
    bad[41] = 0xff;
    bad[42] = 0xff;
    bad[43] = 0x7f;
    auto sound = MUST(decode_wav(bad.span()));
    EXPECT_EQ(sound->samples().size(), make_tone(1000, 0.01, 0.3f)->samples().size());
}
