//
// Created by root on 11/4/25.
//

#ifndef CODEC_H
#define CODEC_H

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <type_traits>
#include <tuple>
#include <array>
#include <cstdint>
#include <cstring>
#include "ureflect/ureflect_auto.h"


namespace urpcv1
{
    struct Buf
    {
        const char* p{};
        size_t n{};
        size_t i{};

        bool get(uint8_t& b)
        {
            if (i >= n) return false;
            b = (uint8_t)p[i++];
            return true;
        }
    };


    inline void put_varu(std::string& o, uint64_t v)
    {
        while (v >= 0x80)
        {
            o.push_back(char((v & 0x7F) | 0x80));
            v >>= 7;
        }
        o.push_back(char(v));
    }

    inline bool get_varu(Buf& b, uint64_t& v)
    {
        v = 0;
        int sh = 0;
        uint8_t by;
        for (;;)
        {
            if (!b.get(by)) return false;
            v |= (uint64_t(by & 0x7F) << sh);
            if (!(by & 0x80)) break;
            sh += 7;
            if (sh > 63) return false;
        }
        return true;
    }

    inline void put_vars(std::string& o, int64_t s)
    {
        uint64_t u = (uint64_t(s) << 1) ^ uint64_t(s >> 63);
        put_varu(o, u);
    }

    inline bool get_vars(Buf& b, int64_t& s)
    {
        uint64_t u;
        if (!get_varu(b, u)) return false;
        s = int64_t((u >> 1) ^ (~(u & 1) + 1));
        return true;
    }

    inline void put_bytes(std::string& o, std::string_view sv)
    {
        put_varu(o, sv.size());
        o.append(sv);
    }

    inline bool get_bytes(Buf& b, std::string& out)
    {
        uint64_t n;
        if (!get_varu(b, n)) return false;
        if (n > (b.n - b.i)) return false;
        out.assign(b.p + b.i, b.p + b.i + n);
        b.i += size_t(n);
        return true;
    }


    constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
    constexpr uint64_t FNV_PRIME = 1099511628211ull;

    inline uint64_t fnv1a64_fast(const uint8_t* p, size_t n)
    {
        uint64_t h = FNV_OFFSET;

        while (n >= 8)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            h ^= (uint8_t)(v);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 8);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 16);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 24);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 32);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 40);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 48);
            h *= FNV_PRIME;
            h ^= (uint8_t)(v >> 56);
            h *= FNV_PRIME;
            p += 8;
            n -= 8;
        }
        while (n--)
        {
            h ^= *p++;
            h *= FNV_PRIME;
        }
        return h;
    }

    inline uint64_t fnv1a64_fast(std::string_view s)
    {
        return fnv1a64_fast(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }


    // primitives


    template <class T>
    inline void enc_prim(std::string& o, const T& v)
    {
        if constexpr (std::is_same_v<T, bool>) o.push_back(v ? 1 : 0);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) put_vars(o, (int64_t)v);
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) put_varu(o, (uint64_t)v);
        else if constexpr (std::is_same_v<T, float>)
        {
            const char* p = (const char*)&v;
            o.append(p, p + 4);
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            const char* p = (const char*)&v;
            o.append(p, p + 8);
        }
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
            put_bytes(
                o, std::string_view(v));
        else static_assert(!sizeof(T*), "enc_prim unsupported");
    }


    template <class T>
    inline bool dec_prim(Buf& b, T& v)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            uint8_t c;
            if (!b.get(c)) return false;
            v = (c != 0);
            return true;
        }
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        {
            int64_t s;
            if (!get_vars(b, s)) return false;
            v = (T)s;
            return true;
        }
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
        {
            uint64_t u;
            if (!get_varu(b, u)) return false;
            v = (T)u;
            return true;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            if (b.n - b.i < 4) return false;
            std::memcpy(&v, b.p + b.i, 4);
            b.i += 4;
            return true;
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            if (b.n - b.i < 8) return false;
            std::memcpy(&v, b.p + b.i, 8);
            b.i += 8;
            return true;
        }
        else if constexpr (std::is_same_v<T, std::string>) { return get_bytes(b, v); }
        else static_assert(!sizeof(T*), "dec_prim unsupported");
    }


    // forward
    template <class T>
    inline void encode(std::string& o, const T& v);
    template <class T>
    inline bool decode(Buf& b, T& v);


    template <class T>
    inline void encode(std::string& o, const std::vector<T>& v)
    {
        put_varu(o, v.size());
        for (auto& e : v) encode(o, e);
    }


    template <class T>
    inline bool decode(Buf& b, std::vector<T>& v)
    {
        uint64_t n;
        if (!get_varu(b, n)) return false;
        v.resize((size_t)n);
        for (auto& e : v) if (!decode(b, e)) return false;
        return true;
    }


    template <class T>
    inline void encode(std::string& o, const std::optional<T>& v)
    {
        o.push_back(v ? 1 : 0);
        if (v) encode(o, *v);
    }


    template <class T>
    inline bool decode(Buf& b, std::optional<T>& v)
    {
        uint8_t c;
        if (!b.get(c)) return false;
        if (!c)
        {
            v.reset();
            return true;
        }
        T tmp{};
        if (!decode(b, tmp)) return false;
        v = std::move(tmp);
        return true;
    }


    // enums


    template <class E> concept Enum = std::is_enum_v<E>;


    template <Enum E>
    inline void encode(std::string& o, const E& e)
    {
        using U = std::underlying_type_t<E>;
        enc_prim(o, (U)e);
    }


    template <Enum E>
    inline bool decode(Buf& b, E& e)
    {
        using U = std::underlying_type_t<E>;
        U u{};
        if (!dec_prim(b, u)) return false;
        e = (E)u;
        return true;
    }


    // aggregates via ureflect


    template <class T>
    inline void encode(std::string& o, const T& obj)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>) enc_prim(o, obj);
        else if constexpr (Enum<T>) encode(o, (T)obj);
        else
        {
            auto tie = ureflect::to_tie(const_cast<T&>(obj));
            constexpr auto N = std::tuple_size_v<decltype(tie)>;
            [&]<size_t... I>(std::index_sequence<I...>) { (encode(o, std::get<I>(tie)), ...); }(
                std::make_index_sequence<N>{});
        }
    }


    template <class T>
    inline bool decode(Buf& b, T& obj)
    {
    } // namespace urpcv1
}

#endif //CODEC_H
