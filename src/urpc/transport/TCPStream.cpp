#include <urpc/transport/TCPStream.h>

namespace urpc
{
    TcpRpcStream::TcpRpcStream(
        usub::uvent::net::TCPClientSocket&& sock)
        : socket_(std::move(sock))
    {
#if URPC_LOGS
        usub::ulog::info(
            "TcpRpcStream ctor: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
#endif
    }

    usub::uvent::task::Awaitable<ssize_t>
    TcpRpcStream::async_read(
        usub::uvent::utils::DynamicBuffer& buf,
        size_t max_read)
    {
#if URPC_LOGS
        usub::ulog::info(
            "TcpRpcStream::async_read: this={} fd={} max_read={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            max_read);
#endif
        co_return co_await this->socket_.async_read(buf, max_read);
    }

    usub::uvent::task::Awaitable<ssize_t>
    TcpRpcStream::async_write(uint8_t* data, size_t len)
    {
#if URPC_LOGS
        usub::ulog::info(
            "TcpRpcStream::async_write: this={} fd={} len={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            len);
#endif
        co_return co_await this->socket_.async_write(data, len);
    }

    const RpcPeerIdentity* TcpRpcStream::peer_identity() const noexcept
    {
        return nullptr;
    }

    void TcpRpcStream::shutdown()
    {
#if URPC_LOGS
        usub::ulog::info(
            "TcpRpcStream::shutdown: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
#endif
        this->socket_.shutdown();
    }
}