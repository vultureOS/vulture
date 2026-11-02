/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAudio/Wav.h>

namespace Audio {

namespace {

[[nodiscard]] u16 read_u16(u8 const* bytes)
{
    return static_cast<u16>(bytes[0] | (bytes[1] << 8));
}

[[nodiscard]] u32 read_u32(u8 const* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

[[nodiscard]] bool is_tag(u8 const* bytes, char const (&tag)[5])
{
    return bytes[0] == static_cast<u8>(tag[0]) && bytes[1] == static_cast<u8>(tag[1]) && bytes[2] == static_cast<u8>(tag[2]) && bytes[3] == static_cast<u8>(tag[3]);
}

}

ErrorOr<NonnullRefPtr<Sound const>> decode_wav(ReadonlyBytes file)
{
    if (file.size() < 12 || !is_tag(file.data(), "RIFF") || !is_tag(file.data() + 8, "WAVE"))
        return Error::from_string_literal("not a WAV file");

    u32 rate = 0;
    u32 channels = 0;
    u32 bits = 0;
    u8 const* data = nullptr;
    size_t data_size = 0;
    size_t position = 12;
    while (position + 8 <= file.size()) {
        u8 const* chunk = file.data() + position;
        size_t size = read_u32(chunk + 4);
        size_t room = file.size() - position - 8;
        if (is_tag(chunk, "fmt ")) {
            if (size < 16 || size > room)
                return Error::from_string_literal("WAV: bad format chunk");
            u16 tag = read_u16(chunk + 8);
            channels = read_u16(chunk + 10);
            rate = read_u32(chunk + 12);
            bits = read_u16(chunk + 22);
            bool extensible_pcm = tag == 0xfffe && size >= 26 && read_u16(chunk + 32) == 1;
            if (tag != 1 && !extensible_pcm)
                return Error::from_string_literal("WAV: only integer PCM is understood");
        } else if (is_tag(chunk, "data")) {
            data = chunk + 8;
            // A writer that never came back to say how long it was: what there is.
            data_size = size > room ? room : size;
            break;
        }
        // Chunks are padded to even sizes.
        if (size > room)
            return Error::from_string_literal("WAV: a chunk runs past the end");
        position += 8 + size + (size & 1);
    }
    if (!data || rate == 0 || channels == 0 || (bits != 8 && bits != 16 && bits != 24))
        return Error::from_string_literal("WAV: format or data missing, or not understood");
    if (rate > 768000 || channels > 64)
        return Error::from_string_literal("WAV: format not understood");

    size_t bytes_per_sample = bits / 8;
    size_t bytes_per_frame = bytes_per_sample * channels;
    size_t source_frames = data_size / bytes_per_frame;
    auto sample_at = [&](size_t frame, size_t channel) -> i32 {
        u8 const* at = data + frame * bytes_per_frame + channel * bytes_per_sample;
        switch (bits) {
        case 8:
            return (static_cast<i32>(at[0]) - 128) * 256;
        case 16:
            return static_cast<i16>(read_u16(at));
        default:
            // The top two bytes of three.
            return static_cast<i16>(read_u16(at + 1));
        }
    };

    u64 frames = static_cast<u64>(source_frames) * sample_rate / rate;
    // (Half an hour is more than any sound here should be, in memory.)
    if (frames > static_cast<u64>(sample_rate) * 1800)
        return Error::from_string_literal("WAV: too long");
    Vector<i16> samples;
    TRY(samples.try_resize(static_cast<size_t>(frames) * channel_count));
    for (size_t frame = 0; frame < static_cast<size_t>(frames); ++frame) {
        u64 scaled = static_cast<u64>(frame) * rate;
        auto left = static_cast<size_t>(scaled / sample_rate);
        auto between = static_cast<i32>(scaled % sample_rate);
        size_t right = left + 1 < source_frames ? left + 1 : left;
        for (size_t channel = 0; channel < channel_count; ++channel) {
            size_t from = channel < channels ? channel : 0;
            i32 a = sample_at(left, from);
            i32 b = sample_at(right, from);
            samples[frame * channel_count + channel] = static_cast<i16>(a + (b - a) * between / static_cast<i32>(sample_rate));
        }
    }
    return NonnullRefPtr<Sound const>(Sound::create(move(samples)));
}

ErrorOr<Vector<u8>> encode_wav(Sound const& sound)
{
    auto samples = sound.samples();
    size_t data_size = samples.size() * sizeof(i16);
    Vector<u8> file;
    TRY(file.try_ensure_capacity(44 + data_size));
    auto tag = [&](char const (&text)[5]) {
        for (int i = 0; i < 4; ++i)
            file.append(static_cast<u8>(text[i]));
    };
    auto put_u16 = [&](u16 value) {
        file.append(static_cast<u8>(value));
        file.append(static_cast<u8>(value >> 8));
    };
    auto put_u32 = [&](u32 value) {
        put_u16(static_cast<u16>(value));
        put_u16(static_cast<u16>(value >> 16));
    };
    tag("RIFF");
    put_u32(static_cast<u32>(36 + data_size));
    tag("WAVE");
    tag("fmt ");
    put_u32(16);
    put_u16(1);
    put_u16(channel_count);
    put_u32(sample_rate);
    put_u32(sample_rate * channel_count * sizeof(i16));
    put_u16(channel_count * sizeof(i16));
    put_u16(16);
    tag("data");
    put_u32(static_cast<u32>(data_size));
    for (size_t i = 0; i < samples.size(); ++i)
        put_u16(static_cast<u16>(samples[i]));
    return file;
}

}
