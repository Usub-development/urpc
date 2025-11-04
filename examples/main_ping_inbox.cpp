#include "urpc/ServerRouter.h"
#include "urpc/SocketRW.h"
#include "urpc/UrpcChannel.h"
#include "urpc/UrpcSettingsBuilder.h"
#include "urpc/Codec.h"
#include "urpc/Wire.h"
#include "urpc/Transport.h"
#include "uvent/Uvent.h"
#include <iostream>
#include <chrono>

using namespace usub::uvent;

struct EchoReq
{
    std::string text;
};

struct EchoResp
{
    std::string text;
};

struct MulReq
{
    int64_t a{}, b{};
};

struct MulResp
{
    int64_t v{};
};

static task::Awaitable<void> echo_svc_auto(net::TCPServerSocket& srv)
{
    urpc::ServerRouter<urpc::RawTransport<urpc::SocketRW>> router;

    router.register_unary_sync<EchoReq, EchoResp>("Echo",
                                                  [](EchoReq r) { return EchoResp{.text = "echo: " + r.text}; });

    router.register_unary<MulReq, MulResp>("Mul",
                                           [](MulReq r)-> task::Awaitable<MulResp>
                                           {
                                               co_return MulResp{.v = r.a * r.b};
                                           });

    for (;;)
    {
        auto cli = co_await srv.async_accept();
        if (!cli) continue;
        urpc::SocketRW rw(std::move(*cli));
        system::co_spawn(router.serve_connection(std::move(rw), urpc::TransportMode::RAW));
    }
}

static task::Awaitable<void> run_client_inbox()
{
    using namespace std::chrono_literals;

    co_await system::this_coroutine::sleep_for(150ms);

    net::TCPClientSocket sock;
    {
        auto rc = co_await sock.async_connect("127.0.0.1", "7001");
        if (rc.has_value()) co_return;
    }

    urpc::SocketRW rw(std::move(sock));
    auto ch = urpc::make_raw_channel(std::move(rw));

    urpc::UrpcSettingsBuilder sb;
    sb.mode(urpc::TransportMode::RAW).alpn("urpcv1").endpoint("127.0.0.1", 7001, "tcp");

    bool ok = co_await ch.open(sb);
    std::cout << "[client2] open=" << (ok ? "ok" : "fail") << std::endl;
    if (!ok) co_return;

    ok = co_await ch.ping();
    std::cout << "[client2] ping=" << (ok ? "ok" : "fail") << std::endl;

    ch.enable_inbox(1024, true);
    system::co_spawn(ch.pump_inbox());

    if (auto r = co_await ch.unary_by_name<EchoReq, EchoResp>("Echo", EchoReq{.text = "hello from client2"}))
        std::cout << "[client2] EchoResp: " << r->text << std::endl;

    if (auto r2 = co_await ch.unary_by_name<MulReq, MulResp>("Mul", MulReq{.a = 9, .b = 9}))
        std::cout << "[client2] MulResp: " << r2->v << std::endl;

    co_await system::this_coroutine::sleep_for(200ms);
    ch.stop_inbox();
    std::cout << "[client2] inbox_overflow=" << ch.inbox_overflow_count() << std::endl;

    co_return;
}

int main()
{
    usub::Uvent uvent(4);

    net::TCPServerSocket srv{"0.0.0.0", 7001};
    system::co_spawn(echo_svc_auto(srv));
    system::co_spawn(run_client_inbox());

    uvent.run();
    return 0;
}
