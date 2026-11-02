/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Cursors.h>
#include <LibBase/StringView.h>
#include <LibGfx/Painter.h>
#include <LibImage/Image.h>

// cursor-sheet OUTPUT.png [ZOOM [SHAPE]]
//
// Every shape of the pointer at each scale, on the colors it's seen on most. QEMU's screenshots don't show a
// hardware cursor, so this is how they get looked at, and compared with a golden copy (Meta/ui-test.sh). ZOOM makes
// each pixel that many, to see what each one is; SHAPE is one shape alone, by number.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const* format, ...);
}

using namespace Gfx;
using Window::CursorShape;

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: cursor-sheet OUTPUT.png [ZOOM]\n");
        return 2;
    }
    auto number_at = [&](int index, int fallback) {
        return argc > index ? StringView(argv[index], __builtin_strlen(argv[index])).to_number<int>().value_or(fallback) : fallback;
    };
    int const zoom = number_at(2, 1);
    int const only_shape = number_at(3, -1);
    int const shapes = only_shape >= 0 ? 1 : static_cast<int>(CursorShape::Count);
    constexpr int cell = 64;
    u32 const backgrounds[] = { 0xf6f6f6, 0x282828, 0x5a6fa8 };
    constexpr int background_count = 3;
    constexpr int rows = background_count * 2;
    auto sheet = MUST(Bitmap::create({ shapes * cell * zoom, rows * cell * zoom }));
    auto small = MUST(Bitmap::create({ shapes * cell, rows * cell }));
    {
        Painter painter(*small);
        for (int row = 0; row < rows; ++row) {
            painter.fill_rect(FloatRect { 0, static_cast<float>(row * cell), static_cast<float>(shapes * cell), static_cast<float>(cell) }, Color::from_rgb(backgrounds[row % background_count]));
            int const scale = row < background_count ? 1 : 2;
            for (int shape = 0; shape < shapes; ++shape) {
                auto const image = WindowServer::make_cursor(static_cast<CursorShape>(only_shape >= 0 ? only_shape : shape), scale);
                painter.blit(*image.image, { shape * cell, row * cell }, { 0, 0, image.image->width(), image.image->height() });
            }
        }
    }
    for (int y = 0; y < sheet->height(); ++y) {
        for (int x = 0; x < sheet->width(); ++x)
            sheet->set_pixel_at(x, y, small->pixel_at(x / zoom, y / zoom));
    }

    auto const png = MUST(Image::encode_png(*sheet));
    FILE* output = fopen(argv[1], "wb");
    if (!output || fwrite(png.begin(), 1, png.size(), output) != png.size()) {
        printf("cursor-sheet: can't write %s\n", argv[1]);
        return 1;
    }
    fclose(output);
    return 0;
}
