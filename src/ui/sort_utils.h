#pragma once

namespace sort_utils {

// Three-way comparison helper for use in ImGui table sort lambdas.
// Returns -1, 0, or 1.
template <typename T>
inline int three_way_cmp(const T& a, const T& b) {
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

}  // namespace sort_utils
