//
// Created by root on 11/4/25.
//

#ifndef WIRE_H
#define WIRE_H

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <span>
#include <array>

namespace urpc
{
    enum class MsgType : uint8_t { REQUEST = 0, RESPONSE = 1, ERROR = 2, CANCEL = 3 };

    enum : uint8_t
    {
        F_TP_RAW = 0b00,
        F_TP_TLS = 0b01,
        F_TP_MTLS = 0b10,
        F_TP_CUSTOM = 0b11,
    };

    inline uint8_t flags_set_transport(uint8_t f, uint8_t tp2bits)
    {
        return static_cast<uint8_t>((f & ~0b11u) | (tp2bits & 0b11u));
    }

    inline uint8_t flags_get_transport(uint8_t f) { return static_cast<uint8_t>(f & 0b11u); }

    [[nodiscard]] inline bool flags_has(uint8_t f, uint8_t mask) { return (f & mask) != 0; }

    inline void flags_set(uint8_t& f, uint8_t mask) { f = static_cast<uint8_t>(f | mask); }

    inline void flags_unset(uint8_t& f, uint8_t mask) { f = static_cast<uint8_t>(f & ~mask); }

    enum : uint8_t
    {
        F_COMPRESSED = 1u << 2,
        F_CB_PRESENT = 1u << 3,
        F_STREAM_LAST = 1u << 4,
    };

    inline constexpr uint32_t SETTINGS_STREAM = 0;
    inline constexpr uint64_t SETTINGS_METHOD = 0;

    [[nodiscard]] inline bool transport_matches(bool is_tls, bool is_mtls, uint8_t hdr_flags)
    {
        const uint8_t tp = flags_get_transport(hdr_flags);
        if (tp == F_TP_RAW) return !is_tls && !is_mtls;
        if (tp == F_TP_TLS) return is_tls && !is_mtls;
        if (tp == F_TP_MTLS) return is_tls && is_mtls;
        return true;
    }

#pragma pack(push,1)
    struct UrpcHdr
    {
        uint32_t len;
        uint8_t ver{1};
        uint8_t type{0};
        uint8_t flags{0};
        uint8_t rsv{0};
        uint32_t stream{0};
        uint64_t method{0};
        uint32_t meta_len{0};
        uint32_t body_len{0};
    };
#pragma pack(pop)

    inline void put_le32(std::string& o, uint32_t v)
    {
        std::array<char, 4> b{};
        for (int i = 0; i < 4; ++i) b[static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
        o.append(b.data(), b.data() + b.size());
    }

    inline void put_le64(std::string& o, uint64_t v)
    {
        std::array<char, 8> b{};
        for (int i = 0; i < 8; ++i) b[static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
        o.append(b.data(), b.data() + b.size());
    }

    inline uint32_t get_le32(const char* p)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i));
        return v;
    }

    inline uint64_t get_le64(const char* p)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i));
        return v;
    }

    inline constexpr size_t HDR_NO_LEN = 1 + 1 + 1 + 1 + 4 + 8 + 4 + 4;
    inline constexpr size_t HDR_SIZE = 4 + HDR_NO_LEN;

    inline void hdr_encode_into(std::string& out, const UrpcHdr& h)
    {
        out.reserve(out.size() + HDR_SIZE);
        put_le32(out, h.len);
        out.push_back(static_cast<char>(h.ver));
        out.push_back(static_cast<char>(h.type));
        out.push_back(static_cast<char>(h.flags));
        out.push_back(static_cast<char>(h.rsv));
        put_le32(out, h.stream);
        put_le64(out, h.method);
        put_le32(out, h.meta_len);
        put_le32(out, h.body_len);
    }

    inline std::string hdr_encode(const UrpcHdr& h)
    {
        std::string out;
        out.reserve(HDR_SIZE);
        hdr_encode_into(out, h);
        return out;
    }

    [[nodiscard]] inline std::optional<UrpcHdr>
    hdr_decode(std::span<const std::byte> buf)
    {
        const size_t n = buf.size();
        if (n < HDR_SIZE) return std::nullopt;

        auto ptr = [&](size_t idx) -> const char*
        {
            return reinterpret_cast<const char*>(buf.data() + idx);
        };

        size_t i = 0;
        UrpcHdr h{};

        h.len = get_le32(ptr(i));
        i += 4;
        h.ver = static_cast<uint8_t>(*(ptr(i) + 0));
        i += 1;
        h.type = static_cast<uint8_t>(*(ptr(i) + 0));
        i += 1;
        h.flags = static_cast<uint8_t>(*(ptr(i) + 0));
        i += 1;
        h.rsv = static_cast<uint8_t>(*(ptr(i) + 0));
        i += 1;
        h.stream = get_le32(ptr(i));
        i += 4;
        h.method = get_le64(ptr(i));
        i += 8;
        h.meta_len = get_le32(ptr(i));
        i += 4;
        h.body_len = get_le32(ptr(i));
        i += 4;

        const uint64_t expect = static_cast<uint64_t>(HDR_NO_LEN) + h.meta_len + h.body_len;
        if (h.len != expect) return std::nullopt;
        if (n < HDR_SIZE + h.meta_len + h.body_len) return std::nullopt;

        return h;
    }

    [[nodiscard]] inline std::optional<UrpcHdr>
    hdr_decode(const char* p, size_t n)
    {
        return hdr_decode(std::span<const std::byte>(reinterpret_cast<const std::byte*>(p), n));
    }

    struct ParsedFrame
    {
        UrpcHdr h{};
        std::string meta;
        std::string body;
    };

    inline std::string make_frame(UrpcHdr h, std::string&& meta, std::string&& body)
    {
        h.meta_len = static_cast<uint32_t>(meta.size());
        h.body_len = static_cast<uint32_t>(body.size());
        h.len = static_cast<uint32_t>(HDR_NO_LEN + h.meta_len + h.body_len);

        std::string out;
        out.reserve(HDR_SIZE + h.meta_len + h.body_len);
        hdr_encode_into(out, h);
        out += meta;
        out += body;
        return out;
    }

    inline bool parse_frame(const char* p, size_t n, ParsedFrame& pf)
    {
        const auto hopt = hdr_decode(p, n);
        if (!hopt) return false;

        pf.h = *hopt;
        const size_t meta_off = HDR_SIZE;
        const size_t body_off = HDR_SIZE + pf.h.meta_len;
        pf.meta.assign(p + meta_off, p + meta_off + pf.h.meta_len);
        pf.body.assign(p + body_off, p + body_off + pf.h.body_len);
        return true;
    }

    inline std::string make_settings_frame(bool use_tls, bool use_mtls, std::string&& meta_json)
    {
        UrpcHdr h{};
        h.type = static_cast<uint8_t>(MsgType::REQUEST);
        h.stream = SETTINGS_STREAM;
        h.method = SETTINGS_METHOD;

        const uint8_t tp = use_mtls ? F_TP_MTLS : (use_tls ? F_TP_TLS : F_TP_RAW);
        h.flags = flags_set_transport(h.flags, tp);
        if (!meta_json.empty()) h.flags |= F_CB_PRESENT;

        return make_frame(h, std::move(meta_json), {});
    }

    [[nodiscard]] inline bool validate_first_settings(const ParsedFrame& pf, bool is_tls, bool is_mtls)
    {
        if (pf.h.stream != SETTINGS_STREAM || pf.h.method != SETTINGS_METHOD)
            return false;
        return transport_matches(is_tls, is_mtls, pf.h.flags);
    }

    [[nodiscard]] inline uint8_t derive_transport_flags(bool use_tls, bool use_mtls, uint8_t base = 0)
    {
        const uint8_t tp = use_mtls ? F_TP_MTLS : (use_tls ? F_TP_TLS : F_TP_RAW);
        return flags_set_transport(base, tp);
    }
}

#endif // WIRE_H
