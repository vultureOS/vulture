/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Compare.h"
#include <LibBase/Format.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibImage/Image.h>

// imgdiff [--max-channel N] [--max-percent P] [--max-cluster C] [--regions FILE] [--output FILE] A.png B.png
//
// Do the two images match, as golden tests count it? See Compare.h for the rules: no channel of any pixel off by more
// than N (default 2), or at most P percent of pixels off by more (default 0.1); no touching group of more than C of those
// (default 8, -1 for no limit); and what FILE says about particular regions, A.png.regions if it isn't given and that's
// there, so that a golden's regions go with it wherever it's compared. Exit status 0 if they do, 1 if not, 2 for trouble.
// --output writes the difference, amplified, for a person to look at.

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
    ImgDiff::Options options;
    char const* output_path = nullptr;
    char const* regions_path = nullptr;
    char const* paths[2] = {};
    int path_count = 0;
    for (int i = 1; i < argc; ++i) {
        StringView argument { argv[i] };
        if (argument == "--max-channel"_sv && i + 1 < argc) {
            options.max_channel = atoi(argv[++i]);
        } else if (argument == "--max-percent"_sv && i + 1 < argc) {
            options.max_percent = atof(argv[++i]);
        } else if (argument == "--max-cluster"_sv && i + 1 < argc) {
            options.max_cluster = atoi(argv[++i]);
        } else if (argument == "--regions"_sv && i + 1 < argc) {
            regions_path = argv[++i];
        } else if (argument == "--output"_sv && i + 1 < argc) {
            output_path = argv[++i];
        } else if (path_count < 2) {
            paths[path_count++] = argv[i];
        } else {
            path_count = 3;
        }
    }
    if (path_count != 2) {
        outln("usage: imgdiff [--max-channel N] [--max-percent P] [--max-cluster C] [--regions FILE] [--output FILE] A.png B.png");
        return 2;
    }
    // Beside the first picture, which is the golden.
    char beside[1024] = {};
    if (!regions_path) {
        StringView const golden { paths[0] };
        constexpr auto suffix = ".regions"_sv;
        if (golden.length() + suffix.length() < sizeof(beside)) {
            __builtin_memcpy(beside, golden.characters_without_null_termination(), golden.length());
            __builtin_memcpy(beside + golden.length(), suffix.characters_without_null_termination(), suffix.length());
            beside[golden.length() + suffix.length()] = 0;
        }
    }
    Vector<u8> text;
    bool const have_regions = regions_path ? read_file(regions_path, text) : read_file(beside, text);
    if (regions_path && !have_regions) {
        outln("imgdiff: cannot read {}", StringView(regions_path));
        return 2;
    }
    if (have_regions) {
        auto regions = ImgDiff::parse_regions(StringView(text.span()));
        if (regions.is_error()) {
            outln("imgdiff: {}: {}", StringView(regions_path ? regions_path : beside), regions.error());
            return 2;
        }
        options.regions = regions.release_value();
    }
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
    auto const& a = *decoded[0];
    auto const& b = *decoded[1];
    if (a.size() != b.size()) {
        outln("imgdiff: the sizes differ: {}x{} and {}x{}", a.width(), a.height(), b.width(), b.height());
        return 1;
    }
    auto difference = MUST(Gfx::Bitmap::create(a.size()));
    auto const result = ImgDiff::compare(a, b, options, difference.ptr());

    outln("imgdiff: {} of {} pixels differ by more than {} ({} may); the worst by {} at {},{}: {}", result.off, result.total, options.max_channel, result.allowed, result.worst, result.worst_x, result.worst_y, result.matches() ? "match"_sv : "DIFFERENT"_sv);
    if (!result.cluster_ok)
        outln("imgdiff: {} touching pixels are off, around {},{} ({} may be)", result.largest_cluster, result.cluster_x, result.cluster_y, options.max_cluster);
    for (auto const& failure : result.strict_failures) {
        auto const& region = options.regions[failure.region_index];
        outln("imgdiff: strict region \"{}\" has {} pixels off, the first at {},{}", region.name.view(), failure.off, failure.first_x, failure.first_y);
    }
    if (output_path) {
        auto encoded = MUST(Image::encode_png(*difference));
        FILE* file = fopen(output_path, "wb");
        if (!file)
            return 2;
        fwrite(encoded.data(), 1, encoded.size(), file);
        fclose(file);
    }
    return result.matches() ? 0 : 1;
}
