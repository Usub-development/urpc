//
// Created by root on 11/29/25.
//

#ifndef RPCSERVER_H
#define RPCSERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <uvent/Uvent.h>
#include <uvent/system/SystemContext.h>
#include <uvent/net/Socket.h>

#include <ulog/ulog.h>

#include <urpc/config/Config.h>
#include <urpc/registry/RPCMethodRegistry.h>
#include <urpc/connection/RPCConnection.h>
#include <urpc/transport/IRPCStreamFactory.h>

namespace urpc
{
    class RpcServer
    {
    public:
        RpcServer(std::string host,
                  uint16_t port,
                  int threads);

        explicit RpcServer(RpcServerConfig cfg);

        RpcMethodRegistry& registry();

        template <uint64_t MethodId, typename F>
        void register_method_ct(F&& f)
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcServer: register_method_ct MethodId={}",
                MethodId);
#endif
            this->registry_.register_method_ct<MethodId>(
                std::forward<F>(f));
        }

        void register_method(uint64_t method_id, RpcHandlerFn fn);
        void register_method(std::string_view name, RpcHandlerFn fn);

        usub::uvent::task::Awaitable<void> run_async();
        void run();

    private:
        usub::uvent::task::Awaitable<void> accept_loop();

    private:
        RpcMethodRegistry registry_;
        RpcServerConfig config_;
    };
}

#endif // RPCSERVER_H