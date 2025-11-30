// urpc/server/RpcServer.cpp

#include <urpc/server/RPCServer.h>

namespace urpc
{
    RpcServer::RpcServer(std::string host,
                         uint16_t port,
                         int threads)
        : host_(std::move(host))
          , port_(port)
          , threads_(threads)
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcServer ctor host={} port={} threads={}",
            this->host_, this->port_, this->threads_);
#endif
    }

    RpcMethodRegistry& RpcServer::registry()
    {
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
            "RpcServer::run_async starting accept_loop");
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
            this->threads_);
#endif

        usub::Uvent uvent(this->threads_);

        usub::uvent::system::co_spawn(
            [this]() -> usub::uvent::task::Awaitable<void>
            {
                co_await this->accept_loop();
                co_return;
            }());

        uvent.run();
#if URPC_LOGS
        usub::ulog::warn("RpcServer::run finished");
#endif
    }

    usub::uvent::task::Awaitable<void> RpcServer::accept_loop()
    {
        using namespace usub::uvent;
#if URPC_LOGS
        usub::ulog::info(
            "RpcServer: creating TCPServerSocket on {}:{}",
            this->host_, this->port_);
#endif

        net::TCPServerSocket acceptor{
            this->host_.c_str(), this->port_
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
            usub::ulog::info(
                "RpcServer: BEFORE async_accept()");
#endif
            auto soc = co_await acceptor.async_accept();
#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: AFTER async_accept()");
#endif

            if (!soc)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcServer: async_accept() returned "
                    "empty socket");
#endif
                continue;
            }
#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: got TCPClientSocket, fd={}",
                soc->get_raw_header()->fd);

            usub::ulog::info(
                "RpcServer: BEFORE make_shared<TcpRpcStream>");
#endif
            auto stream =
                std::make_shared<TcpRpcStream>(
                    std::move(soc.value()));
#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: AFTER make_shared<TcpRpcStream> "
                "stream={}",
                static_cast<void*>(stream.get()));

            usub::ulog::info(
                "RpcServer: BEFORE make_shared<RpcConnection>");
#endif
            auto conn =
                std::make_shared<RpcConnection>(
                    stream, this->registry_);
#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: AFTER make_shared<RpcConnection> "
                "conn={}",
                static_cast<void*>(conn.get()));

            usub::ulog::info(
                "RpcServer: BEFORE co_spawn(RpcConnection::"
                "run_detached), conn={}",
                static_cast<void*>(conn.get()));
#endif
            usub::uvent::system::co_spawn(
                urpc::RpcConnection::run_detached(conn));
#if URPC_LOGS
            usub::ulog::info(
                "RpcServer: AFTER co_spawn(RpcConnection::"
                "run_detached), conn={}",
                static_cast<void*>(conn.get()));
#endif
        }
    }
}
