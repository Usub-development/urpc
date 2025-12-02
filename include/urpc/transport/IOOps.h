#ifndef IOOPS_H
#define IOOPS_H

#include <span>
#include <urpc/datatypes/Frame.h>
#include <urpc/transport/IRPCStream.h>

namespace urpc
{
    using namespace usub::uvent;

    inline task::Awaitable<bool> write_all(
        IRpcStream& stream,
        const uint8_t* data,
        size_t n)
    {
        ssize_t r = co_await stream.async_write(
            const_cast<uint8_t*>(data),
            n);
        if (r <= 0) co_return false;
        co_return true;
    }

    inline task::Awaitable<bool> send_frame(
        IRpcStream& stream,
        const RpcFrameHeader& hdr,
        std::span<const uint8_t> payload)
    {
        uint8_t header_buf[RpcFrameHeaderSize];
        serialize_header(hdr, header_buf);

        if (!(co_await write_all(stream, header_buf, RpcFrameHeaderSize)))
            co_return false;

        if (!payload.empty())
        {
            if (!(co_await write_all(stream, payload.data(), payload.size())))
                co_return false;
        }

        co_return true;
    }
}

#endif // IOOPS_H
