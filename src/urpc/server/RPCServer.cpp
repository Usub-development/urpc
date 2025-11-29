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
        usub::ulog::info(
            "RpcServer ctor host={} port={} threads={}",
            this->host_, this->port_, this->threads_);
    }

    RpcMethodRegistry& RpcServer::registry()
    {
        return this->registry_;
    }

    void RpcServer::register_method(uint64_t method_id,
                                    RpcHandlerFn fn)
    {
        usub::ulog::debug(
            "RpcServer: register_method method_id={}",
            method_id);
        this->registry_.register_method(method_id, std::move(fn));
    }

    void RpcServer::register_method(std::string_view name,
                                    RpcHandlerFn fn)
    {
        usub::ulog::debug(
            "RpcServer: register_method name={}", name);
        this->registry_.register_method(name, std::move(fn));
    }

    usub::uvent::task::Awaitable<void> RpcServer::run_async()
    {
        usub::ulog::info(
            "RpcServer::run_async starting accept_loop");
        co_await this->accept_loop();
        usub::ulog::warn(
            "RpcServer::run_async accept_loop finished");
        co_return;
    }

    void RpcServer::run()
    {
        usub::ulog::info(
            "RpcServer::run starting with threads={}",
            this->threads_);

        usub::Uvent uvent(this->threads_);

        usub::uvent::system::co_spawn(
            [this]() -> usub::uvent::task::Awaitable<void>
            {
                co_await this->accept_loop();
                co_return;
            }());

        uvent.run();

        usub::ulog::warn("RpcServer::run finished");
    }

    usub::uvent::task::Awaitable<void> RpcServer::accept_loop()
    {
        using namespace usub::uvent;

        usub::ulog::info(
            "RpcServer: creating TCPServerSocket on {}:{}",
            this->host_, this->port_);

        net::TCPServerSocket acceptor{
            this->host_.c_str(), this->port_
        };

        usub::ulog::info(
            "RpcServer: accept_loop started, this={} acceptor_fd={}",
            static_cast<void*>(this),
            acceptor.get_raw_header()->fd);

        for (;;)
        {
            usub::ulog::info(
                "RpcServer: BEFORE async_accept()");
            auto soc = co_await acceptor.async_accept();
            usub::ulog::info(
                "RpcServer: AFTER async_accept()");

            if (!soc)
            {
                usub::ulog::warn(
                    "RpcServer: async_accept() returned "
                    "empty socket");
                continue;
            }

            usub::ulog::info(
                "RpcServer: got TCPClientSocket, fd={}",
                soc->get_raw_header()->fd);

            usub::ulog::info(
                "RpcServer: BEFORE make_shared<TcpRpcStream>");
            auto stream =
                std::make_shared<TcpRpcStream>(
                    std::move(soc.value()));
            usub::ulog::info(
                "RpcServer: AFTER make_shared<TcpRpcStream> "
                "stream={}",
                static_cast<void*>(stream.get()));

            usub::ulog::info(
                "RpcServer: BEFORE make_shared<RpcConnection>");
            auto conn =
                std::make_shared<RpcConnection>(
                    stream, this->registry_);
            usub::ulog::info(
                "RpcServer: AFTER make_shared<RpcConnection> "
                "conn={}",
                static_cast<void*>(conn.get()));

            usub::ulog::info(
                "RpcServer: BEFORE co_spawn(RpcConnection::"
                "run_detached), conn={}",
                static_cast<void*>(conn.get()));

            usub::uvent::system::co_spawn(
                urpc::RpcConnection::run_detached(conn));

            usub::ulog::info(
                "RpcServer: AFTER co_spawn(RpcConnection::"
                "run_detached), conn={}",
                static_cast<void*>(conn.get()));
        }
    }
}
