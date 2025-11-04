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
#include <span>
#include <bit>
#include <cstdint>
#include <cstring>
#include "ureflect/ureflect_auto.h"

namespace urpc
{
    // ============================
    // Buffer (safe, span-friendly)
    // ============================
    struct Buf
    {
        const char* p{};
        size_t n{};
        size_t i{};

        Buf() = default;
        Buf(const char* data, size_t size) : p(data), n(size), i(0) {}
        explicit Buf(std::string_view sv) : p(sv.data()), n(sv.size()), i(0) {}
        explicit Buf(std::span<const std::byte> s)
            : p(reinterpret_cast<const char*>(s.data())), n(s.size()), i(0) {}

        [[nodiscard]] size_t remaining() const noexcept { return (i <= n) ? (n - i) : 0; }

        bool get(uint8_t& b)
        {
            if (i >= n) return false;
            b = static_cast<uint8_t>(p[i++]);
            return true;
        }

        bool read_bytes(size_t len, const char*& out_begin)
        {
            if (remaining() < len) return false;
            out_begin = p + i;
            i += len;
            return true;
        }

        bool skip(size_t len)
        {
            if (remaining() < len) return false;
            i += len;
            return true;
        }
    };

    // ==============
    // Varint (LEB128)
    // ==============
    inline void put_varu(std::string& o, uint64_t v)
    {
        while (v >= 0x80)
        {
            o.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        o.push_back(static_cast<char>(v));
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
            if ((by & 0x80) == 0) break;
            sh += 7;
            if (sh > 63) return false;
        }
        return true;
    }

    inline void put_vars(std::string& o, int64_t s)
    {
        // ZigZag
        const uint64_t u = (uint64_t(s) << 1) ^ uint64_t(s >> 63);
        put_varu(o, u);
    }

    inline bool get_vars(Buf& b, int64_t& s)
    {
        uint64_t u{};
        if (!get_varu(b, u)) return false;
        s = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
        return true;
    }

    inline void put_bytes(std::string& o, std::string_view sv)
    {
        put_varu(o, sv.size());
        o.append(sv.data(), sv.size());
    }

    inline bool get_bytes(Buf& b, std::string& out)
    {
        uint64_t len{};
        if (!get_varu(b, len)) return false;
        if (len > b.remaining()) return false;
        out.assign(b.p + b.i, b.p + b.i + static_cast<size_t>(len));
        b.i += static_cast<size_t>(len);
        return true;
    }

    // ==========
    // FNV-1a 64
    // ==========
    constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;

    inline uint64_t fnv1a64_fast(const uint8_t* p, size_t n)
    {
        uint64_t h = FNV_OFFSET;
        while (n >= 8)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            h ^= static_cast<uint8_t>(v >> 0);  h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 8);  h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 16); h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 24); h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 32); h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 40); h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 48); h *= FNV_PRIME;
            h ^= static_cast<uint8_t>(v >> 56); h *= FNV_PRIME;
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

    // ==================
    // Primitive codecs
    // ==================
    template <class T>
    inline void enc_prim(std::string& o, const T& v)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            o.push_back(v ? char(1) : char(0));
        }
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        {
            put_vars(o, static_cast<int64_t>(v));
        }
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
        {
            put_varu(o, static_cast<uint64_t>(v));
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            const auto bits = std::bit_cast<std::array<char,4>>(v);
            o.append(bits.data(), bits.size());
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            const auto bits = std::bit_cast<std::array<char,8>>(v);
            o.append(bits.data(), bits.size());
        }
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
        {
            put_bytes(o, std::string_view(v));
        }
        else
        {
            static_assert(!sizeof(T*), "enc_prim unsupported type");
        }
    }

    template <class T>
    inline bool dec_prim(Buf& b, T& v)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            uint8_t c{};
            if (!b.get(c)) return false;
            v = (c != 0);
            return true;
        }
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        {
            int64_t s{};
            if (!get_vars(b, s)) return false;
            v = static_cast<T>(s);
            return true;
        }
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
        {
            uint64_t u{};
            if (!get_varu(b, u)) return false;
            v = static_cast<T>(u);
            return true;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            if (b.remaining() < 4) return false;
            float tmp{};
            std::memcpy(&tmp, b.p + b.i, 4);
            b.i += 4;
            v = tmp;
            return true;
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            if (b.remaining() < 8) return false;
            double tmp{};
            std::memcpy(&tmp, b.p + b.i, 8);
            b.i += 8;
            v = tmp;
            return true;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return get_bytes(b, v);
        }
        else
        {
            static_assert(!sizeof(T*), "dec_prim unsupported type");
        }
    }

    // ===========
    // Forwarding
    // ===========
    template <class T> inline void encode(std::string& o, const T& v);
    template <class T> inline bool decode(Buf& b, T& v);

    // ================
    // std::vector<T>
    // ================
    template <class T>
    inline void encode(std::string& o, const std::vector<T>& v)
    {
        put_varu(o, v.size());
        for (const auto& e : v) encode(o, e);
    }

    template <class T>
    inline bool decode(Buf& b, std::vector<T>& v)
    {
        uint64_t cnt{};
        if (!get_varu(b, cnt)) return false;
        v.resize(static_cast<size_t>(cnt));
        for (auto& e : v)
            if (!decode(b, e)) return false;
        return true;
    }

    // ===============
    // std::optional<T>
    // ===============
    template <class T>
    inline void encode(std::string& o, const std::optional<T>& v)
    {
        o.push_back(v ? char(1) : char(0));
        if (v) encode(o, *v);
    }

    template <class T>
    inline bool decode(Buf& b, std::optional<T>& v)
    {
        uint8_t c{};
        if (!b.get(c)) return false;
        if (!c) { v.reset(); return true; }
        T tmp{};
        if (!decode(b, tmp)) return false;
        v = std::move(tmp);
        return true;
    }

    // ======
    // enums
    // ======
    template <class E> concept Enum = std::is_enum_v[E];

    template <Enum E>
    inline void encode(std::string& o, const E& e)
    {
        using U = std::underlying_type_t<E>;
        enc_prim(o, static_cast<U>(e));
    }

    template <Enum E>
    inline bool decode(Buf& b, E& e)
    {
        using U = std::underlying_type_t<E>;
        U u{};
        if (!dec_prim(b, u)) return false;
        e = static_cast<E>(u);
        return true;
    }

    // ==========================
    // std::array<T, N> (fixed)
    // ==========================
    template <class T, size_t N>
    inline void encode(std::string& o, const std::array<T, N>& a)
    {
        for (const auto& e : a) encode(o, e);
    }

    template <class T, size_t N>
    inline bool decode(Buf& b, std::array<T, N>& a)
    {
        for (auto& e : a)
            if (!decode(b, e)) return false;
        return true;
    }

    // ==========================
    // std::tuple<Ts...> (opt)
    // ==========================
    template <class... Ts>
    inline void encode(std::string& o, const std::tuple<Ts...>& tp)
    {
        std::apply([&](const Ts&... xs){ (encode(o, xs), ...); }, tp);
    }

    template <class... Ts>
    inline bool decode(Buf& b, std::tuple<Ts...>& tp)
    {
        bool ok = true;
        std::apply([&](Ts&... xs){
            ((ok = ok && decode(b, xs)), ...);
        }, tp);
        return ok;
    }

    // ================================
    // Generic object via ureflect::tie
    // ================================
    template <class T>
    inline void encode(std::string& o, const T& obj)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
        {
            enc_prim(o, obj);
        }
        else if constexpr (Enum<T>)
        {
            encode(o, static_cast<T>(obj));
        }
        else
        {
            auto tie = ureflect::to_tie(const_cast<T&>(obj));
            constexpr auto N = std::tuple_size_v<decltype(tie)>;
            [&]<size_t... I>(std::index_sequence<I...>){
                (encode(o, std::get<I>(tie)), ...);
            }(std::make_index_sequence<N>{});
        }
    }

    template <class T>
    inline bool decode(Buf& b, T& obj)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
        {
            return dec_prim(b, obj);
        }
        else if constexpr (Enum<T>)
        {
            return decode(b, reinterpret_cast<std::underlying_type_t<T>&>(obj));
        }
        else
        {
            auto tie = ureflect::to_tie(obj);
            constexpr auto N = std::tuple_size_v<decltype(tie)>;
            bool ok = true;
            [&]<size_t... I>(std::index_sequence<I...>){
                ((ok = ok && decode(b, std::get<I>(tie))), ...);
            }(std::make_index_sequence<N>{});
            return ok;
        }
    }

} // namespace urpc

#endif // CODEC_H
