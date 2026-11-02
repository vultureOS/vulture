/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// /dev/audio0: writes play. Always signed 16-bit samples, little-endian, two channels
// interleaved, at 48 kHz; whole frames only. Writes wait while the device has as much as it
// can hold, about 85 ms, and poll() says when there is room.
#define VULTURE_AUDIO_SAMPLE_RATE 48000
#define VULTURE_AUDIO_CHANNELS 2
#define VULTURE_AUDIO_BYTES_PER_FRAME 4
#define VULTURE_AUDIO_PERIOD_BYTES 4096
