//
// Created by root on 11/29/25.
//

#ifndef TCPSTREAM_H
#define TCPSTREAM_H

#include <uvent/net/Socket.h>
#include <urpc/transport/IRPCStream.h>
#include <ulog/ulog.h>

namespace urpc
{
    class TcpRpcStream : public IRpcStream
    {
    public:
        explicit TcpRpcStream(
            usub::uvent::net::TCPClientSocket&& sock);

        usub::uvent::task::Awaitable<ssize_t> async_read(
            usub::uvent::utils::DynamicBuffer& buf,
            size_t max_read) override;

        usub::uvent::task::Awaitable<ssize_t> async_write(
            uint8_t* data,
            size_t len) override;

        void shutdown() override;

    private:
        usub::uvent::net::TCPClientSocket socket_;
    };
}

#endif // TCPSTREAM_H