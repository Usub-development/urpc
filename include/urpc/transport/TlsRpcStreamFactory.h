//
// Created by root on 12/1/25.
//

#ifndef TLSRPCSTREAMFACTORY_H
#define TLSRPCSTREAMFACTORY_H

#include <memory>
#include <string>

#include <uvent/tasks/Awaitable.h>
#include <uvent/net/Socket.h>
#include <ulog/ulog.h>

#include <urpc/transport/IRPCStream.h>
#include <urpc/transport/IRPCStreamFactory.h>
#include <urpc/transport/TCPStream.h>
#include <urpc/transport/TlsRpcStream.h>
#include <urpc/transport/TlsConfig.h>

namespace urpc
{
    class TlsRpcStreamFactory : public IRpcStreamFactory
    {
    public:
        explicit TlsRpcStreamFactory(TlsClientConfig client_cfg)
            : client_cfg_(std::move(client_cfg))
        {
        }

        void set_server_cfg(TlsServerConfig cfg)
        {
            server_cfg_ = std::move(cfg);
        }

        usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_client_stream(const std::string& host,
                             uint16_t port) override
        {
            using namespace usub::uvent;

            if (!client_cfg_.enabled)
            {
#if URPC_LOGS
                usub::ulog::info(
                    "TlsRpcStreamFactory::create_client_stream: TLS disabled, using plain TCP");
#endif
                net::TCPClientSocket sock;

                if (client_cfg_.socket_timeout_ms > 0)
                    sock.set_timeout_ms(client_cfg_.socket_timeout_ms);

                auto port_str = std::to_string(port);

                if (client_cfg_.socket_timeout_ms > 0)
                {
                    auto res = co_await sock.async_connect(
                        host.c_str(),
                        port_str.c_str(),
                        std::chrono::milliseconds{client_cfg_.socket_timeout_ms});
                    if (res.has_value())
                    {
#if URPC_LOGS
                        usub::ulog::error(
                            "TlsRpcStreamFactory::create_client_stream (plain): async_connect failed ec={}",
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
                            "TlsRpcStreamFactory::create_client_stream (plain): async_connect failed ec={}",
                            res.value());
#endif
                        co_return nullptr;
                    }
                }

                auto stream = std::make_shared<TcpRpcStream>(std::move(sock));
                co_return stream;
            }

#if URPC_LOGS
            usub::ulog::info(
                "TlsRpcStreamFactory::create_client_stream: TLS enabled, connecting to {}:{}",
                host, port);
#endif

            auto stream = co_await TlsRpcStream::connect(host, port, client_cfg_);
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "TlsRpcStreamFactory::create_client_stream: TlsRpcStream::connect failed");
#endif
            }
            co_return stream;
        }

        usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_server_stream(usub::uvent::net::TCPClientSocket&& socket) override
        {
            using namespace usub::uvent;

            if (!server_cfg_.enabled)
            {
#if URPC_LOGS
                usub::ulog::info(
                    "TlsRpcStreamFactory::create_server_stream: TLS disabled, using plain TCP");
#endif
                auto stream = std::make_shared<TcpRpcStream>(std::move(socket));
                co_return stream;
            }

#if URPC_LOGS
            usub::ulog::info(
                "TlsRpcStreamFactory::create_server_stream: TLS enabled for accepted socket");
#endif

            auto stream = co_await TlsRpcStream::from_accepted_socket(
                std::move(socket), server_cfg_);
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "TlsRpcStreamFactory::create_server_stream: from_accepted_socket failed");
#endif
            }
            co_return stream;
        }

    private:
        TlsClientConfig client_cfg_;
        TlsServerConfig server_cfg_{};
    };
}

#endif //TLSRPCSTREAMFACTORY_H