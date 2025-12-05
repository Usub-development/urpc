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
#include <concepts>
#include <vector>

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
        requires std::same_as<
            decltype(std::declval<F&>()(
                std::declval<urpc::RpcContext&>(),
                std::declval<std::span<const std::uint8_t>>())),
            usub::uvent::task::Awaitable<std::vector<std::uint8_t>>>
        void register_method_ct(F&& f)
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcServer: register_method_ct MethodId={}",
                MethodId);
#endif
            using Functor = std::decay_t<F>;
            static Functor func = std::forward<F>(f);

            auto wrapper = [](urpc::RpcContext& ctx,
                              std::span<const std::uint8_t> body)
                -> usub::uvent::task::Awaitable<std::vector<std::uint8_t>>
            {
                co_return co_await func(ctx, body);
            };

            this->register_method(MethodId, wrapper);
        }

        template <uint64_t MethodId, typename F>
        requires std::same_as<
            decltype(std::declval<F&>()(
                std::declval<urpc::RpcContext&>(),
                std::declval<std::span<const std::uint8_t>>())),
            usub::uvent::task::Awaitable<std::string>>
        void register_method_ct(F&& f)
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcServer: register_method_ct<string> MethodId={}",
                MethodId);
#endif
            using Functor = std::decay_t<F>;
            static Functor func = std::forward<F>(f);

            auto wrapper = [](urpc::RpcContext& ctx,
                              std::span<const std::uint8_t> body)
                -> usub::uvent::task::Awaitable<std::vector<std::uint8_t>>
            {
                std::string s = co_await func(ctx, body);
                std::vector<std::uint8_t> out;
                out.assign(s.begin(), s.end());
                co_return out;
            };

            this->register_method(MethodId, wrapper);
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
