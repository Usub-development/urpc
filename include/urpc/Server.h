#ifndef URPC_SERVER_H
#define URPC_SERVER_H

#include <string>
#include <utility>

#include "uvent/Uvent.h"
#include "uvent/net/Socket.h"

#include "urpc/Wire.h"
#include "urpc/Codec.h"
#include "urpc/Transport.h"
#include "urpc/ServerRouter.h"
#include "urpc/UrpcSettingsIO.h"

namespace urpc
{
    template <TransportLike T>
    class Server
    {
    public:
        Server() = default;

        template <class Req, class Resp, class Fn>
        void register_unary(std::string name, Fn fn)
        {
            router_.template register_unary<Req, Resp>(std::move(name), std::move(fn));
        }

        template <class Req, class Resp, class C>
        void register_unary(std::string name, C* obj, usub::uvent::task::Awaitable<Resp> (C::*mf)(Req))
        {
            router_.template register_unary<Req, Resp>(std::move(name), obj, mf);
        }

        template <class Req, class Resp, class Fn>
        void register_unary_sync(std::string name, Fn fn)
        {
            router_.template register_unary_sync<Req, Resp>(std::move(name), std::move(fn));
        }

        template <class Req, class Resp, class Fn>
        void register_server_streaming(std::string name, Fn fn)
        {
            router_.template register_server_streaming<Req, Resp>(std::move(name), std::move(fn));
        }

        template <class RW>
        usub::uvent::task::Awaitable<void> serve_connection(RW rw, TransportMode mode)
        {
            co_await router_.template serve_connection<RW>(std::move(rw), mode);
            co_return;
        }

        template <class RW>
        usub::uvent::task::Awaitable<void> serve_loop(uint16_t port, TransportMode mode, std::string host = "0.0.0.0")
        {
            usub::uvent::net::TCPServerSocket acceptor{host.c_str(), port};
            for (;;)
            {
                auto soc = co_await acceptor.async_accept();
                if (!soc) co_return;
                usub::uvent::system::co_spawn(
                    router_.template serve_connection<RW>(RW(std::move(*soc)), mode)
                );
            }
        }

        usub::uvent::task::Awaitable<void> goaway_on(T& transport)
        {
            co_await router_.goaway(transport);
            co_return;
        }

    private:
        ServerRouter<T> router_{};
    };
} // namespace urpc

#endif // URPC_SERVER_H