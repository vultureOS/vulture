/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/CharacterTypes.h>
#include <LibCrypto/Curve25519.h>
#include <LibPackage/Catalog.h>

namespace Package {

namespace {

bool decode_hex(StringView text, Bytes output)
{
    if (text.length() != output.size() * 2)
        return false;
    for (size_t i = 0; i < output.size(); ++i) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        int const high = digit(text[i * 2]);
        int const low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<u8>(high << 4 | low);
    }
    return true;
}

}

String hex_of(ReadonlyBytes bytes)
{
    StringBuilder builder;
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
        char const pair[2] = { digits[bytes[i] >> 4], digits[bytes[i] & 15] };
        builder.append({ pair, 2 });
    }
    return MUST(builder.to_string());
}

bool is_valid_name(StringView name)
{
    if (name.is_empty() || name.length() > 128 || name[0] == '.' || name[0] == '-')
        return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char const c = name[i];
        if (!is_ascii_alphanumeric(c) && c != '-' && c != '_' && c != '.')
            return false;
    }
    return true;
}

bool is_allowed_install_path(StringView path)
{
    if (path.is_empty() || path[0] == '/')
        return false;
    bool bad = false;
    path.for_each_split_view('/', SplitBehavior::KeepEmpty, [&](StringView part) {
        if (part.is_empty() || part == "."_sv || part == ".."_sv)
            bad = true;
    });
    if (bad)
        return false;
    return path.starts_with("Applications/"_sv) || path.starts_with("usr/local/"_sv);
}

bool verify_signature(ReadonlyBytes data, StringView signature_hex, StringView public_key_hex)
{
    u8 signature[Crypto::ed25519_signature_size];
    u8 key[Crypto::ed25519_public_key_size];
    if (!decode_hex(signature_hex, { signature, sizeof(signature) }) || !decode_hex(public_key_hex, { key, sizeof(key) }))
        return false;
    return Crypto::ed25519_verify(key, data, signature);
}

ErrorOr<Vector<Available>> parse_index(ReadonlyBytes json, StringView base_url, StringView repository_name, u64* serial)
{
    auto root = TRY(JSON::parse({ reinterpret_cast<char const*>(json.data()), json.size() }));
    if (!root.is_object())
        return Error::from_string_literal("package index: not an object");
    if (serial) {
        auto number = root.get_number("serial"_sv);
        *serial = number.has_value() && number.value() >= 0 ? static_cast<u64>(number.value()) : 0;
    }
    auto const* packages = root.get("packages"_sv);
    if (!packages || !packages->is_array())
        return Error::from_string_literal("package index: no list of packages");
    Vector<Available> result;
    for (size_t i = 0; i < packages->size(); ++i) {
        auto const& entry = packages->at(i);
        if (!entry.is_object())
            return Error::from_string_literal("package index: an entry that isn't an object");
        auto const name = entry.get_string("name"_sv);
        auto const version = entry.get_string("version"_sv);
        auto const file = entry.get_string("file"_sv);
        auto const sha256 = entry.get_string("sha256"_sv);
        auto const size = entry.get_number("size"_sv);
        if (!name.has_value() || !version.has_value() || !file.has_value() || !sha256.has_value() || !size.has_value())
            return Error::from_string_literal("package index: an entry with something missing");
        if (!is_valid_name(name.value()) || !is_valid_name(version.value()) || !is_valid_name(file.value()))
            return Error::from_string_literal("package index: a name with odd characters");
        if (sha256.value().length() != 64)
            return Error::from_string_literal("package index: a bad hash");
        Available package;
        package.name = TRY(String::from_view(name.value()));
        package.version = TRY(String::from_view(version.value()));
        if (auto summary = entry.get_string("summary"_sv); summary.has_value())
            package.summary = TRY(String::from_view(summary.value()));
        StringBuilder url;
        url.append(base_url);
        if (!base_url.ends_with("/"_sv))
            url.append("/"_sv);
        url.append(file.value());
        package.url = TRY(url.to_string());
        package.sha256 = TRY(String::from_view(sha256.value()));
        package.size = static_cast<u64>(size.value());
        package.repository = TRY(String::from_view(repository_name));
        TRY(result.try_append(move(package)));
    }
    return result;
}

}
