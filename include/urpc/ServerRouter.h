#ifndef URPC_SERVER_ROUTER_H
#define URPC_SERVER_ROUTER_H

#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <utility>
#include <optional>
#include <cstdint>
#include <iostream>
#include <type_traits>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "uvent/Uvent.h"

namespace urpc
{
    // ---------------- helpers: response/error headers ----------------
    inline UrpcHdr make_response_for(const UrpcHdr& req)
    {
        UrpcHdr h{};
        h.type = uint8_t(MsgType::RESPONSE);
        h.flags = uint8_t(req.flags & ~F_GOAWAY);
        h.stream = req.stream;
        h.method = req.method;
        h.cancel_id = req.cancel_id;
        h.codec = req.codec;
        h.comp = req.comp;
        return h;
    }

    inline UrpcHdr make_error_for(const UrpcHdr& req, StatusCode code, std::string_view msg, std::string& out_body)
    {
        UrpcHdr h{};
        h.type = uint8_t(MsgType::ERROR);
        h.flags = uint8_t(req.flags & ~F_GOAWAY);
        h.stream = req.stream;
        h.method = req.method;
        h.cancel_id = req.cancel_id;
        h.codec = req.codec;
        h.comp = req.comp;

        out_body.clear();
        out_body.reserve(2 + msg.size());
        const uint16_t c = static_cast<uint16_t>(code);
        out_body.push_back(static_cast<char>(c & 0xFF));
        out_body.push_back(static_cast<char>((c >> 8) & 0xFF));
        out_body.append(msg.data(), msg.size());
        return h;
    }

    template <TransportLike T>
    class ServerRouter
    {
    public:
        using Transport = T;

        ServerRouter() = default;

        // --------- unary (async) ----------
        template <class Req, class Resp, class Fn>
        void register_unary(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' id=" << mid << "\n";
            handlers_[mid] = [fn = std::move(fn)](Transport& tr, ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                Req req{};
                Buf bb{pf.body};
                if (!decode(bb, req))
                {
                    std::string body;
                    UrpcHdr eh = make_error_for(pf.h, StatusCode::PROTOCOL_ERROR, "bad body", body);
                    std::string fr = make_frame(eh, {}, std::move(body));
                    (void)co_await tr.send_frame(std::move(fr));
                    co_await tr.flush();
                    co_return;
                }

                Resp resp = co_await fn(std::move(req));
                std::string meta_bin, body_bin;
                encode(meta_bin, std::monostate{});
                encode(body_bin, resp);

                UrpcHdr rh = make_response_for(pf.h);
                std::string fr = make_frame(rh, std::move(meta_bin), std::move(body_bin));
                (void)co_await tr.send_frame(std::move(fr));
                co_await tr.flush();
                co_return;
            };
        }

        // --------- unary (sync handler) ----------
        template <class Req, class Resp, class Fn>
        void register_unary_sync(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' id=" << mid << "\n";
            handlers_[mid] = [fn = std::move(fn)](Transport& tr, ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                Req req{};
                Buf bb{pf.body};
                if (!decode(bb, req))
                {
                    std::string body;
                    UrpcHdr eh = make_error_for(pf.h, StatusCode::PROTOCOL_ERROR, "bad body", body);
                    std::string fr = make_frame(eh, {}, std::move(body));
                    (void)co_await tr.send_frame(std::move(fr));
                    co_await tr.flush();
                    co_return;
                }

                Resp resp = fn(std::move(req));
                std::string meta_bin, body_bin;
                encode(meta_bin, std::monostate{});
                encode(body_bin, resp);

                UrpcHdr rh = make_response_for(pf.h);
                std::string fr = make_frame(rh, std::move(meta_bin), std::move(body_bin));
                (void)co_await tr.send_frame(std::move(fr));
                co_await tr.flush();
                co_return;
            };
        }

        class StreamWriter
        {
        public:
            StreamWriter(Transport& tr, UrpcHdr base, uint64_t method, uint32_t stream)
                : tr_(tr), base_(base), method_(method), stream_(stream)
            {
            }

            template <class Resp>
            usub::uvent::task::Awaitable<bool> write(const Resp& item, bool last)
            {
                std::string meta_bin, body_bin;
                encode(meta_bin, std::monostate{});
                encode(body_bin, item);

                UrpcHdr rh = base_;
                rh.type = uint8_t(MsgType::RESPONSE);
                rh.method = method_;
                rh.stream = stream_;
                if (last) rh.flags = uint8_t(rh.flags | F_STREAM_LAST);

                std::string fr = make_frame(rh, std::move(meta_bin), std::move(body_bin));
                bool ok = co_await tr_.send_frame(std::move(fr));
                if (!ok) co_return false;
                co_await tr_.flush();
                co_return true;
            }

        private:
            Transport& tr_;
            UrpcHdr base_{};
            uint64_t method_{};
            uint32_t stream_{};
        };

        // ---------- server-streaming ----------
        template <class Req, class Resp, class Fn>
        void register_server_streaming(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' (srv-stream) id=" << mid << "\n";

            handlers_[mid] = [fn = std::move(fn), mid](Transport& tr,
                                                       ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                Req req{};
                Buf bb{pf.body};
                if (!decode(bb, req))
                {
                    std::string body;
                    UrpcHdr eh = make_error_for(pf.h, StatusCode::PROTOCOL_ERROR, "bad body", body);
                    std::string fr = make_frame(eh, {}, std::move(body));
                    (void)co_await tr.send_frame(std::move(fr));
                    co_await tr.flush();
                    co_return;
                }

                StreamWriter w{tr, pf.h, mid, pf.h.stream};
                co_await fn(std::move(req), w);
                co_return;
            };
        }

        // ---------- serve_connection ----------
        template <RWLike RW>
        usub::uvent::task::Awaitable<void> serve_connection(RW rw, TransportMode mode)
        {
            auto make_transport = [&]() -> Transport
            {
                if constexpr (requires(RW a, TransportMode m) { Transport(std::move(a), m); })
                {
                    return Transport(std::move(rw), mode);
                }
                else
                {
                    return Transport(std::move(rw));
                }
            };

            Transport tr = make_transport();
            tr.set_coalescing({0, 0});

            for (;;)
            {
                auto pf = co_await tr.recv_frame();
                if (!pf) break;

                if (pf->h.type == uint8_t(MsgType::PING))
                {
                    UrpcHdr pong = make_pong(pf->h);
                    std::string fr = make_frame(pong, {}, {});
                    (void)co_await tr.send_frame(std::move(fr));
                    co_await tr.flush();
                    continue;
                }

                auto it = handlers_.find(pf->h.method);
                if (it == handlers_.end())
                {
                    std::string body;
                    UrpcHdr eh = make_error_for(pf->h, StatusCode::UNIMPLEMENTED, "no such method", body);
                    std::string fr = make_frame(eh, {}, std::move(body));
                    (void)co_await tr.send_frame(std::move(fr));
                    co_await tr.flush();
                    continue;
                }

                co_await it->second(tr, std::move(*pf));
            }
            co_return;
        }

        usub::uvent::task::Awaitable<void> goaway(Transport& tr)
        {
            UrpcHdr h{};
            h.type = uint8_t(MsgType::GOAWAY);
            h.flags = uint8_t(tr.transport_bits() | F_GOAWAY);
            std::string fr = make_frame(h, {}, {});
            (void)co_await tr.send_frame(std::move(fr));
            co_await tr.flush();
            co_return;
        }

    private:
        using Handler = std::function<usub::uvent::task::Awaitable<void>(Transport&, ParsedFrame)>;
        std::unordered_map<uint64_t, Handler> handlers_;
    };
} // namespace urpc

#endif // URPC_SERVER_ROUTER_H