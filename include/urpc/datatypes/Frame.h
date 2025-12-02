//
// Created by root on 11/29/25.
//

#ifndef FRAME_H
#define FRAME_H

#include <cstdint>
#include <cstring>
#include <span>

#include <uvent/utils/buffer/DynamicBuffer.h>

#include <urpc/utils/Systems.h>
#include <urpc/utils/Endianness.h>

namespace urpc
{
    enum class FrameType : uint8_t
    {
        Request = 0,
        Response = 1,
        Stream = 2,
        Cancel = 3,
        Ping = 4,
        Pong = 5,
    };

    enum FrameFlags : uint8_t
    {
        FLAG_END_STREAM = 0x01,
        FLAG_ERROR = 0x02,
        FLAG_COMPRESSED = 0x04,
    };

    struct RpcFrameHeader
    {
        uint32_t magic; // 'URPC' = 0x55525043
        uint8_t version;
        uint8_t type;
        uint16_t flags;
        uint32_t reserved;
        uint32_t stream_id;
        uint64_t method_id;
        uint32_t length;
    };

    constexpr size_t RpcFrameHeaderSize =
        sizeof(uint32_t) +
        1 +
        1 +
        sizeof(uint16_t) +
        sizeof(uint32_t) +
        sizeof(uint32_t) +
        sizeof(uint64_t) +
        sizeof(uint32_t);

    URPC_ALWAYS_INLINE void serialize_header(const RpcFrameHeader& src, uint8_t* out)
    {
        auto put_be = [](uint8_t*& p, auto v)
        {
            using T = decltype(v);
            T be = host_to_be<T>(v);
            std::memcpy(p, &be, sizeof(T));
            p += sizeof(T);
        };

        auto put8 = [](uint8_t*& p, uint8_t v)
        {
            *p++ = v;
        };

        uint8_t* p = out;

        put_be(p, static_cast<uint32_t>(src.magic));
        put8(p, src.version);
        put8(p, static_cast<uint8_t>(src.type));
        put_be(p, static_cast<uint16_t>(src.flags));
        put_be(p, static_cast<uint32_t>(src.reserved));
        put_be(p, static_cast<uint32_t>(src.stream_id));
        put_be(p, static_cast<uint64_t>(src.method_id));
        put_be(p, static_cast<uint32_t>(src.length));
    }

    URPC_ALWAYS_INLINE RpcFrameHeader parse_header(const uint8_t* in)
    {
        auto get_be = [](const uint8_t*& p, auto& out)
        {
            using T = std::remove_reference_t<decltype(out)>;
            T be{};
            std::memcpy(&be, p, sizeof(T));
            p += sizeof(T);
            out = be_to_host<T>(be);
        };

        auto get8 = [](const uint8_t*& p, uint8_t& out)
        {
            out = *p++;
        };

        RpcFrameHeader h{};
        const uint8_t* p = in;

        get_be(p, h.magic);
        get8(p, h.version);
        get8(p, h.type);
        get_be(p, h.flags);
        get_be(p, h.reserved);
        get_be(p, h.stream_id);
        get_be(p, h.method_id);
        get_be(p, h.length);

        return h;
    }

    struct RpcFrame
    {
        RpcFrameHeader header;
        usub::uvent::utils::DynamicBuffer payload;
    };
}

#endif //FRAME_H
