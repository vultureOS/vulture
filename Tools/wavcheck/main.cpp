/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAudio/Wav.h>
#include <LibBase/Format.h>
#include <LibBase/Math.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// wavcheck FILE --tone HZ --seconds S
//
// Was a tone really played into this recording? Goes through it in pieces of 20 ms, and
// counts a piece as the tone when nearly all its energy is at that frequency and it is loud
// enough to hear. Exit status 0 if the pieces add up to S seconds or more, 1 if not, 2 for
// trouble.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
double atof(char const*);
}

extern "C" long write(int fd, void const* buffer, unsigned long count);

namespace {

class StandardOutput final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(1, text.characters_without_null_termination(), text.length()); }
};

template<typename... Args>
void outln(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    StandardOutput output;
    format_to(output, format, args...);
    output.write("\n"_sv);
}

bool read_file(char const* path, Vector<u8>& bytes)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t at = bytes.size();
        MUST(bytes.try_resize(at + got));
        for (size_t i = 0; i < got; ++i)
            bytes[at + i] = chunk[i];
    }
    fclose(file);
    return true;
}

// The energy at one frequency: one line of a Fourier transform, without the rest of it.
double goertzel(i16 const* interleaved, size_t frames, double frequency)
{
    double angle = 2 * Base::Math::pi * frequency / Audio::sample_rate;
    double coefficient = 2 * Base::Math::cosine(angle);
    double previous = 0;
    double before_that = 0;
    for (size_t i = 0; i < frames; ++i) {
        double left = interleaved[i * 2] / 32768.0;
        double next = left + coefficient * previous - before_that;
        before_that = previous;
        previous = next;
    }
    double power = previous * previous + before_that * before_that - coefficient * previous * before_that;
    return power * 2 / static_cast<double>(frames) / static_cast<double>(frames) * 2;
}

}

int main(int argc, char** argv)
{
    char const* path = nullptr;
    double tone = 0;
    double seconds_wanted = 0;
    for (int i = 1; i < argc; ++i) {
        StringView argument { argv[i] };
        if (argument == "--tone"_sv && i + 1 < argc)
            tone = atof(argv[++i]);
        else if (argument == "--seconds"_sv && i + 1 < argc)
            seconds_wanted = atof(argv[++i]);
        else
            path = argv[i];
    }
    if (!path || tone <= 0 || seconds_wanted <= 0) {
        outln("usage: wavcheck FILE --tone HZ --seconds S");
        return 2;
    }

    Vector<u8> bytes;
    if (!read_file(path, bytes)) {
        outln("wavcheck: can't read {}", StringView { path });
        return 2;
    }
    auto decoded = Audio::decode_wav(bytes.span());
    if (decoded.is_error()) {
        outln("wavcheck: {} is not a WAV file we can read", StringView { path });
        return 2;
    }
    auto sound = decoded.release_value();
    auto samples = sound->samples();

    static constexpr size_t frames_per_piece = Audio::sample_rate / 50;
    size_t pieces_with_tone = 0;
    size_t pieces_with_sound = 0;
    double loudest = 0;
    for (size_t frame = 0; frame + frames_per_piece <= sound->frame_count(); frame += frames_per_piece) {
        double energy = 0;
        for (size_t i = 0; i < frames_per_piece; ++i) {
            double left = samples[(frame + i) * 2] / 32768.0;
            energy += left * left;
        }
        energy /= frames_per_piece;
        if (energy > loudest)
            loudest = energy;
        // About -50 dB: anything quieter nobody would hear as the tone.
        if (energy < 0.00001)
            continue;
        ++pieces_with_sound;
        // A sine at amplitude a has energy a*a/2 and, from goertzel(), power a*a: so a pure
        // tone gives the ratio 2, and this asks for most of that.
        double power = goertzel(samples.data() + frame * 2, frames_per_piece, tone);
        if (power / energy > 1.2)
            ++pieces_with_tone;
    }

    double seconds_of_tone = static_cast<double>(pieces_with_tone) * frames_per_piece / Audio::sample_rate;
    double seconds_of_sound = static_cast<double>(pieces_with_sound) * frames_per_piece / Audio::sample_rate;
    outln("wavcheck: {} ms recorded, {} ms with sound, {} ms of it a {} Hz tone, loudest {}%",
        static_cast<u64>(sound->seconds() * 1000), static_cast<u64>(seconds_of_sound * 1000), static_cast<u64>(seconds_of_tone * 1000),
        static_cast<u64>(tone), static_cast<u64>(Base::Math::square_root(loudest * 2) * 100));
    if (seconds_of_tone < seconds_wanted) {
        outln("wavcheck: wanted {} ms of that tone", static_cast<u64>(seconds_wanted * 1000));
        return 1;
    }
    return 0;
}
