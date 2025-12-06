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
#include <type_traits>
#include <span>

#include <uvent/Uvent.h>
#include <uvent/system/SystemContext.h>
#include <uvent/net/Socket.h>

#include <ulog/ulog.h>

#include <urpc/config/Config.h>
#include <urpc/registry/RPCMethodRegistry.h>
#include <urpc/connection/RPCConnection.h>
#include <urpc/transport/IRPCStreamFactory.h>
#include <urpc/context/RPCContext.h>

namespace urpc
{
    namespace detail
    {
        template <class T>
        struct awaitable_value;

        template <class T>
        struct awaitable_value<usub::uvent::task::Awaitable<T>>
        {
            using type = T;
        };

        template <class T>
        using awaitable_value_t = typename awaitable_value<T>::type;
    }

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
            using Functor = std::decay_t<F>;
            static Functor func = std::forward<F>(f);

            using RawRet = std::invoke_result_t<
                Functor&,
                RpcContext&,
                std::span<const std::uint8_t>>;

            using Result = detail::awaitable_value_t<RawRet>;

            auto wrapper =
                [](urpc::RpcContext& ctx,
                   std::span<const std::uint8_t> body)
                -> usub::uvent::task::Awaitable<std::vector<std::uint8_t>>
            {
                if constexpr (std::is_same_v<Result, std::vector<std::uint8_t>>)
                {
                    co_return co_await func(ctx, body);
                }
                else if constexpr (ByteRange<Result>)
                {
                    Result r = co_await func(ctx, body);
                    co_return to_byte_vector(std::move(r));
                }
                else
                {
                    static_assert(
                        std::is_same_v<Result, void>,
                        "RpcServer::register_method_ct: unsupported handler result type");
                    co_return std::vector<std::uint8_t>{};
                }
            };

            this->register_method(MethodId, wrapper);
        }

        void register_method(uint64_t method_id, RpcHandlerPtr fn);
        void register_method(std::string_view name, RpcHandlerPtr fn);

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