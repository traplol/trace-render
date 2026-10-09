#include "xpress_huffman.h"
#include <algorithm>
#include <array>
#include <stdexcept>

// Independently implemented from Microsoft's MS-XCA 2.2.4 specification:
// https://learn.microsoft.com/openspecs/windows_protocols/ms-xca/26db8e62-bbd8-472c-a09e-623f6de10f0b
bool decompress_xpress_huffman(std::string_view input, size_t expected_size, std::vector<uint8_t>& output,
                               std::string& error) {
    output.clear();
    error.clear();
    try {
        if (expected_size > (size_t(1) << 31)) throw std::runtime_error("output exceeds 2 GiB limit");
        size_t position = 0;
        auto byte = [&]() -> uint8_t {
            if (position == input.size()) throw std::runtime_error("truncated compressed data");
            return static_cast<uint8_t>(input[position++]);
        };
        auto word = [&]() -> uint32_t {
            uint32_t low = byte();
            return low | (uint32_t(byte()) << 8);
        };
        output.reserve(expected_size);
        while (output.size() < expected_size) {
            std::array<uint8_t, 512> lengths{};
            for (size_t i = 0; i < 256; ++i) {
                uint8_t value = byte();
                lengths[2 * i] = value & 15;
                lengths[2 * i + 1] = value >> 4;
            }
            std::array<uint16_t, 32768> table{};
            size_t entry = 0;
            for (unsigned length = 1; length <= 15; ++length) {
                for (unsigned symbol = 0; symbol < 512; ++symbol) {
                    if (lengths[symbol] != length) continue;
                    size_t count = size_t(1) << (15 - length);
                    if (count > table.size() - entry) throw std::runtime_error("oversubscribed Huffman table");
                    std::fill_n(table.begin() + entry, count, static_cast<uint16_t>(symbol));
                    entry += count;
                }
            }
            if (entry != table.size()) throw std::runtime_error("incomplete Huffman table");
            uint32_t bits = word() << 16;
            bits |= word();
            int extra = 16;
            auto consume = [&](unsigned count) {
                bits <<= count;
                extra -= static_cast<int>(count);
                if (extra < 0) {
                    bits |= word() << -extra;
                    extra += 16;
                }
            };
            size_t block_end = std::min(expected_size, output.size() + 65536);
            while (output.size() < block_end) {
                unsigned symbol = table[bits >> 17];
                consume(lengths[symbol]);
                if (symbol < 256) {
                    output.push_back(static_cast<uint8_t>(symbol));
                    continue;
                }
                symbol -= 256;
                size_t length = symbol & 15;
                unsigned offset_bits = symbol >> 4;
                if (length == 15) {
                    length = byte();
                    if (length == 255) {
                        length = word();
                        if (length < 15) throw std::runtime_error("invalid extended match length");
                        length -= 15;
                    }
                    length += 15;
                }
                length += 3;
                size_t offset = size_t(1) << offset_bits;
                if (offset_bits) offset += bits >> (32 - offset_bits);
                consume(offset_bits);
                if (offset > output.size() || length > expected_size - output.size())
                    throw std::runtime_error("match outside decompressed buffer");
                for (size_t i = 0; i < length; ++i) output.push_back(output[output.size() - offset]);
            }
        }
        return true;
    } catch (const std::exception& exception) {
        output.clear();
        error = std::string("Invalid XPRESS-Huffman data: ") + exception.what();
        return false;
    }
}
