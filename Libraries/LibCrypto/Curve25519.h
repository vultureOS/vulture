/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Types.h>

// X25519 key agreement (RFC 7748) and Ed25519 signatures (RFC 8032). Both work on the same
// field, and take the same time whatever the secrets are.

namespace Crypto {

inline constexpr size_t x25519_key_size = 32;

// `output` = `scalar` times the point with u-coordinate `point`. All three are 32 bytes.
void x25519(u8 const* scalar, u8 const* point, u8* output);
// The public key for a private one: `scalar` times the base point.
void x25519_base(u8 const* scalar, u8* output);

inline constexpr size_t ed25519_seed_size = 32;
inline constexpr size_t ed25519_public_key_size = 32;
inline constexpr size_t ed25519_signature_size = 64;

void ed25519_public_key(u8 const* seed, u8* public_key);
void ed25519_sign(u8 const* seed, ReadonlyBytes message, u8* signature);
// Refuses a signature with S not below the group's order, or a key that is not on the curve.
[[nodiscard]] bool ed25519_verify(u8 const* public_key, ReadonlyBytes message, u8 const* signature);

}
