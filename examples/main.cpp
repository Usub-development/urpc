#include "urpc/ServerRouter.h"
#include "urpc/SocketRW.h"
#include "urpc/UrpcChannel.h"
#include "urpc/UrpcSettingsBuilder.h"
#include "urpc/Codec.h"
#include "urpc/Wire.h"
#include "urpc/Transport.h"
#include "uvent/Uvent.h"
#include <iostream>

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

template <typename RW>
task::Awaitable<bool> handshake_raw(RW& rw)
{
    std::cout << "[server] wait SETTINGS..." << std::endl;
    urpc::Framer fr;
    fr.transport_bits = urpc::derive_transport_flags(urpc::TransportMode::RAW);

    auto pf = co_await urpc::recv_one(rw, fr);
    if (!pf)
    {
        std::cout << "[server] no frame" << std::endl;
        co_return false;
    }

    std::cout << "[server] got frame: type=" << int(pf->h.type)
        << " stream=" << pf->h.stream
        << " method=" << pf->h.method
        << " meta=" << pf->meta.size()
        << " body=" << pf->body.size() << std::endl;

    if (!(pf->h.stream == urpc::SETTINGS_STREAM && pf->h.method == urpc::SETTINGS_METHOD))
    {
        std::cout << "[server] first frame is not SETTINGS" << std::endl;
        co_return false;
    }

    urpc::UrpcHdr h{};
    h.type = static_cast<uint8_t>(urpc::MsgType::RESPONSE);
    h.stream = urpc::SETTINGS_STREAM;
    h.method = urpc::SETTINGS_METHOD;
    h.flags = urpc::derive_transport_flags(urpc::TransportMode::RAW);

    std::string ack = urpc::make_frame(h, {}, {});
    std::cout << "[server] send SETTINGS-ACK (" << ack.size() << "B)" << std::endl;
    co_await rw.write_all(urpc::as_bytes(ack));
    co_return true;
}

task::Awaitable<void> echo_svc(net::TCPServerSocket& srv)
{
    std::cout << "echo_svc" << std::endl;
    urpc::ServerRouter<urpc::RawTransport<urpc::SocketRW>> router;

    router.register_unary_sync<EchoReq, EchoResp>("Echo", [](EchoReq r)
    {
        std::cout << "[server] EchoReq: " << r.text << std::endl;
        return EchoResp{.text = "echo: " + r.text};
    });

    router.register_unary<MulReq, MulResp>("Mul", [](MulReq r)-> task::Awaitable<MulResp>
    {
        std::cout << "[server] MulReq: " << r.a << " * " << r.b << std::endl;
        co_return MulResp{.v = r.a * r.b};
    });

    for (;;)
    {
        auto cli = co_await srv.async_accept();
        if (!cli) continue;

        std::cout << "[server] accepted" << std::endl;
        urpc::SocketRW rw(std::move(*cli));

        if (!(co_await handshake_raw(rw)))
        {
            std::cout << "[server] handshake failed" << std::endl;
            continue;
        }

        std::cout << "[server] hand over to router" << std::endl;
        system::co_spawn(router.serve_connection(std::move(rw), urpc::TransportMode::RAW));
    }
}

task::Awaitable<void> run_client()
{
    std::cout << "run_client" << std::endl;

    using namespace std::chrono_literals;
    co_await system::this_coroutine::sleep_for(150ms); // till server will be initialized

    net::TCPClientSocket sock;
    {
        auto rc = co_await sock.async_connect("127.0.0.1", "7001");
        if (rc.has_value())
        {
            std::cout << "[client] connect failed\n";
            co_return;
        }
        std::cout << "[client] connected" << std::endl;
    }

    urpc::SocketRW rw(std::move(sock));
    auto ch = urpc::make_raw_channel(std::move(rw));

    urpc::UrpcSettingsBuilder sb;
    sb.mode(urpc::TransportMode::RAW).alpn("urpcv1").endpoint("127.0.0.1", 7001, "tcp");

    bool ok = co_await ch.open(sb);
    std::cout << "[client] open=" << (ok ? "ok" : "fail") << std::endl;
    if (!ok) co_return;

    if (auto r = co_await ch.unary_by_name<EchoReq, EchoResp>("Echo", EchoReq{.text = "hi"}))
        std::cout << "[client] EchoResp: " << r->text << std::endl;
    else
        std::cout << "[client] Echo failed" << std::endl;

    if (auto r2 = co_await ch.unary_by_name<MulReq, MulResp>("Mul", MulReq{.a = 6, .b = 7}))
        std::cout << "[client] MulResp: " << r2->v << std::endl;
    else
        std::cout << "[client] Mul failed" << std::endl;

    co_return;
}

int main()
{
    usub::Uvent uvent(4);

    net::TCPServerSocket srv{"0.0.0.0", 7001};

    system::co_spawn(echo_svc(srv));
    system::co_spawn(run_client());

    uvent.run();
    return 0;
}
