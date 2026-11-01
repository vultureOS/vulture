/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Format.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibImage/Image.h>

// mkvicns OUTPUT POINTS@SCALE=IMAGE.png ...
//
// An icon file from the same icon rendered at each size, as 16@2x=icon32.png.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
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

int main(int argc, char** argv)
{
    if (argc < 3) {
        outln("usage: mkvicns OUTPUT POINTS@SCALE=IMAGE.png ...");
        return 2;
    }
    Vector<Vector<u8>> files;
    Vector<Image::IconFile::Entry> entries;
    MUST(files.try_ensure_capacity(static_cast<size_t>(argc)));
    for (int i = 2; i < argc; ++i) {
        StringView argument { argv[i] };
        auto at = argument.find('@');
        auto equals = argument.find('=');
        if (!at.has_value() || !equals.has_value() || equals.value() < at.value()) {
            outln("mkvicns: expected POINTS@SCALE=IMAGE.png, not {}", argument);
            return 2;
        }
        u32 points = 0, scale = 0;
        for (size_t c = 0; c < at.value(); ++c)
            points = points * 10 + static_cast<u32>(argument[c] - '0');
        for (size_t c = at.value() + 1; c < equals.value() && argument[c] != 'x'; ++c)
            scale = scale * 10 + static_cast<u32>(argument[c] - '0');
        FILE* file = fopen(argv[i] + equals.value() + 1, "rb");
        if (!file) {
            outln("mkvicns: cannot read {}", StringView(argv[i] + equals.value() + 1));
            return 2;
        }
        Vector<u8> bytes;
        u8 chunk[65536];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
            size_t offset = bytes.size();
            MUST(bytes.try_resize(offset + got));
            __builtin_memcpy(bytes.data() + offset, chunk, got);
        }
        fclose(file);
        if (auto checked = Image::decode_png(bytes.span()); checked.is_error()) {
            outln("mkvicns: {}: {}", StringView(argv[i] + equals.value() + 1), checked.error());
            return 2;
        }
        files.append(move(bytes));
        entries.append(Image::IconFile::Entry { points, scale, files.last().span() });
    }
    auto encoded = MUST(Image::encode_icon_file(entries));
    FILE* output = fopen(argv[1], "wb");
    if (!output) {
        outln("mkvicns: cannot write {}", StringView(argv[1]));
        return 2;
    }
    fwrite(encoded.data(), 1, encoded.size(), output);
    fclose(output);
    return 0;
}
