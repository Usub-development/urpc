//
// Created by root on 01.12.2025.
//

#ifndef URPC_CONFIG_H
#define URPC_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace urpc
{
    struct IRpcStreamFactory;

    enum class RpcCancelStage : uint8_t
    {
        BeforeHandler = 0,
        AfterHandler  = 1,
    };

    struct RpcCancelEvent
    {
        RpcCancelStage stage;
        uint32_t       stream_id;
        uint64_t       method_id;
        std::size_t    dropped_response_bytes;
    };

    using RpcCancelCallback = std::function<void(const RpcCancelEvent&)>;

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

        RpcCancelCallback on_request_cancelled;
    };
}

#endif // URPC_CONFIG_H