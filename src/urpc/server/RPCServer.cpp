#include <chrono>

#include <urpc/server/RPCServer.h>
#include <urpc/transport/TCPStreamFactory.h>

namespace urpc
{
    RpcServer::RpcServer(std::string host,
                         uint16_t port,
                         int threads)
        : RpcServer(RpcServerConfig{
            std::move(host),
            port,
            threads,
            nullptr
        })
    {
    }

    RpcServer::RpcServer(RpcServerConfig cfg)
        : registry_()
          , config_(std::move(cfg))
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcServer ctor host={} port={} threads={}",
            this->config_.host,
            this->config_.port,
            this->config_.threads);
#endif
        if (!this->config_.stream_factory)
        {
            this->config_.stream_factory =
                std::make_shared<TcpRpcStreamFactory>();
        }
    }

    RpcMethodRegistry& RpcServer::registry()
    {
#if URPC_LOGS
        usub::ulog::debug(
            "RpcServer::registry: returning registry, this={}",
            static_cast<const void*>(this));
#endif
        return this->registry_;
    }

    void RpcServer::register_method(uint64_t method_id,
                                    RpcHandlerFn fn)
    {
#if URPC_LOGS
        usub::ulog::debug(
            "RpcServer: register_method method_id={}",
            method_id);
#endif
        this->registry_.register_method(method_id, std::move(fn));
    }

    void RpcServer::register_method(std::string_view name,
                                    RpcHandlerFn fn)
    {
#if URPC_LOGS
        usub::ulog::debug(
            "RpcServer: register_method name={}", name);
#endif
        this->registry_.register_method(name, std::move(fn));
    }

    usub::uvent::task::Awaitable<void> RpcServer::run_async()
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcServer::run_async starting accept_loop "
            "host={} port={} threads={}",
            this->config_.host,
            this->config_.port,
            this->config_.threads);
#endif
        co_await this->accept_loop();
#if URPC_LOGS
        usub::ulog::warn(
            "RpcServer::run_async accept_loop finished");
#endif
        co_return;
    }

    void RpcServer::run()
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcServer::run starting with threads={}",
            this->config_.threads);
#endif

        usub::Uvent uvent(this->config_.threads);

#if URPC_LOGS
        usub::ulog::debug(
            "RpcServer::run: spawning run_async coroutine");
#endif
        usub::uvent::system::co_spawn(this->run_async());

        uvent.run();
#if URPC_LOGS
        usub::ulog::warn("RpcServer::run finished");
#endif
    }

    usub::uvent::task::Awaitable<void> RpcServer::accept_loop()
    {
        using namespace usub::uvent;
        using namespace std::chrono_literals;

#if URPC_LOGS
        usub::ulog::info(
            "RpcServer: creating TCPServerSocket on {}:{}",
            this->config_.host, this->config_.port);
#endif

        net::TCPServerSocket acceptor{
            this->config_.host.c_str(), this->config_.port
        };

#if URPC_LOGS
        usub::ulog::info(
            "RpcServer: accept_loop started, this={} acceptor_fd={}",
            static_cast<void*>(this),
            acceptor.get_raw_header()->fd);
#endif

        for (;;)
        {
#if URPC_LOGS
            usub::ulog::info("RpcServer: BEFORE async_accept()");
#endif
            auto soc = co_await acceptor.async_accept();
#if URPC_LOGS
            usub::ulog::info("RpcServer: AFTER async_accept()");
#endif

            if (!soc)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcServer: async_accept() returned empty socket, backing off");
#endif
                co_await system::this_coroutine::sleep_for(50ms);
                continue;
            }

#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: got TCPClientSocket, fd={}",
                soc->get_raw_header()->fd);
#endif

            if (!this->config_.stream_factory)
            {
                this->config_.stream_factory =
                    std::make_shared<TcpRpcStreamFactory>();
            }

            auto stream =
                co_await this->config_.stream_factory->create_server_stream(
                    std::move(soc.value()));

            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcServer: stream_factory returned nullptr, dropping connection");
#endif
                continue;
            }

            auto conn = std::make_shared<RpcConnection>(
                stream, this->registry_);

#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: BEFORE co_spawn(RpcConnection::run_detached), conn={}",
                static_cast<void*>(conn.get()));
#endif

            usub::uvent::system::co_spawn(
                urpc::RpcConnection::run_detached(conn));

#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: AFTER co_spawn(RpcConnection::run_detached), conn={}",
                static_cast<void*>(conn.get()));
#endif
        }
    }
}
