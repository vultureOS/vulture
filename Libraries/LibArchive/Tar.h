/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>

// The POSIX "ustar" archive format, which holds the boot archive (the
// initial root filesystem). Standard tools can list and unpack it, which
// helps debugging. Every header is checked (checksum, bounds) on read.

namespace Archive {

enum class TarEntryType : u8 {
    File,
    Directory,
    SymbolicLink,
    Other,
};

struct TarEntry {
    // Relative, without "./" or a trailing slash: "sbin/init". Valid only
    // while the visiting callback runs.
    StringView path;
    TarEntryType type { TarEntryType::Other };
    u32 mode { 0 };
    ReadonlyBytes data;
    StringView link_target;
};

enum class IterationDecision {
    Continue,
    Break,
};

class TarReader {
public:
    [[nodiscard]] static ErrorOr<TarReader> parse(ReadonlyBytes archive);

    // Visits every entry; `callback(TarEntry const&)` returns an IterationDecision.
    template<typename Callback>
    ErrorOr<void> for_each_entry(Callback callback) const
    {
        return walk([](void* context, TarEntry const& entry) {
            return (*static_cast<Callback*>(context))(entry);
        },
            &callback);
    }

    // "/sbin/init", "sbin/init" and "./sbin/init" all name the same entry.
    // The result's path refers to `path`'s characters.
    [[nodiscard]] Optional<TarEntry> find(StringView path) const;

    [[nodiscard]] static StringView normalize(StringView path);

private:
    using Visitor = IterationDecision (*)(void* context, TarEntry const&);

    TarReader() = default;
    ErrorOr<void> walk(Visitor, void* context) const;

    ReadonlyBytes m_archive;
};

// Whether unpacking this member stays inside the folder it goes in: relative, with no
// "..". An archive says what it likes, so anything that unpacks one asks. (Links can
// lead elsewhere too: whoever unpacks must also refuse to go through any link it made.)
[[nodiscard]] bool is_safe_member_path(StringView path);
// Whether a link at member `path` leads to somewhere inside as well.
[[nodiscard]] bool is_safe_link_target(StringView path, StringView target);

// Writes an archive through a sink (a file in the host tool).
class TarSink {
public:
    virtual void write(ReadonlyBytes) = 0;

protected:
    ~TarSink() = default;
};

class TarWriter {
public:
    explicit TarWriter(TarSink& sink)
        : m_sink(sink)
    {
    }

    ErrorOr<void> add_directory(StringView path, u32 mode);
    ErrorOr<void> add_file(StringView path, u32 mode, ReadonlyBytes data);
    ErrorOr<void> add_symbolic_link(StringView path, StringView target);
    // Writes the two zero blocks that end an archive.
    void finish();

private:
    ErrorOr<void> write_header(StringView path, TarEntryType, u32 mode, u64 size, StringView link_target = {});

    TarSink& m_sink;
};

}
