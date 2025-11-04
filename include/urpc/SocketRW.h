#ifndef SOCKETRW_H
#define SOCKETRW_H

#include "uvent/Uvent.h"
#include <span>
#include <cstddef>
#include <cstring>

namespace urpc
{
    struct SocketRW
    {
        explicit SocketRW(usub::uvent::net::TCPClientSocket s) noexcept : sock(std::move(s))
        {
        }

        usub::uvent::task::Awaitable<void> write_all(std::span<const std::byte> src)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(src.data());
            size_t left = src.size();
            while (left)
            {
                auto res = co_await this->sock.async_send(const_cast<uint8_t*>(p), left);
                if (!res.has_value() || *res <= 0) co_return;
                const size_t wr = *res;
                p += wr;
                left -= static_cast<size_t>(wr);
            }
            co_return;
        }

        usub::uvent::task::Awaitable<size_t> read_some(std::span<std::byte> dst)
        {
            using namespace usub::uvent;
            utils::DynamicBuffer buf;
            const size_t req = std::min(dst.size(), this->CHUNK);
            buf.reserve(req);
            const ssize_t r = co_await this->sock.async_read(buf, req);
            if (r <= 0) co_return size_t(0);
            std::memcpy(dst.data(), buf.data(), static_cast<size_t>(r));
            co_return static_cast<size_t>(r);
        }

        [[nodiscard]] int native_handle() noexcept { return this->sock.get_raw_header()->fd; }

    private:
        usub::uvent::net::TCPClientSocket sock;
        static constexpr size_t CHUNK = 64 * 1024;
    };
} // namespace urpc

#endif // SOCKETRW_H