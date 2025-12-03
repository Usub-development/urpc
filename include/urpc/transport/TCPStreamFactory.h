//
// Created by root on 01.12.2025.
//

#ifndef URPC_TCPSTREAMFACTORY_H
#define URPC_TCPSTREAMFACTORY_H

#include <memory>
#include <string>
#include <chrono>

#include <uvent/tasks/Awaitable.h>
#include <uvent/net/Socket.h>

#include <urpc/transport/IRPCStreamFactory.h>
#include <urpc/transport/TCPStream.h>

namespace urpc
{
    class TcpRpcStreamFactory : public IRpcStreamFactory
    {
    public:
        explicit TcpRpcStreamFactory(int timeout_ms = -1)
            : timeout_ms_(timeout_ms)
        {
        }

        usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_client_stream(const std::string& host,
                             uint16_t port) override
        {
            using namespace usub::uvent;

            net::TCPClientSocket sock;

            if (timeout_ms_ > 0)
                sock.set_timeout_ms(timeout_ms_);

            auto port_str = std::to_string(port);

            if (timeout_ms_ > 0)
            {
                auto res = co_await sock.async_connect(
                    host.c_str(),
                    port_str.c_str(),
                    std::chrono::milliseconds{timeout_ms_});
                if (res.has_value())
                {
#if URPC_LOGS
                    usub::ulog::error(
                        "TcpRpcStreamFactory::create_client_stream: "
                        "async_connect failed ec={}",
                        res.value());
#endif
                    co_return nullptr;
                }
            }
            else
            {
                auto res = co_await sock.async_connect(
                    host.c_str(),
                    port_str.c_str());
                if (res.has_value())
                {
#if URPC_LOGS
                    usub::ulog::error(
                        "TcpRpcStreamFactory::create_client_stream: "
                        "async_connect failed ec={}",
                        res.value());
#endif
                    co_return nullptr;
                }
            }

            auto stream = std::make_shared<TcpRpcStream>(std::move(sock));
            co_return stream;
        }


        usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_server_stream(usub::uvent::net::TCPClientSocket&& socket) override
        {
            if (timeout_ms_ > 0)
                socket.set_timeout_ms(timeout_ms_);

            auto stream =
                std::make_shared<TcpRpcStream>(std::move(socket));
            co_return stream;
        }

    private:
        int timeout_ms_;
    };
}

#endif // URPC_TCPSTREAMFACTORY_H