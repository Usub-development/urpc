//
// Created by root on 11/4/25.
//

#ifndef WIRE_H
#define WIRE_H

#include <cstdint>
#include <string>

namespace urpcv1
{
    enum class MsgType : uint8_t { REQUEST = 0, RESPONSE = 1, ERROR = 2, CANCEL = 3 };

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
        char b[4];
        for (int i = 0; i < 4; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
        o.append(b, b + 4);
    }

    inline void put_le64(std::string& o, uint64_t v)
    {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
        o.append(b, b + 8);
    }

    inline uint32_t get_le32(const char* p)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(p[i])) << (8 * i);
        return v;
    }

    inline uint64_t get_le64(const char* p)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(p[i])) << (8 * i);
        return v;
    }


    inline constexpr size_t HDR_NO_LEN = 1 + 1 + 1 + 1 + 4 + 8 + 4 + 4;
    inline constexpr size_t HDR_SIZE = 4 + HDR_NO_LEN;


    inline void hdr_encode(std::string& out, const UrpcHdr& h)
    {
        put_le32(out, h.len);
        out.push_back((char)h.ver);
        out.push_back((char)h.type);
        out.push_back((char)h.flags);
        out.push_back((char)h.rsv);
        put_le32(out, h.stream);
        put_le64(out, h.method);
        put_le32(out, h.meta_len);
        put_le32(out, h.body_len);
    }


    inline bool hdr_decode(const char* p, size_t n, UrpcHdr& h)
    {
        if (n < HDR_SIZE) return false;
        size_t i = 0;
        h.len = get_le32(p + i);
        i += 4;
        h.ver = (uint8_t)p[i++];
        h.type = (uint8_t)p[i++];
        h.flags = (uint8_t)p[i++];
        h.rsv = (uint8_t)p[i++];
        h.stream = get_le32(p + i);
        i += 4;
        h.method = get_le64(p + i);
        i += 8;
        h.meta_len = get_le32(p + i);
        i += 4;
        h.body_len = get_le32(p + i);
        i += 4;
        return true;
    }


    struct ParsedFrame
    {
        UrpcHdr h{};
        std::string meta;
        std::string body;
    };


    inline std::string make_frame(UrpcHdr h, std::string&& meta, std::string&& body)
    {
        h.meta_len = meta.size();
        h.body_len = body.size();
        h.len = uint32_t(HDR_NO_LEN + h.meta_len + h.body_len);
        std::string out;
        out.reserve(HDR_SIZE + h.meta_len + h.body_len);
        hdr_encode(out, h);
        out += meta;
        out += body;
        return out;
    }


    inline bool parse_frame(const char* p, size_t n, ParsedFrame& pf)
    {
        UrpcHdr h{};
        if (!hdr_decode(p, n, h)) return false;
        if (n < HDR_SIZE + h.meta_len + h.body_len) return false;
        pf.h = h;
        pf.meta.assign(p + HDR_SIZE, p + HDR_SIZE + h.meta_len);
        pf.body.assign(p + HDR_SIZE + h.meta_len, p + HDR_SIZE + h.meta_len + h.body_len);
        return true;
    }
}

#endif //WIRE_H
