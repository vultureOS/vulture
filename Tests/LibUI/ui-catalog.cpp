/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Catalog.h"
#include "Render.h"
#include "TestPlatform.h"
#include <LibBase/String.h>
#include <LibImage/Image.h>

// Draws every control of the catalog, as Meta/Calibration/measure.m drew them in
// AppKit, for Meta/Calibration/compare.py: rows of pixels and pictures, in a directory.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const* format, ...);
}

using namespace Gfx;

namespace {

void write_file(StringView path, ReadonlyBytes bytes)
{
    auto terminated = MUST(String::from_view(path));
    FILE* file = fopen(terminated.characters(), "wb");
    VERIFY(file);
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

void append_hex(StringBuilder& json, Pixel pixel)
{
    json.appendff("\"{:02x}{:02x}{:02x}{:02x}\"", pixel_red(pixel), pixel_green(pixel), pixel_blue(pixel), pixel_alpha(pixel));
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: ui-catalog <directory> [measured-sizes.json]\n");
        return 2;
    }
    StringView directory { argv[1], __builtin_strlen(argv[1]) };
    (void)TestPlatform::the();
    auto measured = read_measured_controls();
    StringBuilder json;
    json.append("{\n"_sv);
    auto catalog = control_catalog();
    bool first = true;
    for (auto const& entry : catalog) {
        for (auto look : all_looks()) {
            auto rendered = render_entry(entry, look, measured);
            StringBuilder name;
            name.append(entry.name);
            name.append("-"_sv);
            name.append(look.name);
            if (!first)
                json.append(",\n"_sv);
            first = false;
            json.appendff("\"{}\":{{\"box\":{{\"x\":{},\"y\":{},\"w\":{},\"h\":{}}},\"softBox\":{{\"x\":{},\"y\":{},\"w\":{},\"h\":{}}},\"down\":[", name.view(),
                rendered.box.x, rendered.box.y, rendered.box.width, rendered.box.height,
                rendered.soft_box.x, rendered.soft_box.y, rendered.soft_box.width, rendered.soft_box.height);
            for (size_t i = 0; i < rendered.down.size(); ++i) {
                if (i)
                    json.append(","_sv);
                append_hex(json, rendered.down[i]);
            }
            json.append("],\"across\":["_sv);
            for (size_t i = 0; i < rendered.across.size(); ++i) {
                if (i)
                    json.append(","_sv);
                append_hex(json, rendered.across[i]);
            }
            json.append("]}"_sv);
            auto png = MUST(Image::encode_png(*rendered.picture));
            StringBuilder path;
            path.append(directory);
            path.append("/"_sv);
            path.append(name.view());
            path.append(".png"_sv);
            write_file(path.view(), png.span());
        }
    }
    json.append("\n}\n"_sv);
    StringBuilder path;
    path.append(directory);
    path.append("/rendered.json"_sv);
    write_file(path.view(), ReadonlyBytes(reinterpret_cast<u8 const*>(json.view().characters_without_null_termination()), json.view().length()));
    return 0;
}
