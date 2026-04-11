//
// Created by root on 11/29/25.
//

#ifndef TCPSTREAM_H
#define TCPSTREAM_H

#include <atomic>

#include <uvent/net/Socket.h>
#include <urpc/transport/IRPCStream.h>
#include <ulog/ulog.h>

namespace urpc
{
    namespace diag
    {
        inline std::atomic<uint64_t> tcp_stream_ctor_count{0};
        inline std::atomic<uint64_t> tcp_stream_dtor_count{0};
    }

    class TcpRpcStream : public IRpcStream
    {
    public:
        explicit TcpRpcStream(
            usub::uvent::net::TCPClientSocket&& sock);

        ~TcpRpcStream() override;

        usub::uvent::task::Awaitable<ssize_t> async_read(
            usub::uvent::utils::DynamicBuffer& buf,
            size_t max_read) override;

        usub::uvent::task::Awaitable<ssize_t> async_write(
            uint8_t* data,
            size_t len) override;

        [[nodiscard]] const RpcPeerIdentity* peer_identity() const noexcept override;

        [[nodiscard]] bool get_app_secret_key(
            std::array<uint8_t, 32>& out_key) const noexcept override
        {
            return false;
        }

        void shutdown() override;

    private:
        usub::uvent::net::TCPClientSocket socket_;
    };
}

#endif // TCPSTREAM_H