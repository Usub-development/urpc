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
    };

    struct RpcServerConfig
    {
        std::string host;
        uint16_t port{0};
        int threads{1};
        std::shared_ptr<IRpcStreamFactory> stream_factory;
    };
}

#endif // URPC_CONFIG_H