//
// Created by root on 11/29/25.
//

#ifndef HASH_H
#define HASH_H

#include <string_view>
#include <cstdint>

namespace urpc
{
    constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
    constexpr uint64_t FNV_PRIME = 1099511628211ull;

    constexpr uint64_t fnv1a64_rt(std::string_view s) noexcept
    {
        uint64_t h = FNV_OFFSET;
        for (unsigned char c : s)
        {
            h ^= c;
            h *= FNV_PRIME;
        }
        return h;
    }

    template <size_t N>
    consteval uint64_t fnv1a64_ct(const char (&str)[N])
    {
        uint64_t h = FNV_OFFSET;
        for (size_t i = 0; i + 1 < N; ++i)
        {
            h ^= static_cast<unsigned char>(str[i]);
            h *= FNV_PRIME;
        }
        return h;
    }

    template <size_t N>
    consteval uint64_t method_id(const char (&str)[N])
    {
        return fnv1a64_ct(str);
    }
}

#endif //HASH_H
