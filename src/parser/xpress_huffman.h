#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// MS-XCA section 2.2.4. The caller supplies the recorded uncompressed size.
bool decompress_xpress_huffman(std::string_view input, size_t expected_size, std::vector<uint8_t>& output,
                               std::string& error);
