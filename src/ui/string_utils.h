#pragma once
#include <algorithm>
#include <cctype>
#include <string>

// Case-insensitive substring search.
inline bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    });
    return it != haystack.end();
}
