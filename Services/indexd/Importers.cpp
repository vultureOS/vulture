/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Importers.h"
#include <ContentTypes.h>
#include <Desktop.h>
#include <LibBase/HashMap.h>
#include <LibUnicode/Unicode.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Index {

namespace {

constexpr size_t largest_text_file = 256 * 1024;
constexpr size_t most_words_in_a_file = 4096;
constexpr size_t longest_word = 64;

bool is_word_byte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || static_cast<u8>(c) >= 0x80;
}

bool is_ascii(StringView text)
{
    for (char c : text) {
        if (static_cast<u8>(c) >= 0x80)
            return false;
    }
    return true;
}

void add_once(HashMap<String, bool>& seen, Vector<String>& out, String word)
{
    if (seen.contains(word.view()))
        return;
    seen.set(word, true);
    out.append(move(word));
}

}

String fold(StringView text)
{
    if (is_ascii(text)) {
        StringBuilder lowered;
        for (char c : text)
            lowered.append(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
        return MUST(lowered.to_string());
    }
    auto folded = Unicode::case_fold(text);
    if (folded.is_error() || !Unicode::is_valid_utf8(text))
        return MUST(String::from_view(text));
    auto decomposed = Unicode::normalize(folded.value().view(), Unicode::NormalizationForm::NFD);
    if (decomposed.is_error())
        return folded.release_value();
    auto points = Unicode::code_points_of(decomposed.value().view());
    if (points.is_error())
        return decomposed.release_value();
    Vector<Unicode::CodePoint> kept;
    for (auto point : points.value()) {
        if (Unicode::combining_class(point) == 0)
            kept.append(point);
    }
    return MUST(Unicode::utf8_of(kept.span()));
}

void words_of(StringView text, Vector<String>& out, size_t most)
{
    HashMap<String, bool> seen;
    for (auto const& existing : out)
        seen.set(existing, true);
    size_t index = 0;
    while (index < text.length() && out.size() < most) {
        while (index < text.length() && !is_word_byte(text[index]))
            ++index;
        size_t start = index;
        while (index < text.length() && is_word_byte(text[index]))
            ++index;
        if (index == start)
            continue;
        // A word as long as a paragraph isn't a word: base64, usually.
        if (index - start > longest_word)
            continue;
        auto folded = fold(text.substring_view(start, index - start));
        if (folded.length() < 2)
            continue;
        add_once(seen, out, move(folded));
    }
}

void words_of_name(StringView name, Vector<String>& out)
{
    words_of(name, out, most_words_in_a_file);
    // TextEditor: text and editor as well. And report2024: report.
    HashMap<String, bool> seen;
    for (auto const& existing : out)
        seen.set(existing, true);
    size_t start = 0;
    auto finish = [&](size_t end) {
        if (end > start && end - start >= 2)
            add_once(seen, out, fold(name.substring_view(start, end - start)));
    };
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if (!is_word_byte(c)) {
            finish(i);
            start = i + 1;
            continue;
        }
        if (i > start) {
            char before = name[i - 1];
            bool lower_to_upper = before >= 'a' && before <= 'z' && c >= 'A' && c <= 'Z';
            bool letter_to_digit = ((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z')) && c >= '0' && c <= '9';
            if (lower_to_upper || letter_to_digit) {
                finish(i);
                start = i;
            }
        }
    }
    finish(name.length());
}

namespace {

ssize_t read_start(int fd, u8* buffer, size_t count)
{
    size_t done = 0;
    while (done < count) {
        ssize_t got = read(fd, buffer + done, count - done);
        if (got <= 0)
            break;
        done += static_cast<size_t>(got);
    }
    return static_cast<ssize_t>(done);
}

void import_png(int fd, Metadata& metadata)
{
    u8 header[24];
    if (read_start(fd, header, sizeof(header)) < 24 || memcmp(header, "\x89PNG\r\n\x1a\n", 8) != 0 || memcmp(header + 12, "IHDR", 4) != 0)
        return;
    auto big_endian = [&](size_t at) { return (static_cast<i64>(header[at]) << 24) | (header[at + 1] << 16) | (header[at + 2] << 8) | header[at + 3]; };
    metadata.pixel_width = big_endian(16);
    metadata.pixel_height = big_endian(20);
}

void import_jpeg(int fd, Metadata& metadata)
{
    // To the first frame header, past however many other segments there are.
    static u8 buffer[64 * 1024];
    ssize_t length = read_start(fd, buffer, sizeof(buffer));
    if (length < 4 || buffer[0] != 0xff || buffer[1] != 0xd8)
        return;
    ssize_t at = 2;
    while (at + 9 < length) {
        if (buffer[at] != 0xff) {
            ++at;
            continue;
        }
        u8 marker = buffer[at + 1];
        if (marker == 0xff) {
            ++at;
            continue;
        }
        bool const is_frame_header = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (is_frame_header) {
            metadata.pixel_height = (buffer[at + 5] << 8) | buffer[at + 6];
            metadata.pixel_width = (buffer[at + 7] << 8) | buffer[at + 8];
            return;
        }
        at += 2 + ((buffer[at + 2] << 8) | buffer[at + 3]);
    }
}

void import_text(int fd, size_t size, Metadata& metadata)
{
    if (size > largest_text_file)
        return;
    static char buffer[largest_text_file];
    ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got <= 0)
        return;
    // Text has no zero bytes in its first stretch.
    for (ssize_t i = 0; i < min<ssize_t>(got, 512); ++i) {
        if (buffer[i] == 0)
            return;
    }
    words_of({ buffer, static_cast<size_t>(got) }, metadata.words, most_words_in_a_file);
}

}

void import_file(StringView path, Metadata& metadata)
{
    auto type = Desktop::content_type_of_path(path);
    metadata.content_type = MUST(String::from_view(type));
    metadata.kind_description = MUST(String::from_view(Desktop::content_description_of_path(path)));

    if (type == "com.apple.application-bundle"_sv || type == "public.folder"_sv)
        return;
    auto terminated = MUST(String::from_view(path));
    int fd = open(terminated.characters(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return;
    struct stat information;
    if (fstat(fd, &information) == 0 && S_ISREG(information.st_mode)) {
        if (Desktop::content_type_conforms(type, "public.png"_sv))
            import_png(fd, metadata);
        else if (Desktop::content_type_conforms(type, "public.jpeg"_sv))
            import_jpeg(fd, metadata);
        else if (Desktop::content_type_conforms(type, "public.image"_sv) || Desktop::content_type_conforms(type, "public.audio"_sv) || Desktop::content_type_conforms(type, "public.movie"_sv) || Desktop::content_type_conforms(type, "public.archive"_sv)) {
            // Nothing to read in these yet.
        } else {
            import_text(fd, static_cast<size_t>(information.st_size), metadata);
        }
    }
    close(fd);
}

}
