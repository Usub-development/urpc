#include "urpc/ServerRouter.h"
#include "urpc/SocketRW.h"
#include "urpc/UrpcChannel.h"
#include "urpc/UrpcSettingsBuilder.h"
#include "urpc/Codec.h"
#include "urpc/Wire.h"
#include "urpc/Transport.h"
#include "urpc/Interceptors.h"
#include "uvent/Uvent.h"

#include <iostream>
#include <chrono>

using namespace usub::uvent;

struct EchoReq { std::string text; };
struct EchoResp { std::string text; };

task::Awaitable<void> echo_server(net::TCPServerSocket& srv)
{
    urpc::ServerRouter<urpc::RawTransport<urpc::SocketRW>> router;
    router.register_unary_sync<EchoReq, EchoResp>("Echo", [](EchoReq r)
    {
        std::cout << "[server] EchoReq: " << r.text << std::endl;
        return EchoResp{.text = "echo: " + r.text};
    });

    for (;;)
    {
        auto cli = co_await srv.async_accept();
        if (!cli) continue;

        urpc::SocketRW rw(std::move(*cli));
        std::cout << "[server] accepted\n";
        system::co_spawn(router.serve_connection(std::move(rw), urpc::TransportMode::RAW));
    }
}

task::Awaitable<void> echo_client()
{
    using namespace std::chrono_literals;
    co_await system::this_coroutine::sleep_for(200ms);

    net::TCPClientSocket sock;
    if (auto rc = co_await sock.async_connect("127.0.0.1", "7001"); rc.has_value())
    {
        std::cout << "[client] connect failed\n";
        co_return;
    }
    urpc::SocketRW rw(std::move(sock));
    auto ch = urpc::make_raw_channel(std::move(rw));

    // --- Interceptors ---
    urpc::ClientInterceptor ix{};
    ix.before_send = [](urpc::UrpcHdr& h, std::string& meta, std::string& body)
    {
        std::cout << "[ix] before_send method=" << h.method << " len=" << body.size() << "\n";
    };
    ix.after_recv = [](urpc::ParsedFrame& pf)
    {
        std::cout << "[ix] after_recv type=" << int(pf.h.type) << " len=" << pf.body.size() << "\n";
    };
    ch.add_interceptor(std::move(ix));

    urpc::UrpcSettingsBuilder sb;
    sb.mode(urpc::TransportMode::RAW).alpn("urpcv1").endpoint("127.0.0.1", 7001, "tcp");

    bool ok = co_await ch.open(sb);
    std::cout << "[client] open=" << (ok ? "ok" : "fail") << std::endl;
    if (!ok) co_return;

    // --- Call Echo ---
    if (auto r = co_await ch.unary_by_name<EchoReq, EchoResp>("Echo", EchoReq{.text = "ping"}))
        std::cout << "[client] EchoResp: " << r->text << std::endl;
    else
        std::cout << "[client] Echo failed\n";

    // --- Ping/Pong ---
    if (co_await ch.ping())
        std::cout << "[client] ping=ok\n";
    else
        std::cout << "[client] ping failed\n";

    // --- Cancel demo (no-op) ---
    (void)co_await ch.cancel(1);

    co_return;
}

int main()
{
    usub::Uvent uv(4);
    net::TCPServerSocket srv{"0.0.0.0", 7001};

    system::co_spawn(echo_server(srv));
    system::co_spawn(echo_client());

    uv.run();
    return 0;
}
