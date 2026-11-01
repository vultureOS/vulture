/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Format.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibImage/Image.h>

// imgdiff [--max-channel N] [--max-percent P] [--output FILE] A.png B.png
//
// Do the two images match, as golden tests count it: no channel of any pixel
// off by more than N (default 2), or at most P percent of pixels off by more
// (default 0.1). Exit status 0 if they do, 1 if not, 2 for trouble. --output
// writes the difference, amplified, for a person to look at.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
double atof(char const*);
int atoi(char const*);
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

}

namespace {

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
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return true;
}

}

int main(int argc, char** argv)
{
    int max_channel = 2;
    double max_percent = 0.1;
    char const* output_path = nullptr;
    char const* paths[2] = {};
    int path_count = 0;
    for (int i = 1; i < argc; ++i) {
        StringView argument { argv[i] };
        if (argument == "--max-channel"_sv && i + 1 < argc) {
            max_channel = atoi(argv[++i]);
        } else if (argument == "--max-percent"_sv && i + 1 < argc) {
            max_percent = atof(argv[++i]);
        } else if (argument == "--output"_sv && i + 1 < argc) {
            output_path = argv[++i];
        } else if (path_count < 2) {
            paths[path_count++] = argv[i];
        } else {
            path_count = 3;
        }
    }
    if (path_count != 2) {
        outln("usage: imgdiff [--max-channel N] [--max-percent P] [--output FILE] A.png B.png");
        return 2;
    }
    NonnullRefPtr<Gfx::Bitmap> const* images[2] = {};
    RefPtr<Gfx::Bitmap> decoded[2];
    for (int i = 0; i < 2; ++i) {
        Vector<u8> bytes;
        if (!read_file(paths[i], bytes)) {
            outln("imgdiff: cannot read {}", StringView(paths[i]));
            return 2;
        }
        auto image = Image::decode(bytes.span());
        if (image.is_error()) {
            outln("imgdiff: {}: {}", StringView(paths[i]), image.error());
            return 2;
        }
        decoded[i] = image.release_value();
    }
    (void)images;
    auto const& a = *decoded[0];
    auto const& b = *decoded[1];
    if (a.size() != b.size()) {
        outln("imgdiff: the sizes differ: {}x{} and {}x{}", a.width(), a.height(), b.width(), b.height());
        return 1;
    }
    auto difference = MUST(Gfx::Bitmap::create(a.size()));
    u64 total = static_cast<u64>(a.width()) * static_cast<u64>(a.height());
    u64 off = 0;
    int worst = 0;
    int worst_x = 0, worst_y = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            Gfx::Pixel pa = a.pixel_at(x, y), pb = b.pixel_at(x, y);
            auto distance = [](u8 first, u8 second) { return first > second ? first - second : second - first; };
            int red = distance(Gfx::pixel_red(pa), Gfx::pixel_red(pb));
            int green = distance(Gfx::pixel_green(pa), Gfx::pixel_green(pb));
            int blue = distance(Gfx::pixel_blue(pa), Gfx::pixel_blue(pb));
            int alpha = distance(Gfx::pixel_alpha(pa), Gfx::pixel_alpha(pb));
            int most = max(max(red, green), max(blue, alpha));
            if (most > worst) {
                worst = most;
                worst_x = x;
                worst_y = y;
            }
            if (most > max_channel)
                ++off;
            auto amplified = [](int value) { return static_cast<u8>(min(value * 16, 255)); };
            difference->set_pixel_at(x, y, Gfx::make_pixel(amplified(red), amplified(green), amplified(blue), 255));
        }
    }
    double percent = total ? 100.0 * static_cast<double>(off) / static_cast<double>(total) : 0;
    bool matches = percent <= max_percent;
    outln("imgdiff: {} of {} pixels differ by more than {} (allowed {} percent); the worst by {} at {},{}: {}", off, total, max_channel, static_cast<int>(max_percent * 1000), worst, worst_x, worst_y, matches ? "match"_sv : "DIFFERENT"_sv);
    if (output_path) {
        auto encoded = MUST(Image::encode_png(*difference));
        FILE* file = fopen(output_path, "wb");
        if (!file)
            return 2;
        fwrite(encoded.data(), 1, encoded.size(), file);
        fclose(file);
    }
    return matches ? 0 : 1;
}
