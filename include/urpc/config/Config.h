//
// Created by root on 01.12.2025.
//

#ifndef URPC_CONFIG_H
#define URPC_CONFIG_H

#include <cstdint>
#include <memory>
#include <string>

namespace urpc
{
    struct IRpcStreamFactory;

    struct RpcClientConfig
    {
        std::string host;
        uint16_t port{0};
        std::shared_ptr<IRpcStreamFactory> stream_factory;
        uint32_t ping_interval_ms{0};
        int socket_timeout_ms{-1};
    };

    struct RpcServerConfig
    {
        std::string host;
        uint16_t port{0};
        int threads{1};
        std::shared_ptr<IRpcStreamFactory> stream_factory;
        int timeout_ms{-1};
    };
}

#endif // URPC_CONFIG_H