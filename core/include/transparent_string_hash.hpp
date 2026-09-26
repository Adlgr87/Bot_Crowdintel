#ifndef TRANSPARENT_STRING_HASH_HPP
#define TRANSPARENT_STRING_HASH_HPP

#include <string_view>
#include <string>
#include <functional>
#include <cstddef>

/**
 * Transparent string hash functor for std::unordered_map.
 *
 * Allows O(1) lookup with std::string_view without requiring a std::string
 * temporary. Used by OrderManager's secondary index for self-trade detection.
 *
 * Usage:
 *   std::unordered_map<std::string, size_t, StringHash, StringEqual> index;
 *   index.find("BTC-USD-UP"sv);  // No std::string allocation
 *
 * This is a standard transparent hash implementation using:
 * - std::hash<std::string_view> for hashing (C++11+ standard)
 * - std::equal_to<> for equality (transparent)
 *
 * Both std::hash<std::string_view> and std::equal_to<> are transparent in
 * C++20, allowing heterogeneous lookup with std::string_view.
 */
struct StringHash {
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }

    size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

#endif // TRANSPARENT_STRING_HASH_HPP
