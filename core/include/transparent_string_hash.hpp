#ifndef TRANSPARENT_STRING_HASH_HPP
#define TRANSPARENT_STRING_HASH_HPP

#include <string>
#include <string_view>
#include <functional>

/**
 * Transparent string hash/equal for heterogeneous lookup.
 * Enables unordered_set/map find() with std::string_view — zero-allocation
 * hot path lookups without constructing temporary std::string keys.
 *
 * Usage:
 *   std::unordered_map<std::string, V, TransparentStrHash, TransparentStrEq>
 *   std::unordered_set<std::string, TransparentStrHash, TransparentStrEq>
 *
 * Then cache.find(std::string_view(...)) works without allocation.
 */
struct TransparentStrHash {
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

struct TransparentStrEq {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

#endif // TRANSPARENT_STRING_HASH_HPP
