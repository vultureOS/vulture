/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibBase/Types.h>
#include <LibCrypto/AES.h>
#include <LibCrypto/GCM.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/random.h>

// The processor's AES and carry-less multiply against the plain code: the same answers, for
// everything, or a secret could end up in the clear on some machines and not others. (The
// known answers for the plain code are in Tests/LibCrypto.)

int main()
{
    printf("crypto: hardware capabilities %#lx, AES %s\n", getauxval(AT_HWCAP), Crypto::AES::has_hardware() ? "in hardware" : "in software");
    if (!Crypto::AES::has_hardware())
        return finish();

    static u8 plain[4096];
    static u8 with_hardware[4096];
    static u8 without[4096];
    for (int round = 0; round < 64; ++round) {
        u8 key[32];
        u8 nonce[12];
        u8 associated[37];
        CHECK(getentropy(key, sizeof(key)) == 0);
        CHECK(getentropy(nonce, sizeof(nonce)) == 0);
        CHECK(getentropy(associated, sizeof(associated)) == 0);
        size_t const length = 1 + static_cast<size_t>(round) * 61;
        CHECK(getentropy(plain, 256) == 0);
        memcpy(plain + 256, plain, sizeof(plain) - 256);
        size_t const key_size = round % 2 ? 16 : 32;

        u8 tag_hardware[16];
        u8 tag_software[16];
        Crypto::AES::set_hardware_allowed(true);
        {
            Crypto::AESGCM gcm({ key, key_size });
            gcm.seal(nonce, { associated, sizeof(associated) }, { plain, length }, with_hardware, tag_hardware);
        }
        Crypto::AES::set_hardware_allowed(false);
        {
            Crypto::AESGCM gcm({ key, key_size });
            gcm.seal(nonce, { associated, sizeof(associated) }, { plain, length }, without, tag_software);
            // And each opens what the other sealed.
            static u8 opened[4096];
            CHECK(gcm.open(nonce, { associated, sizeof(associated) }, { with_hardware, length }, tag_hardware, opened));
            CHECK(memcmp(opened, plain, length) == 0);
        }
        CHECK(memcmp(with_hardware, without, length) == 0);
        CHECK(memcmp(tag_hardware, tag_software, 16) == 0);
        Crypto::AES::set_hardware_allowed(true);
    }
    return finish();
}
