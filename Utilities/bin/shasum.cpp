/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCrypto/SHA2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// shasum [-a 256|384|512] [file...]: as on macOS, but without SHA-1, which is broken.

namespace {

template<typename Hash>
bool hash_file(char const* path)
{
    int const fd = strcmp(path, "-") == 0 ? 0 : open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "shasum: %s: %s\n", path, strerror(errno));
        return false;
    }
    Hash hasher;
    static u8 chunk[1024 * 1024];
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "shasum: %s: %s\n", path, strerror(errno));
            if (fd != 0)
                close(fd);
            return false;
        }
        if (got == 0)
            break;
        hasher.update({ chunk, static_cast<size_t>(got) });
    }
    if (fd != 0)
        close(fd);
    auto const digest = hasher.digest();
    for (size_t i = 0; i < digest.size(); ++i)
        printf("%02x", digest[i]);
    printf("  %s\n", path);
    return true;
}

}

int main(int argc, char** argv)
{
    int algorithm = 256;
    int first = 1;
    if (argc >= 3 && strcmp(argv[1], "-a") == 0) {
        algorithm = atoi(argv[2]);
        first = 3;
    }
    if (algorithm != 256 && algorithm != 384 && algorithm != 512) {
        fprintf(stderr, "shasum: only SHA-256, SHA-384 and SHA-512 are here\n");
        return 1;
    }
    char const* standard_input[] = { "-" };
    char const* const* files = first < argc ? const_cast<char const* const*>(argv + first) : standard_input;
    int const count = first < argc ? argc - first : 1;
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        bool const hashed = algorithm == 256 ? hash_file<Crypto::SHA256>(files[i]) : (algorithm == 384 ? hash_file<Crypto::SHA384>(files[i]) : hash_file<Crypto::SHA512>(files[i]));
        ok = ok && hashed;
    }
    return ok ? 0 : 1;
}
