/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>

namespace Audio {

// Everything here is in the device's format: 16-bit stereo, interleaved, at 48 kHz.
inline constexpr u32 sample_rate = 48000;
inline constexpr u32 channel_count = 2;

class Sound final : public RefCounted<Sound> {
public:
    [[nodiscard]] static NonnullRefPtr<Sound> create(Vector<i16> samples)
    {
        return adopt_ref(*new Sound(move(samples)));
    }

    [[nodiscard]] size_t frame_count() const { return m_samples.size() / channel_count; }
    [[nodiscard]] double seconds() const { return static_cast<double>(frame_count()) / sample_rate; }
    [[nodiscard]] Span<i16 const> samples() const { return m_samples.span(); }

private:
    explicit Sound(Vector<i16> samples)
        : m_samples(move(samples))
    {
    }

    Vector<i16> m_samples;
};

}
