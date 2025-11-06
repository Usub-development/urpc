#ifndef URPC_WIRE_H
#define URPC_WIRE_H

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <span>
#include <array>
#include <variant>
#include <type_traits>
#include <cstring>

namespace urpc
{
    // ===== protocol ids / versions =====
    inline constexpr uint16_t SPEC_MAGIC = 0x5552; // 'UR'
    inline constexpr uint8_t SPEC_VER = 1;

    // ===== limits (frames/sections/varint) =====
    inline constexpr uint32_t MAX_FRAME_NO_LEN = 16 * 1024 * 1024;
    inline constexpr int MAX_VARINT_SHIFT = 63;

    // ===== message types =====
    enum class MsgType : uint8_t
    {
        REQUEST = 0, RESPONSE = 1, ERROR = 2, CANCEL = 3,
        PING = 4, PONG = 5, GOAWAY = 6
    };

    // ===== transport bits =====
    enum : uint8_t
    {
        F_TP_RAW = 0b00,
        F_TP_TLS = 0b01,
        F_TP_MTLS = 0b10,
        F_TP_CUSTOM = 0b11,
    };

    enum class TransportMode : uint8_t { RAW = 0, TLS = 1, MTLS = 2, CUSTOM = 3 };

    inline uint8_t flags_set_transport(uint8_t f, uint8_t tp2bits)
    {
        return static_cast<uint8_t>((f & ~0b11u) | (tp2bits & 0b11u));
    }

    inline uint8_t flags_get_transport(uint8_t f) { return static_cast<uint8_t>(f & 0b11u); }

    [[nodiscard]] inline bool flags_has(uint8_t f, uint8_t mask) { return (f & mask) != 0; }
    inline void flags_set(uint8_t& f, uint8_t mask) { f = static_cast<uint8_t>(f | mask); }
    inline void flags_unset(uint8_t& f, uint8_t mask) { f = static_cast<uint8_t>(f & ~mask); }

    // ===== frame flags =====
    enum : uint8_t
    {
        F_COMPRESSED = 1u << 2,
        F_CB_PRESENT = 1u << 3,
        F_STREAM_LAST = 1u << 4,
        F_FLOW_CREDIT = 1u << 5,
        F_GOAWAY = 1u << 6
    };

    inline bool is_stream_last(uint8_t flags) { return flags_has(flags, F_STREAM_LAST); }
    inline bool is_credit_frame(uint8_t flags) { return flags_has(flags, F_FLOW_CREDIT); }

    // ===== codec/compression kinds =====
    enum class CodecKind : uint8_t { RAW = 0, JSON = 1 };

    enum class CompressionKind : uint8_t { NONE = 0, GZIP = 1, ZSTD = 2 };

    // ===== special streams/methods =====
    inline constexpr uint32_t SETTINGS_STREAM = 0;
    inline constexpr uint64_t SETTINGS_METHOD = 0;

    // ===== helpers (LE io) =====
    inline void put_le32(std::string& o, uint32_t v)
    {
        std::array<char, 4> b{};
        for (int i = 0; i < 4; ++i) b[size_t(i)] = char((v >> (8 * i)) & 0xFF);
        o.append(b.data(), b.size());
    }

    inline void put_le64(std::string& o, uint64_t v)
    {
        std::array<char, 8> b{};
        for (int i = 0; i < 8; ++i) b[size_t(i)] = char((v >> (8 * i)) & 0xFF);
        o.append(b.data(), b.size());
    }

    inline uint32_t get_le32(const char* p)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t(uint8_t(p[i])) << (8 * i));
        return v;
    }

    inline uint64_t get_le64(const char* p)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t(uint8_t(p[i])) << (8 * i));
        return v;
    }

    // ===== header  =====
#pragma pack(push,1)
    struct UrpcHdr
    {
        uint32_t len; // total of [hdr-without-len + meta + body]
        uint8_t ver{SPEC_VER};
        uint8_t type{0};
        uint8_t flags{0};
        uint8_t rsv{0};
        uint32_t stream{0};
        uint64_t method{0};

        // options
        uint32_t timeout_ms{0};
        uint64_t cancel_id{0};
        uint8_t codec{uint8_t(CodecKind::RAW)};
        uint8_t comp{uint8_t(CompressionKind::NONE)};
        uint16_t spec{SPEC_MAGIC}; // magic

        // payload sizes
        uint32_t meta_len{0};
        uint32_t body_len{0};
    };
#pragma pack(pop)

    inline constexpr size_t WIRE_HDR_SIZE = 44;
    inline constexpr size_t HDR_NO_LEN = WIRE_HDR_SIZE - 4;
    inline constexpr size_t HDR_SIZE = WIRE_HDR_SIZE;

    enum class StatusCode : uint16_t
    {
        OK = 0, CANCELLED = 1, UNKNOWN = 2, INVALID_ARGUMENT = 3, DEADLINE_EXCEEDED = 4,
        NOT_FOUND = 5, ALREADY_EXISTS = 6, PERMISSION_DENIED = 7, RESOURCE_EXHAUSTED = 8,
        FAILED_PRECONDITION = 9, ABORTED = 10, OUT_OF_RANGE = 11, UNIMPLEMENTED = 12,
        INTERNAL = 13, UNAVAILABLE = 14, DATA_LOSS = 15, UNAUTHENTICATED = 16,
        PROTOCOL_ERROR = 1000, TRANSPORT_ERROR = 1001, WRONG_BODY = 1002
    };

    struct RpcError
    {
        StatusCode code{};
        std::string message;
    };

    template <class T>
    using RpcExpected = std::variant<T, RpcError>;

    // ===== frame =====
    struct ParsedFrame
    {
        UrpcHdr h{};
        std::string meta;
        std::string body;
    };

    inline size_t required_size(size_t meta, size_t body)
    {
        return WIRE_HDR_SIZE + meta + body;
    }

    // ===== settings/helpers =====
    inline uint8_t transport_bits_of(TransportMode m)
    {
        switch (m)
        {
        case TransportMode::RAW: return F_TP_RAW;
        case TransportMode::TLS: return F_TP_TLS;
        case TransportMode::MTLS: return F_TP_MTLS;
        case TransportMode::CUSTOM: return F_TP_CUSTOM;
        }
        return F_TP_RAW;
    }

    [[nodiscard]] inline uint8_t derive_transport_flags(TransportMode m, uint8_t base = 0)
    {
        return flags_set_transport(base, transport_bits_of(m));
    }

    [[nodiscard]] inline UrpcHdr make_settings_hdr(TransportMode m)
    {
        UrpcHdr h{};
        h.type = uint8_t(MsgType::REQUEST);
        h.stream = SETTINGS_STREAM;
        h.method = SETTINGS_METHOD;
        h.flags = derive_transport_flags(m);
        return h;
    }

    [[nodiscard]] inline bool transport_matches(bool is_tls, bool is_mtls, uint8_t hdr_flags)
    {
        const uint8_t tp = flags_get_transport(hdr_flags);
        if (tp == F_TP_RAW) return !is_tls && !is_mtls;
        if (tp == F_TP_TLS) return is_tls && !is_mtls;
        if (tp == F_TP_MTLS) return is_tls && is_mtls;
        return true;
    }

    [[nodiscard]] inline bool validate_first_settings(const ParsedFrame& pf, bool is_tls, bool is_mtls)
    {
        if (pf.h.stream != SETTINGS_STREAM || pf.h.method != SETTINGS_METHOD) return false;
        return transport_matches(is_tls, is_mtls, pf.h.flags);
    }

    inline UrpcHdr make_ping(uint32_t stream = 0)
    {
        UrpcHdr h{};
        h.type = uint8_t(MsgType::PING);
        h.stream = stream;
        return h;
    }

    inline UrpcHdr make_pong(const UrpcHdr& ping)
    {
        UrpcHdr h{};
        h.type = uint8_t(MsgType::PONG);
        h.stream = ping.stream;
        h.flags = ping.flags;
        return h;
    }

    // ===== encode/decode header & frame =====
    inline void hdr_encode_into(std::string& out, const UrpcHdr& h)
    {
        out.reserve(out.size() + WIRE_HDR_SIZE);
        put_le32(out, h.len);
        out.push_back(char(h.ver));
        out.push_back(char(h.type));
        out.push_back(char(h.flags));
        out.push_back(char(h.rsv));
        put_le32(out, h.stream);
        put_le64(out, h.method);
        put_le32(out, h.timeout_ms);
        put_le64(out, h.cancel_id);
        out.push_back(char(h.codec));
        out.push_back(char(h.comp));
        out.push_back(char(h.spec & 0xFF));
        out.push_back(char((h.spec >> 8) & 0xFF));
        put_le32(out, h.meta_len);
        put_le32(out, h.body_len);
    }

    inline std::string make_frame(UrpcHdr h, std::string&& meta, std::string&& body)
    {
        h.meta_len = static_cast<uint32_t>(meta.size());
        h.body_len = static_cast<uint32_t>(body.size());
        h.len = static_cast<uint32_t>(HDR_NO_LEN + h.meta_len + h.body_len);

        std::string out;
        out.reserve(WIRE_HDR_SIZE + h.meta_len + h.body_len);
        hdr_encode_into(out, h);
        out += meta;
        out += body;
        return out;
    }

    inline bool parse_frame(const char* p, size_t n, ParsedFrame& pf)
    {
        if (n < WIRE_HDR_SIZE) return false;

        size_t i = 0;
        UrpcHdr h{};
        h.len = get_le32(p + i);
        i += 4;
        h.ver = uint8_t(p[i++]);
        h.type = uint8_t(p[i++]);
        h.flags = uint8_t(p[i++]);
        h.rsv = uint8_t(p[i++]);
        h.stream = get_le32(p + i);
        i += 4;
        h.method = get_le64(p + i);
        i += 8;
        h.timeout_ms = get_le32(p + i);
        i += 4;
        h.cancel_id = get_le64(p + i);
        i += 8;
        h.codec = uint8_t(p[i++]);
        h.comp = uint8_t(p[i++]);
        h.spec = uint16_t(uint8_t(p[i]) | (uint16_t(uint8_t(p[i + 1])) << 8));
        i += 2;
        h.meta_len = get_le32(p + i);
        i += 4;
        h.body_len = get_le32(p + i);
        i += 4;

        const uint64_t expect_len = uint64_t(HDR_NO_LEN) + h.meta_len + h.body_len;
        if (h.len != expect_len || h.len < HDR_NO_LEN) return false;
        if (h.ver != SPEC_VER || h.spec != SPEC_MAGIC) return false;
        if (n < WIRE_HDR_SIZE + h.meta_len + h.body_len) return false;

        pf.h = h;
        const size_t meta_off = WIRE_HDR_SIZE;
        const size_t body_off = WIRE_HDR_SIZE + pf.h.meta_len;
        pf.meta.assign(p + meta_off, p + meta_off + pf.h.meta_len);
        pf.body.assign(p + body_off, p + body_off + pf.h.body_len);
        return true;
    }

    inline RpcExpected<UrpcHdr> decode_header(std::span<const std::byte> s)
    {
        if (s.size() < WIRE_HDR_SIZE)
            return RpcError{StatusCode::WRONG_BODY, "Body is too small"};

        const uint8_t* u = reinterpret_cast<const uint8_t*>(s.data());
        size_t i = 0;

        UrpcHdr h{};
        h.len = get_le32(reinterpret_cast<const char*>(u + i));
        i += 4;
        h.ver = u[i++];
        h.type = u[i++];
        h.flags = u[i++];
        h.rsv = u[i++];
        h.stream = get_le32(reinterpret_cast<const char*>(u + i));
        i += 4;
        h.method = get_le64(reinterpret_cast<const char*>(u + i));
        i += 8;
        h.timeout_ms = get_le32(reinterpret_cast<const char*>(u + i));
        i += 4;
        h.cancel_id = get_le64(reinterpret_cast<const char*>(u + i));
        i += 8;
        h.codec = u[i++];
        h.comp = u[i++];
        h.spec = uint16_t(uint8_t(u[i]) | (uint16_t(uint8_t(u[i + 1])) << 8));
        i += 2;
        h.meta_len = get_le32(reinterpret_cast<const char*>(u + i));
        i += 4;
        h.body_len = get_le32(reinterpret_cast<const char*>(u + i));
        i += 4;

        if (h.spec != SPEC_MAGIC) return RpcError{StatusCode::PROTOCOL_ERROR, "Bad magic"};
        if (h.ver != SPEC_VER) return RpcError{StatusCode::PROTOCOL_ERROR, "Version mismatch"};

        const uint64_t expect_len = uint64_t(HDR_NO_LEN) + h.meta_len + h.body_len;
        if (h.len != expect_len || h.len < HDR_NO_LEN)
            return RpcError{StatusCode::PROTOCOL_ERROR, "Length mismatch"};

        if (h.meta_len > MAX_FRAME_NO_LEN || h.body_len > MAX_FRAME_NO_LEN)
            return RpcError{StatusCode::RESOURCE_EXHAUSTED, "Section too large"};

        return h;
    }

    namespace detail
    {
        template <class Buf>
        concept DynBufLike = requires(Buf& b)
        {
            { b.data() } -> std::convertible_to<const char*>;
            { b.size() } -> std::convertible_to<size_t>;
        };
    }

    template <detail::DynBufLike DynBuf>
    inline RpcExpected<UrpcHdr> decode_header(DynBuf& buf)
    {
        std::span<const std::byte> s{
            reinterpret_cast<const std::byte*>(buf.data()), buf.size()
        };
        return decode_header(s);
    }
} // namespace urpc

#endif // URPC_WIRE_H