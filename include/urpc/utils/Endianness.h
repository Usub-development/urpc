//
// Created by root on 11/29/25.
//

#ifndef ENDIANNESS_H
#define ENDIANNESS_H

#include <bit>
#include <type_traits>

namespace urpc
{
    template <typename T>
    constexpr T host_to_be(T v)
    {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                      "host_to_be requires integral or enum type");

        if constexpr (sizeof(T) == 1)
        {
            return v;
        }
        else
        {
            if constexpr (std::endian::native == std::endian::little)
                return std::byteswap(v);
            else
                return v;
        }
    }

    template <typename T>
    constexpr T be_to_host(T v)
    {
        return host_to_be(v);
    }
}

#endif // ENDIANNESS_H