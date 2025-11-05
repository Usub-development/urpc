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
#include <memory>
#include <mutex>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "uvent/Uvent.h"

namespace urpc
{
    // helpers: response/error headers
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

        // ===== Credit state (per stream) =====
        struct CreditState
        {
            std::atomic<uint32_t> peer_in_credit{0};
            struct CV
            {
                std::atomic<bool> ready{false};

                usub::uvent::task::Awaitable<void> wait()
                {
                    while (!ready.load(std::memory_order_acquire))
                        co_await usub::uvent::system::this_coroutine::sleep_for(std::chrono::microseconds(50));
                    ready.store(false, std::memory_order_release);
                    co_return;
                }

                void notify() { ready.store(true, std::memory_order_release); }
            } cv;
        };

        // unary (async)
        template <class Req, class Resp, class Fn>
        void register_unary(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' id=" << mid << "\n";
            handlers_[mid] = [fn = std::move(fn), this](Transport& tr,
                                                        ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                if (pf.h.type == uint8_t(MsgType::CANCEL)) co_return; // ignore stray
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

        // unary (sync handler)
        template <class Req, class Resp, class Fn>
        void register_unary_sync(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' id=" << mid << "\n";
            handlers_[mid] = [fn = std::move(fn), this](Transport& tr,
                                                        ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                if (pf.h.type == uint8_t(MsgType::CANCEL)) co_return;
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
            StreamWriter(Transport& tr, UrpcHdr base, uint64_t method, uint32_t stream,
                         std::shared_ptr<CreditState> cs)
                : tr_(tr), base_(base), method_(method), stream_(stream), cs_(std::move(cs))
            {
            }

            template <class Resp>
            usub::uvent::task::Awaitable<bool> write(const Resp& item, bool last)
            {
                for (;;)
                {
                    uint32_t c = cs_->peer_in_credit.load(std::memory_order_acquire);
                    if (c > 0 && cs_->peer_in_credit.compare_exchange_weak(c, c - 1,
                                                                           std::memory_order_acq_rel,
                                                                           std::memory_order_relaxed))
                        break;
                    co_await cs_->cv.wait();
                }

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
            std::shared_ptr<CreditState> cs_;
        };

        // server-streaming
        template <class Req, class Resp, class Fn>
        void register_server_streaming(std::string name, Fn fn)
        {
            const uint64_t mid = method_id(name);
            std::cout << "[server] registered '" << name << "' (srv-stream) id=" << mid << "\n";

            handlers_[mid] = [this, fn = std::move(fn), mid](Transport& tr,
                                                             ParsedFrame pf) -> usub::uvent::task::Awaitable<void>
            {
                if (pf.h.type == uint8_t(MsgType::CANCEL)) co_return;
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

                std::shared_ptr<CreditState> cs;
                {
                    std::scoped_lock lk(cr_m_);
                    auto& slot = credits_[pf.h.stream];
                    if (!slot) slot = std::make_shared<CreditState>();
                    cs = slot;
                }
                if (is_credit_frame(pf.h.flags) && !pf.meta.empty())
                {
                    uint32_t delta{};
                    Buf mb{pf.meta};
                    if (get_credit_meta(mb, delta))
                    {
                        cs->peer_in_credit.fetch_add(delta, std::memory_order_acq_rel);
                        cs->cv.notify();
                    }
                }

                StreamWriter w{tr, pf.h, mid, pf.h.stream, cs};
                co_await fn(std::move(req), w);

                {
                    std::scoped_lock lk(cr_m_);
                    credits_.erase(pf.h.stream);
                }
                co_return;
            };
        }

        // serve_connection
        template <RWLike RW>
        usub::uvent::task::Awaitable<void> serve_connection(RW rw, TransportMode mode)
        {
            auto make_transport = [&]() -> Transport
            {
                if constexpr (requires(RW a, TransportMode m) { Transport(std::move(a), m); })
                    return Transport(std::move(rw), mode);
                else
                    return Transport(std::move(rw));
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

                if ((pf->h.flags & F_GOAWAY) != 0) break;

                if (is_credit_frame(pf->h.flags) && !pf->meta.empty())
                {
                    uint32_t delta{};
                    Buf mb{pf->meta};
                    if (get_credit_meta(mb, delta))
                    {
                        std::shared_ptr<CreditState> cs;
                        {
                            std::scoped_lock lk(cr_m_);
                            auto it = credits_.find(pf->h.stream);
                            if (it != credits_.end()) cs = it->second;
                        }
                        if (cs)
                        {
                            cs->peer_in_credit.fetch_add(delta, std::memory_order_acq_rel);
                            cs->cv.notify();
                            continue;
                        }
                    }
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

        std::mutex cr_m_;
        std::unordered_map<uint32_t, std::shared_ptr<CreditState>> credits_;
    };
} // namespace urpc

#endif // URPC_SERVER_ROUTER_H