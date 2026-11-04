/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

// dd [if=file] [of=file] [bs=n] [ibs=n] [obs=n] [count=n] [skip=n] [seek=n] [conv=notrunc,sync,noerror,swab,ucase,lcase]

namespace {

long long parse_size(char const* text)
{
    char* end = nullptr;
    long long value = strtoll(text, &end, 0);
    for (; end && *end;) {
        switch (*end) {
        case 'b':
            value *= 512;
            break;
        case 'k':
        case 'K':
            value *= 1024;
            break;
        case 'm':
        case 'M':
            value *= 1024 * 1024;
            break;
        case 'g':
        case 'G':
            value *= 1024 * 1024 * 1024;
            break;
        case 'w':
            value *= 4;
            break;
        case 'x':
            return value * parse_size(end + 1);
        default:
            fprintf(stderr, "dd: %s: illegal numeric value\n", text);
            exit(1);
        }
        ++end;
    }
    return value;
}

double now()
{
    timespec value {};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
}

}

int main(int argc, char** argv)
{
    char const* input = nullptr;
    char const* output = nullptr;
    long long input_block = 512, output_block = 512, count = -1, skip = 0, seek = 0;
    bool no_truncate = false, sync_blocks = false, no_error = false, swap_bytes = false;
    bool to_upper = false, to_lower = false;
    for (int i = 1; i < argc; ++i) {
        char const* equals = strchr(argv[i], '=');
        if (!equals) {
            fprintf(stderr, "dd: unknown operand %s\n", argv[i]);
            return 1;
        }
        size_t const length = static_cast<size_t>(equals - argv[i]);
        char const* value = equals + 1;
        auto is = [&](char const* name) { return strlen(name) == length && strncmp(argv[i], name, length) == 0; };
        if (is("if"))
            input = value;
        else if (is("of"))
            output = value;
        else if (is("bs"))
            input_block = output_block = parse_size(value);
        else if (is("ibs"))
            input_block = parse_size(value);
        else if (is("obs"))
            output_block = parse_size(value);
        else if (is("count"))
            count = parse_size(value);
        else if (is("skip") || is("iseek"))
            skip = parse_size(value);
        else if (is("seek") || is("oseek"))
            seek = parse_size(value);
        else if (is("conv")) {
            char conversions[256];
            snprintf(conversions, sizeof(conversions), "%s", value);
            for (char* word = strtok(conversions, ","); word; word = strtok(nullptr, ",")) {
                if (strcmp(word, "notrunc") == 0)
                    no_truncate = true;
                else if (strcmp(word, "sync") == 0)
                    sync_blocks = true;
                else if (strcmp(word, "noerror") == 0)
                    no_error = true;
                else if (strcmp(word, "swab") == 0)
                    swap_bytes = true;
                else if (strcmp(word, "ucase") == 0)
                    to_upper = true;
                else if (strcmp(word, "lcase") == 0)
                    to_lower = true;
                else {
                    fprintf(stderr, "dd: unknown conversion %s\n", word);
                    return 1;
                }
            }
        } else {
            fprintf(stderr, "dd: unknown operand %.*s\n", static_cast<int>(length), argv[i]);
            return 1;
        }
    }
    if (input_block <= 0 || output_block <= 0) {
        fprintf(stderr, "dd: bs must be between 1 and a very large number\n");
        return 1;
    }
    int const in = input ? open(input, O_RDONLY) : STDIN_FILENO;
    if (in < 0) {
        warn("dd", input);
        return 1;
    }
    int const out = output ? open(output, O_WRONLY | O_CREAT | ((seek || no_truncate) ? 0 : O_TRUNC), 0666) : STDOUT_FILENO;
    if (out < 0) {
        warn("dd", output);
        return 1;
    }
    auto* input_buffer = static_cast<char*>(malloc(static_cast<size_t>(input_block)));
    auto* output_buffer = static_cast<char*>(malloc(static_cast<size_t>(output_block)));
    if (!input_buffer || !output_buffer) {
        fprintf(stderr, "dd: not enough memory\n");
        return 1;
    }
    for (long long i = 0; i < skip; ++i) {
        if (lseek(in, input_block, SEEK_CUR) < 0) {
            if (read(in, input_buffer, static_cast<size_t>(input_block)) <= 0)
                break;
        }
    }
    if (seek > 0)
        (void)lseek(out, seek * output_block, SEEK_CUR);

    long long full_in = 0, partial_in = 0, full_out = 0, partial_out = 0, bytes = 0;
    long long pending = 0;
    int status = 0;
    double const started = now();
    auto write_out = [&](char const* data, long long length) {
        long long done = 0;
        while (done < length) {
            ssize_t const wrote = write(out, data + done, static_cast<size_t>(length - done));
            if (wrote < 0) {
                warn("dd", output ? output : "stdout");
                status = 1;
                return false;
            }
            done += wrote;
        }
        bytes += length;
        if (length == output_block)
            ++full_out;
        else
            ++partial_out;
        return true;
    };
    for (long long block = 0; count < 0 || block < count; ++block) {
        ssize_t got = read(in, input_buffer, static_cast<size_t>(input_block));
        if (got < 0) {
            warn("dd", input ? input : "stdin");
            if (!no_error) {
                status = 1;
                break;
            }
            got = 0;
            if (sync_blocks)
                got = 0;
            else
                continue;
        } else if (got == 0) {
            break;
        }
        if (got == input_block)
            ++full_in;
        else if (got > 0)
            ++partial_in;
        long long length = got;
        if (sync_blocks && length < input_block) {
            memset(input_buffer + length, 0, static_cast<size_t>(input_block - length));
            length = input_block;
        }
        for (long long i = 0; (to_upper || to_lower) && i < length; ++i) {
            char& c = input_buffer[i];
            if (to_upper && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            else if (to_lower && c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        if (swap_bytes) {
            for (long long i = 0; i + 1 < length; i += 2) {
                char const held = input_buffer[i];
                input_buffer[i] = input_buffer[i + 1];
                input_buffer[i + 1] = held;
            }
        }
        long long from = 0;
        while (from < length) {
            long long const take = min(length - from, output_block - pending);
            memcpy(output_buffer + pending, input_buffer + from, static_cast<size_t>(take));
            pending += take;
            from += take;
            if (pending == output_block) {
                if (!write_out(output_buffer, pending))
                    goto finished;
                pending = 0;
            }
        }
    }
    if (pending > 0)
        (void)write_out(output_buffer, pending);
finished:
    double const seconds = now() - started;
    fprintf(stderr, "%lld+%lld records in\n%lld+%lld records out\n", full_in, partial_in, full_out, partial_out);
    fprintf(stderr, "%lld bytes transferred in %.6f secs (%.0f bytes/sec)\n", bytes, seconds, seconds > 0 ? static_cast<double>(bytes) / seconds : 0);
    return status;
}
