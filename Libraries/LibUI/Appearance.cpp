/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibJSON/JSON.h>
#include <LibUI/Appearance.h>
#include <LibUI/Platform.h>
#include <LibUI/WindowRegistry.h>

namespace UI {

namespace {

OwnPtr<Appearance> s_current;
size_t s_missing_tokens;

struct Faces {
    RefPtr<Text::Typeface> regular, medium, semibold, bold, mono, mono_bold;
};

Faces s_faces;
Text::GlyphAtlas* s_atlas;

struct CachedFont {
    FontSpec spec;
    float scale;
    Text::Font* font;
};

Vector<CachedFont> s_fonts;

ErrorOr<NonnullRefPtr<Text::Typeface>> load_face(StringView file)
{
    StringBuilder path;
    path.append(Platform::the().library_directory());
    path.append("/Fonts/"_sv);
    path.append(file);
    auto full = TRY(path.to_string());
    return Text::Typeface::parse(TRY(Platform::the().read_file(full.view())));
}

RefPtr<Text::Typeface>& face_for(FontSpec spec)
{
    auto load = [](RefPtr<Text::Typeface>& slot, StringView file) -> RefPtr<Text::Typeface>& {
        if (!slot)
            slot = MUST(load_face(file));
        return slot;
    };
    if (spec.mono)
        return spec.weight >= FontWeight::SemiBold ? load(s_faces.mono_bold, "JetBrainsMono-Bold.ttf"_sv) : load(s_faces.mono, "JetBrainsMono-Regular.ttf"_sv);
    switch (spec.weight) {
    case FontWeight::Regular:
        return load(s_faces.regular, "Inter-Regular.ttf"_sv);
    case FontWeight::Medium:
        return load(s_faces.medium, "Inter-Medium.ttf"_sv);
    case FontWeight::SemiBold:
        return load(s_faces.semibold, "Inter-SemiBold.ttf"_sv);
    case FontWeight::Bold:
        return load(s_faces.bold, "Inter-Bold.ttf"_sv);
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Gfx::Color> parse_color(StringView text)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if ((text.length() != 7 && text.length() != 9) || text[0] != '#')
        return Error::from_string_literal("appearance: a color is not #rrggbb or #rrggbbaa");
    u8 values[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < (text.length() - 1) / 2; ++i) {
        int high = hex(text[1 + i * 2]);
        int low = hex(text[2 + i * 2]);
        if (high < 0 || low < 0)
            return Error::from_string_literal("appearance: a color is not #rrggbb or #rrggbbaa");
        values[i] = static_cast<u8>(high * 16 + low);
    }
    return Gfx::Color { values[0], values[1], values[2], values[3] };
}

ErrorOr<JSON::Value> read_json(StringView name)
{
    StringBuilder path;
    path.append(Platform::the().library_directory());
    path.append("/Themes/"_sv);
    path.append(name);
    auto full = TRY(path.to_string());
    auto bytes = TRY(Platform::the().read_file(full.view()));
    return JSON::parse(StringView(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
}

}

StringView name_of(ControlSize size)
{
    switch (size) {
    case ControlSize::Regular:
        return "regular"_sv;
    case ControlSize::Large:
        return "large"_sv;
    case ControlSize::Small:
        return "small"_sv;
    case ControlSize::Mini:
        return "mini"_sv;
    }
    VERIFY_NOT_REACHED();
}

Appearance& Appearance::the()
{
    if (!s_current)
        MUST(use("Light"_sv));
    return *s_current;
}

bool Appearance::is_loaded()
{
    return s_current.ptr() != nullptr;
}

ErrorOr<void> Appearance::merge(StringView prefix, void const* value_pointer)
{
    auto const& value = *static_cast<JSON::Value const*>(value_pointer);
    for (size_t i = 0; i < value.size(); ++i) {
        StringBuilder key_builder;
        if (!prefix.is_empty()) {
            key_builder.append(prefix);
            key_builder.append("."_sv);
        }
        key_builder.append(value.key_at(i));
        auto key = TRY(key_builder.to_string());
        auto const& member = value.at(i);
        if (member.is_object()) {
            TRY(merge(key.view(), &member));
        } else if (member.is_string()) {
            TRY(m_colors.try_set(move(key), TRY(parse_color(member.as_string()))));
        } else if (member.is_number()) {
            TRY(m_metrics.try_set(move(key), static_cast<float>(member.as_number())));
        } else if (member.is_array()) {
            Vector<float> numbers;
            for (size_t j = 0; j < member.size(); ++j) {
                auto const& number = member.at(j);
                if (!number.is_number())
                    return Error::from_string_literal("appearance: a list holds something other than numbers");
                TRY(numbers.try_append(static_cast<float>(number.as_number())));
            }
            TRY(m_lists.try_set(move(key), move(numbers)));
        }
    }
    return {};
}

ErrorOr<void> Appearance::use(StringView name)
{
    auto appearance = adopt_own(*new Appearance);
    appearance->m_name = TRY(String::from_view(name));
    appearance->m_dark = name == "Dark"_sv;

    StringBuilder file;
    file.append(name);
    file.append(".json"_sv);
    auto theme = TRY(read_json(TRY(file.to_string()).view()));
    auto const* ui = theme.get_object("ui"_sv);
    if (!ui)
        return Error::from_string_literal("appearance: the theme has no \"ui\"");
    TRY(appearance->merge(""_sv, ui));
    auto metrics = TRY(read_json("Metrics.json"_sv));
    TRY(appearance->merge(""_sv, &metrics));
    appearance->m_accent = appearance->color("accent"_sv);
    // Whoever is drawn from tokens looks them up afresh after this.
    if (s_current)
        appearance->m_accent = s_current->m_accent;
    s_current = move(appearance);
    WindowRegistry::appearance_changed();
    return {};
}

void Appearance::set_accent(Gfx::Color color)
{
    m_accent = color;
    WindowRegistry::appearance_changed();
}

Gfx::Color Appearance::color(StringView key) const
{
    if (auto const* found = m_colors.get(key))
        return *found;
    ++s_missing_tokens;
    return { 255, 0, 255, 255 };
}

float Appearance::metric(StringView key) const
{
    if (auto const* found = m_metrics.get(key))
        return *found;
    ++s_missing_tokens;
    return 0;
}

Span<float const> Appearance::list(StringView key) const
{
    if (auto const* found = m_lists.get(key))
        return found->span();
    ++s_missing_tokens;
    return {};
}

float Appearance::metric(StringView group, ControlSize size, StringView name) const
{
    char buffer[96];
    size_t at = 0;
    auto add = [&](StringView part) {
        for (size_t i = 0; i < part.length() && at < sizeof(buffer); ++i)
            buffer[at++] = part[i];
    };
    add(group);
    add("."_sv);
    add(name_of(size));
    add("."_sv);
    add(name);
    return metric(StringView(buffer, at));
}

size_t Appearance::missing_tokens()
{
    return s_missing_tokens;
}

Text::Font& Appearance::font(FontSpec spec, float scale)
{
    for (auto const& cached : s_fonts) {
        if (cached.spec == spec && cached.scale == scale)
            return *cached.font;
    }
    if (!s_atlas)
        s_atlas = new Text::GlyphAtlas;
    auto& face = face_for(spec);
    auto* font = new Text::Font(*face, spec.size, scale, *s_atlas);
    s_fonts.append(CachedFont { spec, scale, font });
    return *font;
}

float Appearance::text_width(FontSpec spec, StringView text)
{
    return font(spec, 2).width_of(text) / 2;
}

float Appearance::ascent(FontSpec spec)
{
    return font(spec, 2).ascent() / 2;
}

float Appearance::descent(FontSpec spec)
{
    return font(spec, 2).descent() / 2;
}

float Appearance::line_height(FontSpec spec)
{
    return font(spec, 2).line_height() / 2;
}

float Appearance::cap_height(FontSpec spec)
{
    return font(spec, 2).cap_height() / 2;
}

FontSpec Appearance::named_font(StringView name) const
{
    char buffer[64];
    size_t at = 0;
    auto add = [&](StringView part) {
        for (size_t i = 0; i < part.length() && at < sizeof(buffer); ++i)
            buffer[at++] = part[i];
    };
    add("fonts."_sv);
    add(name);
    size_t base = at;
    add(".size"_sv);
    float size = metric(StringView(buffer, at));
    at = base;
    add(".weight"_sv);
    auto weight = static_cast<FontWeight>(static_cast<u8>(metric(StringView(buffer, at))));
    return { size, weight, false };
}

FontSpec Appearance::control_font(ControlSize size) const
{
    return { metric("control"_sv, size, "fontSize"_sv), FontWeight::Regular, false };
}

FontSpec Appearance::bezel_font(ControlSize size) const
{
    return { metric("control"_sv, size, "fontSize"_sv), FontWeight::Medium, false };
}

}
