//
// Created by root on 11/29/25.
//

#ifndef RPCCONTEXT_H
#define RPCCONTEXT_H

#include <span>
#include <functional>

#include <uvent/tasks/Awaitable.h>
#include <uvent/sync/AsyncCancellation.h>

#include <urpc/transport/IRPCStream.h>

namespace urpc
{
    struct RpcContext
    {
        IRpcStream& stream;
        uint32_t stream_id;
        uint64_t method_id;
        uint16_t flags;
        usub::uvent::sync::CancellationToken cancel_token;
    };

    using RpcHandlerAwaitable = usub::uvent::task::Awaitable<void>;

    using RpcHandlerFn = usub::uvent::task::Awaitable<std::vector<uint8_t>>(
        RpcContext&, std::span<const uint8_t>);

    using RpcHandlerPtr = RpcHandlerFn*;
}

#endif // RPCCONTEXT_H
