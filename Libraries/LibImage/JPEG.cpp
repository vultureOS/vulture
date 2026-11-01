/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/StdLibExtras.h>
#include <LibGfx/Color.h>
#include <LibImage/Image.h>

// JPEG (ITU T.81): Huffman-coded, baseline, extended sequential and
// progressive; grey, YCbCr, RGB and CMYK. Coefficients are kept per component
// until every scan is in, then transformed, upsampled and converted.

namespace Image {

namespace {

constexpr u8 zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

Error corrupt()
{
    return Error::from_string_literal("JPEG: invalid data");
}

struct HuffmanTable {
    bool defined { false };
    // By code length: the first code, how many, and where their values start.
    i32 first_code[17] {};
    i32 count[17] {};
    i32 first_index[17] {};
    u8 values[256] {};
    // Short codes at a look: (length << 8) | value, or 0.
    u16 fast[256] {};

    ErrorOr<void> build(u8 const* lengths, u8 const* symbol_values, size_t total)
    {
        i32 code = 0;
        i32 index = 0;
        __builtin_memset(fast, 0, sizeof(fast));
        for (u32 length = 1; length <= 16; ++length) {
            first_code[length] = code;
            count[length] = lengths[length - 1];
            first_index[length] = index;
            for (i32 i = 0; i < count[length]; ++i) {
                if (length <= 8) {
                    u32 shifted = static_cast<u32>(code + i) << (8 - length);
                    if (shifted + (1u << (8 - length)) > 256)
                        return corrupt();
                    for (u32 fill = 0; fill < (1u << (8 - length)); ++fill)
                        fast[shifted + fill] = static_cast<u16>((length << 8) | symbol_values[index + i]);
                }
            }
            index += count[length];
            code = (code + count[length]) << 1;
        }
        if (static_cast<size_t>(index) != total)
            return corrupt();
        __builtin_memcpy(values, symbol_values, total);
        defined = true;
        return {};
    }
};

struct Component {
    u8 id { 0 };
    u8 horizontal { 1 };
    u8 vertical { 1 };
    u8 quantization { 0 };
    u8 dc_table { 0 };
    u8 ac_table { 0 };
    int dc_predictor { 0 };
    // In blocks, whole MCUs' worth.
    int blocks_wide { 0 };
    int blocks_high { 0 };
    Vector<i16> coefficients;
    // After the transform: blocks_wide * 8 by blocks_high * 8 samples.
    Vector<u8> samples;

    [[nodiscard]] i16* block(int x, int y) { return coefficients.data() + (static_cast<size_t>(y) * static_cast<size_t>(blocks_wide) + static_cast<size_t>(x)) * 64; }
};

class Decoder {
public:
    explicit Decoder(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode()
    {
        TRY(read_markers());
        if (!m_seen_frame || !m_seen_scan)
            return Error::from_string_literal("JPEG: no image in the file");
        for (u32 i = 0; i < m_component_count; ++i)
            TRY(transform_component(m_components[i]));
        return assemble();
    }

private:
    [[nodiscard]] u32 read_u16(size_t at) const { return (static_cast<u32>(m_bytes[at]) << 8) | m_bytes[at + 1]; }

    ErrorOr<void> read_markers()
    {
        if (m_bytes.size() < 2 || m_bytes[0] != 0xff || m_bytes[1] != 0xd8)
            return Error::from_string_literal("JPEG: not a JPEG file");
        size_t position = 2;
        for (;;) {
            // Fill bytes may precede a marker.
            while (position < m_bytes.size() && m_bytes[position] != 0xff)
                ++position;
            while (position < m_bytes.size() && m_bytes[position] == 0xff)
                ++position;
            if (position >= m_bytes.size()) {
                // A file cut short still shows what came.
                if (m_seen_scan)
                    return {};
                return corrupt();
            }
            u8 marker = m_bytes[position++];
            if (marker == 0xd9)
                return {};
            if (marker == 0 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
                continue;
            if (position + 2 > m_bytes.size())
                return corrupt();
            u32 length = read_u16(position);
            if (length < 2 || position + length > m_bytes.size())
                return corrupt();
            size_t body = position + 2;
            size_t body_size = length - 2;
            switch (marker) {
            case 0xdb:
                TRY(read_quantization(body, body_size));
                break;
            case 0xc4:
                TRY(read_huffman(body, body_size));
                break;
            case 0xc0:
            case 0xc1:
            case 0xc2:
                TRY(read_frame(marker, body, body_size));
                break;
            case 0xc3:
            case 0xc5:
            case 0xc6:
            case 0xc7:
            case 0xc9:
            case 0xca:
            case 0xcb:
            case 0xcd:
            case 0xce:
            case 0xcf:
                return Error::from_string_literal("JPEG: a kind this decoder does not handle");
            case 0xdd:
                if (body_size < 2)
                    return corrupt();
                m_restart_interval = read_u16(body);
                break;
            case 0xee:
                if (body_size >= 12 && __builtin_memcmp(m_bytes.data() + body, "Adobe", 5) == 0) {
                    m_has_adobe = true;
                    m_adobe_transform = m_bytes[body + 11];
                }
                break;
            case 0xda:
                position = TRY(read_scan(body, body_size));
                m_seen_scan = true;
                continue;
            default:
                break;
            }
            position += length;
        }
    }

    ErrorOr<void> read_quantization(size_t at, size_t size)
    {
        size_t end = at + size;
        while (at < end) {
            u8 info = m_bytes[at++];
            u8 precision = info >> 4;
            u8 index = info & 15;
            if (index > 3 || precision > 1 || at + (precision ? 128u : 64u) > end)
                return corrupt();
            for (u32 i = 0; i < 64; ++i) {
                u16 value = precision ? static_cast<u16>(read_u16(at + i * 2)) : m_bytes[at + i];
                m_quantization[index][zigzag[i]] = value;
            }
            at += precision ? 128 : 64;
        }
        return {};
    }

    ErrorOr<void> read_huffman(size_t at, size_t size)
    {
        size_t end = at + size;
        while (at < end) {
            if (at + 17 > end)
                return corrupt();
            u8 info = m_bytes[at++];
            u8 table_class = info >> 4;
            u8 index = info & 15;
            if (table_class > 1 || index > 3)
                return corrupt();
            u8 const* lengths = m_bytes.data() + at;
            size_t total = 0;
            for (u32 i = 0; i < 16; ++i)
                total += lengths[i];
            at += 16;
            if (total > 256 || at + total > end)
                return corrupt();
            TRY((table_class ? m_ac_tables : m_dc_tables)[index].build(lengths, m_bytes.data() + at, total));
            at += total;
        }
        return {};
    }

    ErrorOr<void> read_frame(u8 marker, size_t at, size_t size)
    {
        if (m_seen_frame || size < 6)
            return corrupt();
        if (m_bytes[at] != 8)
            return Error::from_string_literal("JPEG: only 8-bit samples are handled");
        m_progressive = marker == 0xc2;
        m_height = static_cast<int>(read_u16(at + 1));
        m_width = static_cast<int>(read_u16(at + 3));
        m_component_count = m_bytes[at + 5];
        if (m_width == 0 || m_height == 0 || (m_component_count != 1 && m_component_count != 3 && m_component_count != 4) || size < 6 + 3u * m_component_count)
            return Error::from_string_literal("JPEG: unsupported frame");
        for (u32 i = 0; i < m_component_count; ++i) {
            auto& component = m_components[i];
            component.id = m_bytes[at + 6 + i * 3];
            component.horizontal = m_bytes[at + 7 + i * 3] >> 4;
            component.vertical = m_bytes[at + 7 + i * 3] & 15;
            component.quantization = m_bytes[at + 8 + i * 3];
            if (component.horizontal < 1 || component.horizontal > 4 || component.vertical < 1 || component.vertical > 4 || component.quantization > 3)
                return corrupt();
            m_max_horizontal = max<u32>(m_max_horizontal, component.horizontal);
            m_max_vertical = max<u32>(m_max_vertical, component.vertical);
        }
        m_mcus_wide = (m_width + static_cast<int>(m_max_horizontal) * 8 - 1) / (static_cast<int>(m_max_horizontal) * 8);
        m_mcus_high = (m_height + static_cast<int>(m_max_vertical) * 8 - 1) / (static_cast<int>(m_max_vertical) * 8);
        for (u32 i = 0; i < m_component_count; ++i) {
            auto& component = m_components[i];
            component.blocks_wide = m_mcus_wide * component.horizontal;
            component.blocks_high = m_mcus_high * component.vertical;
            size_t count = static_cast<size_t>(component.blocks_wide) * static_cast<size_t>(component.blocks_high) * 64;
            TRY(component.coefficients.try_resize(count));
            __builtin_memset(component.coefficients.data(), 0, count * sizeof(i16));
        }
        m_seen_frame = true;
        return {};
    }

    // --- Entropy-coded data ---

    void fill_bits()
    {
        while (m_bit_count <= 24) {
            u32 byte = 0;
            if (!m_hit_marker && m_scan_position < m_bytes.size()) {
                byte = m_bytes[m_scan_position];
                if (byte == 0xff) {
                    u8 next = m_scan_position + 1 < m_bytes.size() ? m_bytes[m_scan_position + 1] : 0xd9;
                    if (next == 0) {
                        m_scan_position += 2;
                    } else {
                        // A marker ends the data; zeros are read past it.
                        m_hit_marker = true;
                        byte = 0;
                    }
                } else {
                    ++m_scan_position;
                }
            }
            m_bit_buffer |= byte << (24 - m_bit_count);
            m_bit_count += 8;
        }
    }

    u32 read_bits(u32 count)
    {
        if (count == 0)
            return 0;
        if (m_bit_count < count)
            fill_bits();
        u32 value = m_bit_buffer >> (32 - count);
        m_bit_buffer <<= count;
        m_bit_count -= count;
        return value;
    }

    u32 read_bit() { return read_bits(1); }

    // The value of `size` more bits, extended to its sign as T.81 F.2.2.1 has it.
    int receive_extend(u32 size)
    {
        if (size == 0)
            return 0;
        int value = static_cast<int>(read_bits(size));
        return value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
    }

    ErrorOr<u8> decode_symbol(HuffmanTable const& table)
    {
        if (m_bit_count < 16)
            fill_bits();
        u16 entry = table.fast[m_bit_buffer >> 24];
        if (entry) {
            u32 length = entry >> 8;
            m_bit_buffer <<= length;
            m_bit_count -= length;
            return static_cast<u8>(entry);
        }
        i32 code = 0;
        for (u32 length = 1; length <= 16; ++length) {
            code = (code << 1) | static_cast<i32>(read_bit());
            if (table.count[length] && code >= table.first_code[length] && code - table.first_code[length] < table.count[length])
                return table.values[table.first_index[length] + code - table.first_code[length]];
        }
        return Error::from_string_literal("JPEG: bad Huffman code");
    }

    // Past a restart marker, if one is due: the data starts afresh.
    void restart()
    {
        m_bit_buffer = 0;
        m_bit_count = 0;
        m_hit_marker = false;
        m_end_of_band_run = 0;
        for (u32 i = 0; i < m_component_count; ++i)
            m_components[i].dc_predictor = 0;
        // Find the marker, wherever the bits stopped.
        while (m_scan_position + 1 < m_bytes.size()) {
            if (m_bytes[m_scan_position] == 0xff && m_bytes[m_scan_position + 1] >= 0xd0 && m_bytes[m_scan_position + 1] <= 0xd7) {
                m_scan_position += 2;
                return;
            }
            if (m_bytes[m_scan_position] == 0xff && m_bytes[m_scan_position + 1] != 0 && m_bytes[m_scan_position + 1] != 0xff)
                return; // Another marker: the scan's end.
            ++m_scan_position;
        }
    }

    ErrorOr<void> decode_baseline_block(Component& component, i16* block)
    {
        auto const& dc = m_dc_tables[component.dc_table];
        auto const& ac = m_ac_tables[component.ac_table];
        if (!dc.defined || !ac.defined)
            return corrupt();
        u8 size = TRY(decode_symbol(dc));
        if (size > 15)
            return corrupt();
        component.dc_predictor += receive_extend(size);
        block[0] = static_cast<i16>(component.dc_predictor);
        for (u32 k = 1; k < 64;) {
            u8 symbol = TRY(decode_symbol(ac));
            u32 run = symbol >> 4;
            u32 bits = symbol & 15;
            if (bits == 0) {
                if (run != 15)
                    break;
                k += 16;
                continue;
            }
            k += run;
            if (k > 63)
                return corrupt();
            block[zigzag[k]] = static_cast<i16>(receive_extend(bits));
            ++k;
        }
        return {};
    }

    ErrorOr<void> decode_progressive_block(Component& component, i16* block)
    {
        if (m_spectral_start == 0) {
            // DC: first scan or refinement.
            if (m_approximation_high == 0) {
                auto const& dc = m_dc_tables[component.dc_table];
                if (!dc.defined)
                    return corrupt();
                u8 size = TRY(decode_symbol(dc));
                if (size > 15)
                    return corrupt();
                component.dc_predictor += receive_extend(size);
                block[0] = static_cast<i16>(component.dc_predictor * (1 << m_approximation_low));
            } else if (read_bit()) {
                block[0] = static_cast<i16>(block[0] | (1 << m_approximation_low));
            }
            return {};
        }
        auto const& ac = m_ac_tables[component.ac_table];
        if (!ac.defined)
            return corrupt();
        if (m_approximation_high == 0) {
            // AC, first scan of this band.
            if (m_end_of_band_run > 0) {
                --m_end_of_band_run;
                return {};
            }
            for (u32 k = m_spectral_start; k <= m_spectral_end;) {
                u8 symbol = TRY(decode_symbol(ac));
                u32 run = symbol >> 4;
                u32 bits = symbol & 15;
                if (bits == 0) {
                    if (run < 15) {
                        m_end_of_band_run = (1u << run) - 1 + read_bits(run);
                        break;
                    }
                    k += 16;
                    continue;
                }
                k += run;
                if (k > 63)
                    return corrupt();
                block[zigzag[k]] = static_cast<i16>(receive_extend(bits) * (1 << m_approximation_low));
                ++k;
            }
            return {};
        }
        // AC, refinement: one more bit for what is already there, and new
        // coefficients among the zeros.
        i16 bit = static_cast<i16>(1 << m_approximation_low);
        auto refine = [&](i16& coefficient) {
            if (read_bit() && !(coefficient & bit))
                coefficient = static_cast<i16>(coefficient + (coefficient >= 0 ? bit : -bit));
        };
        u32 k = m_spectral_start;
        if (m_end_of_band_run == 0) {
            for (; k <= m_spectral_end; ++k) {
                u8 symbol = TRY(decode_symbol(ac));
                u32 run = symbol >> 4;
                u32 bits = symbol & 15;
                i16 value = 0;
                if (bits == 0) {
                    if (run < 15) {
                        m_end_of_band_run = (1u << run) + read_bits(run);
                        break;
                    }
                } else {
                    if (bits != 1)
                        return corrupt();
                    value = read_bit() ? bit : static_cast<i16>(-bit);
                }
                // Skip `run` zeros, refining what is passed on the way.
                for (; k <= m_spectral_end; ++k) {
                    auto& coefficient = block[zigzag[k]];
                    if (coefficient != 0) {
                        refine(coefficient);
                    } else {
                        if (run == 0) {
                            if (value)
                                coefficient = value;
                            break;
                        }
                        --run;
                    }
                }
            }
        }
        if (m_end_of_band_run > 0) {
            for (; k <= m_spectral_end; ++k) {
                auto& coefficient = block[zigzag[k]];
                if (coefficient != 0)
                    refine(coefficient);
            }
            --m_end_of_band_run;
        }
        return {};
    }

    ErrorOr<void> decode_block(Component& component, i16* block)
    {
        if (m_progressive)
            return decode_progressive_block(component, block);
        return decode_baseline_block(component, block);
    }

    // Returns where the markers go on, past the entropy-coded data.
    ErrorOr<size_t> read_scan(size_t at, size_t size)
    {
        if (!m_seen_frame || size < 1)
            return Error::from_string_literal("JPEG: a scan before the frame");
        u32 scan_components = m_bytes[at];
        if (scan_components < 1 || scan_components > 4 || size < 1 + scan_components * 2 + 3)
            return corrupt();
        Component* in_scan[4] = {};
        for (u32 i = 0; i < scan_components; ++i) {
            u8 id = m_bytes[at + 1 + i * 2];
            for (u32 c = 0; c < m_component_count; ++c) {
                if (m_components[c].id == id)
                    in_scan[i] = &m_components[c];
            }
            if (!in_scan[i])
                return corrupt();
            in_scan[i]->dc_table = m_bytes[at + 2 + i * 2] >> 4;
            in_scan[i]->ac_table = m_bytes[at + 2 + i * 2] & 15;
            if (in_scan[i]->dc_table > 3 || in_scan[i]->ac_table > 3)
                return corrupt();
        }
        m_spectral_start = m_bytes[at + 1 + scan_components * 2];
        m_spectral_end = m_bytes[at + 2 + scan_components * 2];
        m_approximation_high = m_bytes[at + 3 + scan_components * 2] >> 4;
        m_approximation_low = m_bytes[at + 3 + scan_components * 2] & 15;
        if (m_progressive) {
            if (m_spectral_end > 63 || m_spectral_start > m_spectral_end || m_approximation_low > 13 || (m_spectral_start == 0 && m_spectral_end != 0) || (m_spectral_start != 0 && scan_components != 1))
                return corrupt();
        }

        m_scan_position = at + size;
        m_bit_buffer = 0;
        m_bit_count = 0;
        m_hit_marker = false;
        m_end_of_band_run = 0;
        for (u32 c = 0; c < m_component_count; ++c)
            m_components[c].dc_predictor = 0;

        int until_restart = static_cast<int>(m_restart_interval);
        auto after_unit = [&] {
            if (m_restart_interval && --until_restart == 0) {
                restart();
                until_restart = static_cast<int>(m_restart_interval);
            }
        };
        if (scan_components == 1) {
            // Not interleaved: this component's own blocks, only those that
            // hold image.
            auto& component = *in_scan[0];
            int across = ((m_width * component.horizontal + static_cast<int>(m_max_horizontal) - 1) / static_cast<int>(m_max_horizontal) + 7) / 8;
            int down = ((m_height * component.vertical + static_cast<int>(m_max_vertical) - 1) / static_cast<int>(m_max_vertical) + 7) / 8;
            for (int y = 0; y < down; ++y) {
                for (int x = 0; x < across; ++x) {
                    TRY(decode_block(component, component.block(x, y)));
                    after_unit();
                }
            }
        } else {
            for (int mcu_y = 0; mcu_y < m_mcus_high; ++mcu_y) {
                for (int mcu_x = 0; mcu_x < m_mcus_wide; ++mcu_x) {
                    for (u32 i = 0; i < scan_components; ++i) {
                        auto& component = *in_scan[i];
                        for (int v = 0; v < component.vertical; ++v) {
                            for (int h = 0; h < component.horizontal; ++h)
                                TRY(decode_block(component, component.block(mcu_x * component.horizontal + h, mcu_y * component.vertical + v)));
                        }
                    }
                    after_unit();
                }
            }
        }
        return m_scan_position;
    }

    // --- Samples ---

    // The inverse DCT in floating point (Arai, Agui and Nakajima's factoring,
    // the input scaled to suit it).
    static void inverse_transform(float* block, u8* output, size_t pitch)
    {
        constexpr float sqrt2 = 1.414213562f;
        auto one_dimension = [](float* data, size_t stride) {
            float even0 = data[0 * stride], even1 = data[2 * stride], even2 = data[4 * stride], even3 = data[6 * stride];
            float sum02 = even0 + even2;
            float difference02 = even0 - even2;
            float sum13 = even1 + even3;
            float difference13 = (even1 - even3) * sqrt2 - sum13;
            float t0 = sum02 + sum13;
            float t3 = sum02 - sum13;
            float t1 = difference02 + difference13;
            float t2 = difference02 - difference13;

            float odd0 = data[1 * stride], odd1 = data[3 * stride], odd2 = data[5 * stride], odd3 = data[7 * stride];
            float z13 = odd2 + odd1;
            float z10 = odd2 - odd1;
            float z11 = odd0 + odd3;
            float z12 = odd0 - odd3;
            float t7 = z11 + z13;
            float rotated = (z11 - z13) * sqrt2;
            float z5 = (z10 + z12) * 1.847759065f;
            float part10 = 1.082392200f * z12 - z5;
            float part12 = -2.613125930f * z10 + z5;
            float t6 = part12 - t7;
            float t5 = rotated - t6;
            float t4 = part10 + t5;

            data[0 * stride] = t0 + t7;
            data[7 * stride] = t0 - t7;
            data[1 * stride] = t1 + t6;
            data[6 * stride] = t1 - t6;
            data[2 * stride] = t2 + t5;
            data[5 * stride] = t2 - t5;
            data[4 * stride] = t3 + t4;
            data[3 * stride] = t3 - t4;
        };
        for (size_t x = 0; x < 8; ++x)
            one_dimension(block + x, 8);
        for (size_t y = 0; y < 8; ++y)
            one_dimension(block + y * 8, 1);
        for (size_t y = 0; y < 8; ++y) {
            for (size_t x = 0; x < 8; ++x) {
                float value = block[y * 8 + x] / 8.0f + 128.0f + 0.5f;
                output[y * pitch + x] = static_cast<u8>(clamp(static_cast<int>(value), 0, 255));
            }
        }
    }

    ErrorOr<void> transform_component(Component& component)
    {
        static constexpr float scale[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f, 1.0f, 0.785694958f, 0.541196100f, 0.275899379f };
        size_t pitch = static_cast<size_t>(component.blocks_wide) * 8;
        TRY(component.samples.try_resize(pitch * static_cast<size_t>(component.blocks_high) * 8));
        auto const& table = m_quantization[component.quantization];
        float factors[64];
        for (u32 v = 0; v < 8; ++v) {
            for (u32 u = 0; u < 8; ++u)
                factors[v * 8 + u] = static_cast<float>(table[v * 8 + u]) * scale[u] * scale[v];
        }
        for (int y = 0; y < component.blocks_high; ++y) {
            for (int x = 0; x < component.blocks_wide; ++x) {
                auto const* coefficients = component.block(x, y);
                float block[64];
                for (u32 i = 0; i < 64; ++i)
                    block[i] = static_cast<float>(coefficients[i]) * factors[i];
                inverse_transform(block, component.samples.data() + static_cast<size_t>(y) * 8 * pitch + static_cast<size_t>(x) * 8, pitch);
            }
        }
        return {};
    }

    // A component's plane at full resolution. Halved chroma is interpolated
    // (each output between the two nearest samples, 3:1), as other decoders do.
    ErrorOr<Vector<u8>> upsampled(Component& component)
    {
        size_t width = static_cast<size_t>(m_width);
        size_t height = static_cast<size_t>(m_height);
        Vector<u8> plane;
        TRY(plane.try_resize(width * height));
        size_t pitch = static_cast<size_t>(component.blocks_wide) * 8;
        u32 scale_x = m_max_horizontal / component.horizontal;
        u32 scale_y = m_max_vertical / component.vertical;
        bool exact = m_max_horizontal % component.horizontal == 0 && m_max_vertical % component.vertical == 0;
        // Samples that hold image (the rest of the last block is padding).
        int valid_x = (m_width * component.horizontal + static_cast<int>(m_max_horizontal) - 1) / static_cast<int>(m_max_horizontal);
        int valid_y = (m_height * component.vertical + static_cast<int>(m_max_vertical) - 1) / static_cast<int>(m_max_vertical);
        auto const* samples = component.samples.data();
        auto sample = [&](int x, int y) {
            return static_cast<u32>(samples[static_cast<size_t>(clamp(y, 0, valid_y - 1)) * pitch + static_cast<size_t>(clamp(x, 0, valid_x - 1))]);
        };
        if (exact && scale_x == 1 && scale_y == 1) {
            for (size_t y = 0; y < height; ++y)
                __builtin_memcpy(plane.data() + y * width, samples + y * pitch, width);
        } else if (exact && scale_x <= 2 && scale_y <= 2) {
            for (size_t y = 0; y < height; ++y) {
                int near_y = static_cast<int>(y / scale_y);
                int far_y = scale_y == 2 ? near_y + ((y & 1) ? 1 : -1) : near_y;
                for (size_t x = 0; x < width; ++x) {
                    int near_x = static_cast<int>(x / scale_x);
                    int far_x = scale_x == 2 ? near_x + ((x & 1) ? 1 : -1) : near_x;
                    u32 near_mix = sample(near_x, near_y) * 3 + sample(far_x, near_y);
                    u32 far_mix = sample(near_x, far_y) * 3 + sample(far_x, far_y);
                    plane[y * width + x] = static_cast<u8>((near_mix * 3 + far_mix + 8) >> 4);
                }
            }
        } else {
            for (size_t y = 0; y < height; ++y) {
                int source_y = static_cast<int>(y * component.vertical / m_max_vertical);
                for (size_t x = 0; x < width; ++x)
                    plane[y * width + x] = static_cast<u8>(sample(static_cast<int>(x * component.horizontal / m_max_horizontal), source_y));
            }
        }
        return plane;
    }

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> assemble()
    {
        auto bitmap = TRY(Gfx::Bitmap::create({ m_width, m_height }));
        Vector<u8> planes[4];
        for (u32 i = 0; i < m_component_count; ++i)
            planes[i] = TRY(upsampled(m_components[i]));
        // YCbCr unless the file says RGB (Adobe's transform 0, or the ids R, G, B).
        bool is_rgb = m_component_count == 3 && ((m_has_adobe && m_adobe_transform == 0) || (!m_has_adobe && m_components[0].id == 'R' && m_components[1].id == 'G' && m_components[2].id == 'B'));
        bool is_ycck = m_component_count == 4 && m_has_adobe && m_adobe_transform == 2;
        auto limit = [](float value) { return static_cast<u8>(clamp(static_cast<int>(value + 0.5f), 0, 255)); };
        for (int y = 0; y < m_height; ++y) {
            auto* row = bitmap->scanline(y);
            size_t base = static_cast<size_t>(y) * static_cast<size_t>(m_width);
            for (int x = 0; x < m_width; ++x) {
                size_t at = base + static_cast<size_t>(x);
                u8 r, g, b;
                if (m_component_count == 1) {
                    r = g = b = planes[0][at];
                } else if (is_rgb) {
                    r = planes[0][at];
                    g = planes[1][at];
                    b = planes[2][at];
                } else {
                    float luma = planes[0][at];
                    float cb = static_cast<float>(planes[1][at]) - 128.0f;
                    float cr = static_cast<float>(planes[2][at]) - 128.0f;
                    r = limit(luma + 1.402f * cr);
                    g = limit(luma - 0.344136f * cb - 0.714136f * cr);
                    b = limit(luma + 1.772f * cb);
                    if (m_component_count == 4) {
                        // Adobe stores CMYK inverted: what is here is already
                        // 255 - C and so on; K scales them all.
                        u32 k = planes[3][at];
                        u8 c = is_ycck ? static_cast<u8>(255 - r) : planes[0][at];
                        u8 m = is_ycck ? static_cast<u8>(255 - g) : planes[1][at];
                        u8 ye = is_ycck ? static_cast<u8>(255 - b) : planes[2][at];
                        r = static_cast<u8>(Gfx::divide_by_255(c * k));
                        g = static_cast<u8>(Gfx::divide_by_255(m * k));
                        b = static_cast<u8>(Gfx::divide_by_255(ye * k));
                    }
                }
                row[x] = Gfx::make_pixel(r, g, b, 255);
            }
        }
        return bitmap;
    }

    ReadonlyBytes m_bytes;
    u16 m_quantization[4][64] {};
    HuffmanTable m_dc_tables[4];
    HuffmanTable m_ac_tables[4];
    Component m_components[4];
    u32 m_component_count { 0 };
    u32 m_max_horizontal { 1 };
    u32 m_max_vertical { 1 };
    int m_width { 0 };
    int m_height { 0 };
    int m_mcus_wide { 0 };
    int m_mcus_high { 0 };
    u32 m_restart_interval { 0 };
    bool m_progressive { false };
    bool m_seen_frame { false };
    bool m_seen_scan { false };
    bool m_has_adobe { false };
    u8 m_adobe_transform { 0 };

    u32 m_spectral_start { 0 };
    u32 m_spectral_end { 63 };
    u32 m_approximation_high { 0 };
    u32 m_approximation_low { 0 };
    u32 m_end_of_band_run { 0 };

    size_t m_scan_position { 0 };
    u32 m_bit_buffer { 0 };
    u32 m_bit_count { 0 };
    bool m_hit_marker { false };
};

}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode_jpeg(ReadonlyBytes bytes)
{
    auto* decoder = new (Base::nothrow) Decoder(bytes);
    if (!decoder)
        return Error::from_string_literal("out of memory");
    auto result = decoder->decode();
    delete decoder;
    return result;
}

}
