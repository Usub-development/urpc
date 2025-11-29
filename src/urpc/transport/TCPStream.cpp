// urpc/transport/TCPStream.cpp

#include <urpc/transport/TCPStream.h>

namespace urpc
{
    TcpRpcStream::TcpRpcStream(
        usub::uvent::net::TCPClientSocket&& sock)
        : socket_(std::move(sock))
    {
        usub::ulog::info(
            "TcpRpcStream ctor: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
    }

    usub::uvent::task::Awaitable<ssize_t>
    TcpRpcStream::async_read(
        usub::uvent::utils::DynamicBuffer& buf,
        size_t max_read)
    {
        usub::ulog::info(
            "TcpRpcStream::async_read: this={} fd={} max_read={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            max_read);
        co_return co_await this->socket_.async_read(buf, max_read);
    }

    usub::uvent::task::Awaitable<ssize_t>
    TcpRpcStream::async_write(uint8_t* data, size_t len)
    {
        usub::ulog::info(
            "TcpRpcStream::async_write: this={} fd={} len={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            len);
        co_return co_await this->socket_.async_write(data, len);
    }

    void TcpRpcStream::shutdown()
    {
        usub::ulog::info(
            "TcpRpcStream::shutdown: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
        this->socket_.shutdown();
    }
}
