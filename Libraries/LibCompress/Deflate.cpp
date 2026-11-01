/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/StdLibExtras.h>
#include <LibCompress/Deflate.h>

namespace Compress {

namespace {

constexpr u16 length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr u8 length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr u16 distance_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr u8 distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

ErrorOr<void> corrupt()
{
    return Error::from_string_literal("invalid compressed data");
}

class BitReader {
public:
    explicit BitReader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    void refill()
    {
        while (m_count <= 56 && m_position < m_bytes.size()) {
            m_buffer |= static_cast<u64>(m_bytes[m_position++]) << m_count;
            m_count += 8;
        }
    }

    // Up to 32 bits at once; false if the input ends first.
    bool read(u32 count, u32& value)
    {
        if (m_count < count) {
            refill();
            if (m_count < count)
                return false;
        }
        value = static_cast<u32>(m_buffer & ((1ull << count) - 1));
        m_buffer >>= count;
        m_count -= count;
        return true;
    }

    [[nodiscard]] u64 peek() const { return m_buffer; }
    [[nodiscard]] u32 available() const { return m_count; }
    void consume(u32 count)
    {
        m_buffer >>= count;
        m_count -= count;
    }

    // To the next byte; what is left then comes byte by byte.
    void align_to_byte()
    {
        u32 extra = m_count % 8;
        consume(extra);
    }

    bool read_byte(u8& value)
    {
        u32 wide = 0;
        if (!read(8, wide))
            return false;
        value = static_cast<u8>(wide);
        return true;
    }

private:
    ReadonlyBytes m_bytes;
    size_t m_position { 0 };
    u64 m_buffer { 0 };
    u32 m_count { 0 };
};

constexpr u32 max_code_length = 15;

u32 reverse_bits(u32 value, u32 count)
{
    u32 result = 0;
    for (u32 i = 0; i < count; ++i) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}

// Canonical Huffman codes: a table for the short ones, a walk for the rest.
class HuffmanTable {
public:
    static constexpr u32 fast_bits = 10;

    ErrorOr<void> build(u8 const* lengths, u32 count)
    {
        u16 counts[max_code_length + 1] = {};
        for (u32 i = 0; i < count; ++i)
            ++counts[lengths[i]];
        counts[0] = 0;
        // An over-subscribed code is invalid; an incomplete one is allowed
        // (a single distance code is common), and running off it is an error.
        int left = 1;
        for (u32 length = 1; length <= max_code_length; ++length) {
            left = left * 2 - counts[length];
            if (left < 0)
                return corrupt();
        }
        u16 next_code[max_code_length + 2] = {};
        u16 code = 0;
        u16 index = 0;
        for (u32 length = 1; length <= max_code_length; ++length) {
            code = static_cast<u16>((code + (length > 1 ? counts[length - 1] : 0)) << 1);
            next_code[length] = code;
            m_first_code[length] = code;
            m_first_index[length] = index;
            m_count[length] = counts[length];
            index = static_cast<u16>(index + counts[length]);
        }
        u16 offsets[max_code_length + 1];
        for (u32 length = 1; length <= max_code_length; ++length)
            offsets[length] = m_first_index[length];
        __builtin_memset(m_fast, 0, sizeof(m_fast));
        for (u32 symbol = 0; symbol < count; ++symbol) {
            u32 length = lengths[symbol];
            if (length == 0)
                continue;
            m_symbols[offsets[length]++] = static_cast<u16>(symbol);
            u32 assigned = next_code[length]++;
            if (length <= fast_bits) {
                u32 reversed = reverse_bits(assigned, length);
                for (u32 fill = reversed; fill < (1u << fast_bits); fill += 1u << length)
                    m_fast[fill] = static_cast<u16>((symbol << 4) | length);
            }
        }
        return {};
    }

    // The next symbol, or -1 if the data is bad or ends.
    int decode(BitReader& reader) const
    {
        if (reader.available() < max_code_length)
            reader.refill();
        u16 entry = m_fast[reader.peek() & ((1u << fast_bits) - 1)];
        if (entry) {
            u32 length = entry & 15;
            if (length > reader.available())
                return -1;
            reader.consume(length);
            return entry >> 4;
        }
        // Bit by bit from the longest fast length on.
        u32 code = 0;
        u64 bits = reader.peek();
        for (u32 length = 1; length <= max_code_length; ++length) {
            if (length > reader.available())
                return -1;
            code = (code << 1) | (bits & 1);
            bits >>= 1;
            if (m_count[length] && code >= m_first_code[length] && code - m_first_code[length] < m_count[length]) {
                reader.consume(length);
                return m_symbols[m_first_index[length] + code - m_first_code[length]];
            }
        }
        return -1;
    }

private:
    u16 m_fast[1 << fast_bits];
    u16 m_symbols[288];
    u16 m_first_code[max_code_length + 1] {};
    u16 m_first_index[max_code_length + 1] {};
    u16 m_count[max_code_length + 1] {};
};

ErrorOr<void> inflate_block(BitReader& reader, HuffmanTable const& literals, HuffmanTable const& distances, Vector<u8>& output, size_t limit)
{
    for (;;) {
        int symbol = literals.decode(reader);
        if (symbol < 0)
            return corrupt();
        if (symbol < 256) {
            if (output.size() >= limit)
                return Error::from_string_literal("decompressed data too large");
            TRY(output.try_append(static_cast<u8>(symbol)));
            continue;
        }
        if (symbol == 256)
            return {};
        symbol -= 257;
        if (symbol >= 29)
            return corrupt();
        u32 extra = 0;
        if (!reader.read(length_extra[symbol], extra))
            return corrupt();
        size_t length = length_base[symbol] + extra;
        int distance_symbol = distances.decode(reader);
        if (distance_symbol < 0 || distance_symbol >= 30)
            return corrupt();
        if (!reader.read(distance_extra[distance_symbol], extra))
            return corrupt();
        size_t distance = distance_base[distance_symbol] + extra;
        if (distance > output.size())
            return corrupt();
        if (output.size() + length > limit)
            return Error::from_string_literal("decompressed data too large");
        size_t start = output.size();
        TRY(output.try_resize(start + length));
        u8* data = output.data();
        // One byte at a time: a copy may overlap what it is producing.
        for (size_t i = 0; i < length; ++i)
            data[start + i] = data[start + i - distance];
    }
}

}

ErrorOr<Vector<u8>> inflate(ReadonlyBytes input, size_t limit)
{
    BitReader reader(input);
    Vector<u8> output;
    auto* literals = new (Base::nothrow) HuffmanTable;
    auto* distances = new (Base::nothrow) HuffmanTable;
    if (!literals || !distances) {
        delete literals;
        delete distances;
        return Error::from_string_literal("out of memory");
    }
    struct Tables {
        HuffmanTable* a;
        HuffmanTable* b;
        ~Tables()
        {
            delete a;
            delete b;
        }
    } owner { literals, distances };

    for (;;) {
        u32 is_final = 0, type = 0;
        if (!reader.read(1, is_final) || !reader.read(2, type))
            TRY(corrupt());
        if (type == 0) {
            reader.align_to_byte();
            u32 length = 0, inverse = 0;
            if (!reader.read(16, length) || !reader.read(16, inverse) || (length ^ 0xffff) != inverse)
                TRY(corrupt());
            if (output.size() + length > limit)
                return Error::from_string_literal("decompressed data too large");
            for (u32 i = 0; i < length; ++i) {
                u8 byte = 0;
                if (!reader.read_byte(byte))
                    TRY(corrupt());
                TRY(output.try_append(byte));
            }
        } else if (type == 1) {
            u8 lengths[288];
            for (u32 i = 0; i < 288; ++i)
                lengths[i] = i < 144 ? 8 : (i < 256 ? 9 : (i < 280 ? 7 : 8));
            TRY(literals->build(lengths, 288));
            u8 distance_lengths[30];
            for (auto& length : distance_lengths)
                length = 5;
            TRY(distances->build(distance_lengths, 30));
            TRY(inflate_block(reader, *literals, *distances, output, limit));
        } else if (type == 2) {
            u32 literal_count = 0, distance_count = 0, code_count = 0;
            if (!reader.read(5, literal_count) || !reader.read(5, distance_count) || !reader.read(4, code_count))
                TRY(corrupt());
            literal_count += 257;
            distance_count += 1;
            code_count += 4;
            if (literal_count > 286 || distance_count > 30)
                TRY(corrupt());
            static constexpr u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            u8 code_lengths[19] = {};
            for (u32 i = 0; i < code_count; ++i) {
                u32 value = 0;
                if (!reader.read(3, value))
                    TRY(corrupt());
                code_lengths[order[i]] = static_cast<u8>(value);
            }
            TRY(literals->build(code_lengths, 19));
            u8 lengths[320] = {};
            u32 total = literal_count + distance_count;
            for (u32 i = 0; i < total;) {
                int symbol = literals->decode(reader);
                if (symbol < 0)
                    TRY(corrupt());
                if (symbol < 16) {
                    lengths[i++] = static_cast<u8>(symbol);
                    continue;
                }
                u32 repeat = 0;
                u8 value = 0;
                if (symbol == 16) {
                    if (i == 0 || !reader.read(2, repeat))
                        TRY(corrupt());
                    value = lengths[i - 1];
                    repeat += 3;
                } else if (symbol == 17) {
                    if (!reader.read(3, repeat))
                        TRY(corrupt());
                    repeat += 3;
                } else {
                    if (!reader.read(7, repeat))
                        TRY(corrupt());
                    repeat += 11;
                }
                if (i + repeat > total)
                    TRY(corrupt());
                while (repeat--)
                    lengths[i++] = value;
            }
            if (lengths[256] == 0)
                TRY(corrupt());
            TRY(literals->build(lengths, literal_count));
            TRY(distances->build(lengths + literal_count, distance_count));
            TRY(inflate_block(reader, *literals, *distances, output, limit));
        } else {
            TRY(corrupt());
        }
        if (is_final)
            return output;
    }
}

u32 adler32(ReadonlyBytes bytes)
{
    u32 a = 1, b = 0;
    size_t position = 0;
    while (position < bytes.size()) {
        // The sums stay below 2^32 for this many bytes.
        size_t chunk = min<size_t>(bytes.size() - position, 5552);
        for (size_t i = 0; i < chunk; ++i) {
            a += bytes[position + i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        position += chunk;
    }
    return (b << 16) | a;
}

ErrorOr<Vector<u8>> zlib_decompress(ReadonlyBytes input, size_t limit)
{
    if (input.size() < 6)
        return Error::from_string_literal("invalid compressed data");
    u8 method = input[0];
    u8 flags = input[1];
    if ((method & 15) != 8 || (method >> 4) > 7 || ((method << 8) | flags) % 31 != 0 || (flags & 0x20))
        return Error::from_string_literal("invalid zlib header");
    auto output = TRY(inflate({ input.data() + 2, input.size() - 2 }, limit));
    // The checksum is the last four bytes (anything after the stream is not
    // looked for: PNG files have none).
    u8 const* tail = input.data() + input.size() - 4;
    u32 expected = (static_cast<u32>(tail[0]) << 24) | (static_cast<u32>(tail[1]) << 16) | (static_cast<u32>(tail[2]) << 8) | tail[3];
    if (adler32(output.span()) != expected)
        return Error::from_string_literal("checksum mismatch in compressed data");
    return output;
}

namespace {

class BitWriter {
public:
    explicit BitWriter(Vector<u8>& output)
        : m_output(output)
    {
    }

    ErrorOr<void> write(u32 value, u32 count)
    {
        m_buffer |= static_cast<u64>(value) << m_count;
        m_count += count;
        while (m_count >= 8) {
            TRY(m_output.try_append(static_cast<u8>(m_buffer)));
            m_buffer >>= 8;
            m_count -= 8;
        }
        return {};
    }

    ErrorOr<void> finish()
    {
        if (m_count > 0) {
            TRY(m_output.try_append(static_cast<u8>(m_buffer)));
            m_buffer = 0;
            m_count = 0;
        }
        return {};
    }

private:
    Vector<u8>& m_output;
    u64 m_buffer { 0 };
    u32 m_count { 0 };
};

// The fixed literal/length code (Huffman codes go most significant bit first).
ErrorOr<void> write_fixed_symbol(BitWriter& writer, u32 symbol)
{
    if (symbol < 144)
        return writer.write(reverse_bits(0x30 + symbol, 8), 8);
    if (symbol < 256)
        return writer.write(reverse_bits(0x190 + (symbol - 144), 9), 9);
    if (symbol < 280)
        return writer.write(reverse_bits(symbol - 256, 7), 7);
    return writer.write(reverse_bits(0xc0 + (symbol - 280), 8), 8);
}

}

// LZ77 with hash chains and the fixed Huffman code: simple, and compresses
// screenshots and icons well.
ErrorOr<Vector<u8>> deflate(ReadonlyBytes input)
{
    Vector<u8> output;
    BitWriter writer(output);
    TRY(writer.write(1, 1)); // The final block...
    TRY(writer.write(1, 2)); // ...with fixed codes.

    constexpr u32 hash_bits = 15;
    constexpr u32 hash_size = 1u << hash_bits;
    constexpr u32 window = 32768;
    constexpr u32 max_chain = 32;
    Vector<i32> heads;
    Vector<i32> previous;
    TRY(heads.try_resize(hash_size));
    TRY(previous.try_resize(window));
    for (auto& head : heads)
        head = -1;
    auto hash_at = [&](size_t i) {
        u32 value = static_cast<u32>(input[i]) | (static_cast<u32>(input[i + 1]) << 8) | (static_cast<u32>(input[i + 2]) << 16);
        return (value * 2654435761u) >> (32 - hash_bits);
    };
    auto insert = [&](size_t i) {
        if (i + 3 > input.size())
            return;
        u32 hash = hash_at(i);
        previous[i % window] = heads[hash];
        heads[hash] = static_cast<i32>(i);
    };

    size_t position = 0;
    while (position < input.size()) {
        size_t best_length = 0;
        size_t best_distance = 0;
        if (position + 3 <= input.size()) {
            i32 candidate = heads[hash_at(position)];
            size_t longest = min<size_t>(input.size() - position, 258);
            for (u32 chain = 0; chain < max_chain && candidate >= 0; ++chain) {
                size_t from = static_cast<size_t>(candidate);
                if (position - from > window - 1)
                    break;
                size_t length = 0;
                while (length < longest && input[from + length] == input[position + length])
                    ++length;
                if (length > best_length) {
                    best_length = length;
                    best_distance = position - from;
                    if (length == longest)
                        break;
                }
                i32 next = previous[from % window];
                if (next >= candidate)
                    break;
                candidate = next;
            }
        }
        if (best_length >= 3) {
            u32 code = 28;
            while (best_length < length_base[code])
                --code;
            TRY(write_fixed_symbol(writer, 257 + code));
            TRY(writer.write(static_cast<u32>(best_length - length_base[code]), length_extra[code]));
            u32 distance_code = 29;
            while (best_distance < distance_base[distance_code])
                --distance_code;
            TRY(writer.write(reverse_bits(distance_code, 5), 5));
            TRY(writer.write(static_cast<u32>(best_distance - distance_base[distance_code]), distance_extra[distance_code]));
            for (size_t i = 0; i < best_length; ++i)
                insert(position + i);
            position += best_length;
        } else {
            TRY(write_fixed_symbol(writer, input[position]));
            insert(position);
            ++position;
        }
    }
    TRY(write_fixed_symbol(writer, 256));
    TRY(writer.finish());
    return output;
}

ErrorOr<Vector<u8>> zlib_compress(ReadonlyBytes input)
{
    auto compressed = TRY(deflate(input));
    Vector<u8> output;
    TRY(output.try_ensure_capacity(compressed.size() + 6));
    TRY(output.try_append(static_cast<u8>(0x78)));
    TRY(output.try_append(static_cast<u8>(0x9c)));
    for (u8 byte : compressed)
        TRY(output.try_append(byte));
    u32 checksum = adler32(input);
    TRY(output.try_append(static_cast<u8>(checksum >> 24)));
    TRY(output.try_append(static_cast<u8>(checksum >> 16)));
    TRY(output.try_append(static_cast<u8>(checksum >> 8)));
    TRY(output.try_append(static_cast<u8>(checksum)));
    return output;
}

}
