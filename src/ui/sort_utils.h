#pragma once

// Three-way comparison helper for use in ImGui table sort lambdas.
// Returns -1, 0, or 1.
template <typename T>
inline int compare(const T& a, const T& b) {
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}
