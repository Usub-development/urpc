// examples/echo_demo.cpp
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
#include <string>

using namespace usub::uvent;

// ===== Models =====
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

// ===== Raw handshake (client SETTINGS -> server ACK) =====
template <typename RW>
task::Awaitable<bool> handshake_raw(RW& rw)
{
    std::cout << "[server] wait SETTINGS...\n";
    urpc::Framer fr;
    fr.transport_bits = urpc::derive_transport_flags(urpc::TransportMode::RAW);

    auto pf = co_await urpc::recv_one(rw, fr);
    if (!pf) co_return false;

    std::cout << "[server] got frame: type=" << int(pf->h.type)
        << " stream=" << pf->h.stream
        << " method=" << pf->h.method
        << " meta=" << pf->meta.size()
        << " body=" << pf->body.size() << "\n";

    if (!(pf->h.stream == urpc::SETTINGS_STREAM && pf->h.method == urpc::SETTINGS_METHOD))
        co_return false;

    urpc::UrpcHdr h{};
    h.type = uint8_t(urpc::MsgType::RESPONSE);
    h.stream = urpc::SETTINGS_STREAM;
    h.method = urpc::SETTINGS_METHOD;
    h.flags = urpc::derive_transport_flags(urpc::TransportMode::RAW);

    std::string ack = urpc::make_frame(h, {}, {});
    std::cout << "[server] send SETTINGS-ACK (" << ack.size() << "B)\n";
    co_await rw.write_all(urpc::as_bytes(ack));
    co_return true;
}

// ===== Server =====
task::Awaitable<void> echo_svc(net::TCPServerSocket& srv)
{
    std::cout << "echo_svc\n";
    urpc::ServerRouter<urpc::RawTransport<urpc::SocketRW>> router;

    router.register_unary_sync<EchoReq, EchoResp>("Echo", [](EchoReq r)
    {
        std::cout << "[server] EchoReq: " << r.text << "\n";
        EchoResp resp{.text = "echo: " + r.text};
        std::cout << "[server] sending response: " << resp.text << "\n";
        return resp;
    });

    router.register_unary<MulReq, MulResp>("Mul",
                                           [](MulReq r) -> task::Awaitable<MulResp>
                                           {
                                               std::cout << "[server] MulReq: " << r.a << " * " << r.b << "\n";
                                               co_return MulResp{.v = r.a * r.b};
                                           });

    router.register_server_streaming<EchoReq, EchoResp>("EchoStream",
                                                        [](EchoReq r, auto& w) -> task::Awaitable<void>
                                                        {
                                                            using namespace std::chrono_literals;
                                                            for (int i = 1; i <= 3; ++i)
                                                            {
                                                                bool last = (i == 3);
                                                                (void)co_await w.write(EchoResp{
                                                                    .text = "chunk#" + std::to_string(i) + " for " +
                                                                    r.text
                                                                }, last);
                                                                co_await usub::uvent::system::this_coroutine::sleep_for(
                                                                    150ms);
                                                            }
                                                            co_return;
                                                        });

    for (;;)
    {
        auto cli = co_await srv.async_accept();
        if (!cli) continue;

        std::cout << "[server] accepted\n";
        urpc::SocketRW rw(std::move(*cli));

        if (!(co_await handshake_raw(rw)))
        {
            std::cout << "[server] handshake failed\n";
            continue;
        }
        std::cout << "[server] hand over to router\n";
        system::co_spawn(router.serve_connection(std::move(rw), urpc::TransportMode::RAW));
    }
}

// ===== Client helpers =====
template <class ChannelT>
task::Awaitable<void> keepalive_loop(ChannelT& ch, int times, std::chrono::milliseconds period)
{
    using usub::uvent::system::this_coroutine::sleep_for;
    for (int i = 0; i < times; ++i)
    {
        bool pong = co_await ch.ping();
        std::cout << "[client] ping -> " << (pong ? "pong" : "fail") << "\n";
        co_await sleep_for(period);
    }
    co_return;
}

template <class ChannelT>
task::Awaitable<void> echo_stream_consumer(ChannelT& ch, std::string who)
{
    bool ok = co_await ch.template server_streaming_by_name<EchoReq, EchoResp>(
        "EchoStream", EchoReq{.text = std::move(who)},
        [](EchoResp item) -> task::Awaitable<bool>
        {
            std::cout << "[client] chunk: " << item.text << "\n";
            co_return true; // keep reading
        });

    std::cout << "[client] stream done: " << (ok ? "ok" : "fail") << "\n";
    co_return;
}

// Доп. демонстрация interceptors
static urpc::ClientInterceptor make_trace_ix()
{
    urpc::ClientInterceptor ix;
    ix.before_send = [](urpc::UrpcHdr& h, std::string&, std::string&)
    {
        // Пример: метим приоритет в flags/priority (если бы он писалcя в hdr)
        (void)h;
        std::cout << "[client/ix] before_send method=" << h.method
            << " stream=" << h.stream << "\n";
    };
    ix.after_recv = [](urpc::ParsedFrame& pf)
    {
        std::cout << "[client/ix] after_recv type=" << int(pf.h.type)
            << " stream=" << pf.h.stream
            << " bytes=" << (urpc::HDR_SIZE + pf.meta.size() + pf.body.size()) << "\n";
    };
    return ix;
}

// ===== Client =====
task::Awaitable<void> run_client()
{
    using namespace std::chrono_literals;
    std::cout << "run_client\n";

    co_await system::this_coroutine::sleep_for(150ms); // чуть подождать сервер

    net::TCPClientSocket sock;
    {
        auto rc = co_await sock.async_connect("127.0.0.1", "7001");
        if (rc.has_value())
        {
            std::cout << "[client] connect failed\n";
            co_return;
        }
        std::cout << "[client] connected\n";
    }

    urpc::SocketRW rw(std::move(sock));
    auto ch = urpc::make_raw_channel<urpc::SocketRW>(std::move(rw));

    urpc::UrpcSettingsBuilder sb;
    sb.mode(urpc::TransportMode::RAW).alpn("urpcv1").endpoint("127.0.0.1", 7001, "tcp");

    ch.set_hooks({
        .on_send = [](uint64_t m, uint32_t s) { std::cout << "[client] send m=" << m << " s=" << s << "\n"; },
        .on_recv = [](uint64_t m, uint32_t s, size_t b)
        {
            std::cout << "[client] recv m=" << m << " s=" << s << " bytes=" << b << "\n";
        },
        .on_error = [](urpc::StatusCode c) { std::cout << "[client] err=" << (int)c << "\n"; },
        .on_keepalive = []() { std::cout << "[client] keepalive\n"; }
    });
    ch.add_interceptor(make_trace_ix());

    bool ok = co_await ch.open(sb);
    std::cout << "[client] open=" << (ok ? "ok" : "fail") << "\n";
    if (!ok) co_return;

    // Echo
    if (auto r = co_await ch.template unary_by_name<EchoReq, EchoResp>("Echo", EchoReq{.text = "hi"}))
        std::cout << "[client] EchoResp: " << r->text << "\n";
    else
        std::cout << "[client] Echo failed\n";

    co_await system::this_coroutine::sleep_for(50ms);

    // Mul
    if (auto r2 = co_await ch.template unary_by_name<MulReq, MulResp>("Mul", MulReq{.a = 6, .b = 7}))
        std::cout << "[client] MulResp: " << r2->v << "\n";
    else
        std::cout << "[client] Mul failed\n";

    // keepalive
    co_await keepalive_loop(ch, 3, 250ms);

    // server streaming
    co_await echo_stream_consumer(ch, "World");

    // graceful close (GOAWAY) с reason
    co_await ch.goaway("client shutdown");
    std::cout << "[client] sent GOAWAY\n";

    co_return;
}

// ===== main =====
int main()
{
    usub::Uvent uvent(4);

    net::TCPServerSocket srv{"0.0.0.0", 7001};
    system::co_spawn(echo_svc(srv));
    system::co_spawn(run_client());

    uvent.run();
    return 0;
}
