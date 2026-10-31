/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Symbols.h>
#include <LibELF/Demangle.h>
#include <LibELF/Image.h>

namespace Kernel::Symbols {

static Optional<ELF::Image> s_kernel_image;

void initialize(API::PhysicalRange kernel_file)
{
    if (kernel_file.size == 0)
        return;
    ReadonlyBytes file { reinterpret_cast<u8 const*>(API::direct_map_base + kernel_file.base), kernel_file.size };
    auto image = ELF::Image::parse(file);
    if (!image.is_error() && image.value().has_symbols())
        s_kernel_image = image.release_value();
}

bool is_available()
{
    return s_kernel_image.has_value();
}

void write_symbolicated(FormatSink& sink, FlatPtr address)
{
    Optional<ELF::Symbol> symbol;
    if (s_kernel_image.has_value())
        symbol = s_kernel_image->find_symbol(address);
    if (!symbol.has_value()) {
        sink.write("??"_sv);
        return;
    }
    if (!ELF::demangle(symbol->name, sink))
        sink.write(symbol->name);
    format_to(sink, "+{:#x}", address - symbol->address);
}

}
