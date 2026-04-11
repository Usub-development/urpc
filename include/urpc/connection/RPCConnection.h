//
// Created by root on 11/29/25.
//

#ifndef RPCCONNECTION_H
#define RPCCONNECTION_H

#include <memory>
#include <unordered_map>
#include <span>

#include <uvent/tasks/Awaitable.h>
#include <uvent/sync/AsyncMutex.h>
#include <uvent/sync/AsyncCancellation.h>
#include <uvent/system/SystemContext.h>
#include <uvent/utils/buffer/DynamicBuffer.h>

#include <ulog/ulog.h>

#include <urpc/datatypes/Frame.h>
#include <urpc/context/RPCContext.h>
#include <urpc/registry/RPCMethodRegistry.h>
#include <urpc/transport/IRPCStream.h>
#include <urpc/transport/IOOps.h>
#include <urpc/utils/Endianness.h>
#include <urpc/config/Config.h>

namespace urpc
{
    class RpcConnection
        : public std::enable_shared_from_this<RpcConnection>
    {
    public:
        RpcConnection(std::shared_ptr<IRpcStream> stream,
                      RpcMethodRegistry& registry);

        RpcConnection(std::shared_ptr<IRpcStream> stream,
                      RpcMethodRegistry& registry,
                      RpcCancelCallback on_cancel);

        static usub::uvent::task::Awaitable<void> run_detached(
            std::shared_ptr<RpcConnection> self);

    private:
        usub::uvent::task::Awaitable<void> loop();

        usub::uvent::task::Awaitable<void> locked_send(
            const RpcFrameHeader& hdr,
            std::span<const uint8_t> body);

        usub::uvent::task::Awaitable<void> send_response(
            RpcContext& ctx,
            std::span<const uint8_t> body);

        usub::uvent::task::Awaitable<void>
        send_simple_error(RpcContext& ctx,
                          uint32_t error_code,
                          std::string_view message,
                          std::span<const uint8_t> details = {});

        usub::uvent::task::Awaitable<void> handle_request(RpcFrame frame);
        usub::uvent::task::Awaitable<void> handle_cancel(RpcFrame frame);
        usub::uvent::task::Awaitable<void> handle_ping(RpcFrame frame);

        static usub::uvent::task::Awaitable<void>
        handle_request_detached(std::shared_ptr<RpcConnection> self,
                                RpcFrame frame);

    private:
        std::shared_ptr<IRpcStream> stream_;
        RpcMethodRegistry& registry_;
        RpcCancelCallback on_cancel_;

        usub::uvent::sync::AsyncMutex write_mutex_;
        usub::uvent::sync::AsyncMutex cancel_map_mutex_;
        std::unordered_map<
            uint64_t,
            std::shared_ptr<usub::uvent::sync::CancellationSource>> cancel_map_;
    };
}

#endif // RPCCONNECTION_H
