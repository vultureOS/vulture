/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAudio/Sound.h>
#include <LibBase/Error.h>

namespace Audio {

// A RIFF WAVE file of integer PCM, 8, 16 or 24 bits, one channel or more, at any rate:
// brought to the device's format (more than two channels, the first two; another rate,
// by straight lines between samples). Every length in the file is checked against the
// file's real size.
[[nodiscard]] ErrorOr<NonnullRefPtr<Sound const>> decode_wav(ReadonlyBytes);
[[nodiscard]] ErrorOr<Vector<u8>> encode_wav(Sound const&);

}
