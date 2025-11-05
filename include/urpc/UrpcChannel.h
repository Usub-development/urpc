#ifndef URPC_CHANNEL_H
#define URPC_CHANNEL_H

#include <memory>
#include <string>
#include <optional>
#include <cstdint>
#include <atomic>
#include <utility>
#include <variant>
#include <deque>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <chrono>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "UrpcSettings.h"
#include "UrpcSettingsIO.h"
#include "UrpcSettingsBuilder.h"
#include "Interceptors.h"
#include "UrpcOptions.h"
#include "uvent/Uvent.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

namespace urpc
{
    template <TransportLike T>
    class Channel
    {
    public:
        explicit Channel(T transport) : tr_(std::move(transport))
        {
        }

        [[nodiscard]] bool alive() const noexcept { return tr_.alive(); }
        [[nodiscard]] int native_handle() const noexcept { return tr_.native_handle(); }
        [[nodiscard]] uint8_t transport_bits() const noexcept { return tr_.transport_bits(); }

        usub::uvent::task::Awaitable<bool> open(const UrpcSettingsBuilder& builder)
        {
            tr_.set_coalescing({32 * 1024, 1});

            if (!(co_await builder.send(tr_))) co_return false;

            auto pf = co_await tr_.recv_frame();
            if (!pf) co_return false;
            if (!validate_first_settings(*pf, matches_tls(), matches_mtls())) co_return false;

            if (!pf->meta.empty())
            {
                Buf b{pf->meta};
                UrpcSettingsMeta meta{};
                if (decode(b, meta)) peer_settings_ = std::move(meta);
            }
            opened_ = true;

            if (!pump_started_.exchange(true)) usub::uvent::system::co_spawn(pump_rx());
            if (!ka_started_.exchange(true)) usub::uvent::system::co_spawn(ping_loop());

            co_return true;
        }

        // ================= High-level facade: unary =================
        template <class Req, class Resp = std::monostate>
        usub::uvent::task::Awaitable<std::variant<Resp, RpcError>>
        send(std::string_view name, const Req& req, MetaOpts opts = {})
        {
            const uint64_t mid = method_id(name);
            co_return co_await unary_with_opts<Req, std::monostate, Resp, std::monostate>(
                mid, std::monostate{}, req, nullptr, opts);
        }

        template <class Req, class Resp>
        usub::uvent::task::Awaitable<std::optional<Resp>>
        unary_by_name(std::string_view name, const Req& req, MetaOpts opts = {})
        {
            auto v = co_await send<Req, Resp>(name, req, opts);
            if (std::holds_alternative<Resp>(v)) co_return std::get<Resp>(v);
            co_return std::nullopt;
        }

        template <class Req, class MetaIn = std::monostate, class Resp = std::monostate, class MetaOut = std::monostate>
        usub::uvent::task::Awaitable<std::variant<Resp, RpcError>>
        unary_with_opts(uint64_t method, const MetaIn& meta_in, const Req& req, MetaOut* out_meta, MetaOpts opts)
        {
            const uint32_t stream = next_stream_.fetch_add(1, std::memory_order_relaxed);
            const uint64_t cancel_id = opts.cancel_id ? opts.cancel_id : ((uint64_t(stream) << 32) ^ method);

            std::string meta_bin, body_bin;
            encode(meta_bin, meta_in);
            encode(body_bin, req);

            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = tr_.transport_bits() | (opts.comp != CompressionKind::NONE ? F_COMPRESSED : 0);
            h.stream = stream;
            h.method = method;
            h.timeout_ms = opts.timeout_ms;
            h.cancel_id = cancel_id;
            h.codec = uint8_t(opts.codec);
            h.comp = uint8_t(opts.comp);

            Pending p{};
            p.cancel_id = cancel_id;
            {
                std::scoped_lock lk(pend_m_);
                pending_.emplace(stream, &p);
            }

            apply_before_send(h, meta_bin, body_bin);

            std::string frame = make_frame(h, std::move(meta_bin), std::move(body_bin));
            if (!(co_await tr_.send_frame(std::move(frame))))
            {
                std::scoped_lock lk(pend_m_);
                pending_.erase(stream);
                co_return RpcError{StatusCode::TRANSPORT_ERROR, "send failed"};
            }
            if (hooks_.on_send) hooks_.on_send(method, stream);

            if (opts.timeout_ms)
            {
                usub::uvent::system::co_spawn(
                    [this, stream, ms = opts.timeout_ms]() -> usub::uvent::task::Awaitable<void>
                    {
                        co_await usub::uvent::system::this_coroutine::sleep_for(std::chrono::milliseconds(ms));
                        std::scoped_lock lk(pend_m_);
                        auto it = pending_.find(stream);
                        if (it != pending_.end() && !it->second->done)
                        {
                            it->second->timed_out = true;
                            UrpcHdr ch{};
                            ch.type = uint8_t(MsgType::CANCEL);
                            ch.flags = tr_.transport_bits();
                            ch.stream = stream;
                            ch.cancel_id = it->second->cancel_id;
                            std::string fr = make_frame(ch, {}, {});
                            usub::uvent::system::co_spawn(tr_.send_frame(std::move(fr)));
                            it->second->cv.notify_one();
                        }
                        co_return;
                    }());
            }

            co_await p.cv.wait();

            {
                std::scoped_lock lk(pend_m_);
                pending_.erase(stream);
            }
            if (p.timed_out) co_return RpcError{StatusCode::DEADLINE_EXCEEDED, "timeout"};
            if (p.err) co_return *p.err;
            if (out_meta)
            {
                Buf mb{p.meta};
                if (!decode(mb, *out_meta)) co_return RpcError{StatusCode::PROTOCOL_ERROR, "bad meta"};
            }
            Resp resp{};
            {
                Buf bb{p.body};
                if (!decode(bb, resp)) co_return RpcError{StatusCode::PROTOCOL_ERROR, "bad body"};
            }
            co_return resp;
        }

        // ================= Server-streaming (client side) =================
        template <class Req, class Resp, class OnMsg>
        usub::uvent::task::Awaitable<bool>
        server_streaming_by_name(std::string_view name, const Req& req, OnMsg on_msg, MetaOpts opts = {})
        {
            const uint64_t method = method_id(name);
            const uint32_t stream = next_stream_.fetch_add(1, std::memory_order_relaxed);
            const uint64_t cancel_id = opts.cancel_id ? opts.cancel_id : ((uint64_t(stream) << 32) ^ method);

            std::string meta_bin, body_bin;
            encode(meta_bin, std::monostate{});
            encode(body_bin, req);

            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = tr_.transport_bits() | (opts.comp != CompressionKind::NONE ? F_COMPRESSED : 0);
            h.stream = stream;
            h.method = method;
            h.timeout_ms = opts.timeout_ms;
            h.cancel_id = cancel_id;
            h.codec = uint8_t(opts.codec);
            h.comp = uint8_t(opts.comp);

            apply_before_send(h, meta_bin, body_bin);

            std::string frame = make_frame(h, std::move(meta_bin), std::move(body_bin));
            if (!(co_await tr_.send_frame(std::move(frame)))) co_return false;
            if (hooks_.on_send) hooks_.on_send(method, stream);

            for (;;)
            {
                auto pf0 = co_await tr_.recv_frame();
                if (!pf0) co_return false;

                apply_after_recv(*pf0);

                if (pf0->h.stream != stream)
                {
                    push_inbox(std::move(*pf0));
                    continue;
                }

                if (pf0->h.type == uint8_t(MsgType::ERROR))
                {
                    if (hooks_.on_error) hooks_.on_error(StatusCode::UNKNOWN);
                    co_return false;
                }
                if (pf0->h.type != uint8_t(MsgType::RESPONSE))
                    continue;

                Resp item{};
                {
                    Buf bb{pf0->body};
                    if (!decode(bb, item)) co_return false;
                }
                if (hooks_.on_recv)
                    hooks_.on_recv(pf0->h.method, pf0->h.stream,
                                   HDR_SIZE + pf0->meta.size() + pf0->body.size());

                if (!co_await on_msg(std::move(item))) co_return false;
                if (is_stream_last(pf0->h.flags)) break;
            }
            co_return true;
        }

        // ================= Utilities =================
        usub::uvent::task::Awaitable<bool> ping()
        {
            UrpcHdr h = make_ping(0);
            h.flags = tr_.transport_bits();
            std::string fr = make_frame(h, {}, {});
            co_return co_await tr_.send_frame(std::move(fr));
        }

        usub::uvent::task::Awaitable<bool> cancel(uint32_t stream, std::optional<uint64_t> cancel_id = std::nullopt)
        {
            UrpcHdr h{};
            h.type = uint8_t(MsgType::CANCEL);
            h.flags = tr_.transport_bits();
            h.stream = stream;
            if (cancel_id) h.cancel_id = *cancel_id;
            std::string fr = make_frame(h, {}, {});
            co_return co_await tr_.send_frame(std::move(fr));
        }

        bool enable_inbox(size_t capacity_pow2 = 1024, bool drop_oldest = true)
        {
            if (inbox_) return false;
            using usub::queue::concurrent::SPSCQueue;
            inbox_ = std::make_unique<SPSCQueue<ParsedFrame>>(capacity_pow2);
            drop_oldest_ = drop_oldest;
            ovf_count_.store(0, std::memory_order_relaxed);
            return true;
        }

        bool try_recv_inbox(ParsedFrame& out)
        {
            if (!inbox_) return false;
            return inbox_->try_dequeue(out);
        }

        void stop_inbox() noexcept { inbox_run_.store(false, std::memory_order_release); }
        uint64_t inbox_overflow_count() const noexcept { return ovf_count_.load(std::memory_order_relaxed); }

        usub::uvent::task::Awaitable<void> pump_inbox()
        {
            if (!pump_started_.exchange(true)) usub::uvent::system::co_spawn(pump_rx());
            co_return;
        }

        struct Hooks
        {
            std::function<void(uint64_t /*method*/, uint32_t /*stream*/)> on_send;
            std::function<void(uint64_t /*method*/, uint32_t /*stream*/, size_t /*bytes*/)> on_recv;
            std::function<void(StatusCode)> on_error;
            std::function<void()> on_keepalive;
        };

        void set_hooks(Hooks h) { hooks_ = std::move(h); }

        void add_interceptor(ClientInterceptor ix) { interceptors_.push_back(std::move(ix)); }
        void clear_interceptors() { interceptors_.clear(); }

        [[nodiscard]] const std::optional<UrpcSettingsMeta>& peer_settings() const noexcept { return peer_settings_; }

    private:
        struct AwaitCV
        {
            std::atomic<bool> ready{false};

            usub::uvent::task::Awaitable<void> wait()
            {
                for (;;)
                {
                    if (ready.load(std::memory_order_acquire)) break;
                    co_await usub::uvent::system::this_coroutine::sleep_for(std::chrono::microseconds(50));
                }
                co_return;
            }

            void notify_one() { ready.store(true, std::memory_order_release); }
        };

        struct Pending
        {
            uint64_t cancel_id{};
            bool timed_out{};
            bool done{};
            std::optional<RpcError> err;
            std::string meta, body;
            AwaitCV cv;
        };

        usub::uvent::task::Awaitable<void> pump_rx()
        {
            for (;;)
            {
                auto pf = co_await tr_.recv_frame();
                if (!pf) break;

                if (pf->h.type == uint8_t(MsgType::PING))
                {
                    UrpcHdr pong = make_pong(pf->h);
                    std::string fr = make_frame(pong, {}, {});
                    (void)co_await tr_.send_frame(std::move(fr));
                    continue;
                }
                if ((pf->h.flags & F_GOAWAY) != 0) break;

                Pending* p{};
                {
                    std::scoped_lock lk(pend_m_);
                    auto it = pending_.find(pf->h.stream);
                    if (it == pending_.end())
                    {
                        push_inbox(std::move(*pf));
                        continue;
                    }
                    p = it->second;
                }

                if (pf->h.type == uint8_t(MsgType::ERROR))
                {
                    Buf bb{pf->body};
                    RpcError e{};
                    if (bb.remaining() >= 2)
                    {
                        uint16_t code = (uint8_t(bb.p[bb.i]) | (uint16_t(uint8_t(bb.p[bb.i + 1])) << 8));
                        bb.i += 2;
                        std::string msg;
                        (void)get_bytes(bb, msg);
                        e.code = (StatusCode)code;
                        e.message = std::move(msg);
                    }
                    else e = {StatusCode::UNKNOWN, "error"};
                    p->err = std::move(e);
                    p->done = true;
                    p->cv.notify_one();
                    if (hooks_.on_error) hooks_.on_error(p->err->code);
                    continue;
                }

                if (pf->h.type != uint8_t(MsgType::RESPONSE))
                {
                    push_inbox(std::move(*pf));
                    continue;
                }

                p->meta = std::move(pf->meta);
                p->body = std::move(pf->body);
                p->done = true;
                p->cv.notify_one();
                if (hooks_.on_recv)
                    hooks_.on_recv(pf->h.method, pf->h.stream,
                                   HDR_SIZE + p->meta.size() + p->body.size());
            }
            co_return;
        }

        usub::uvent::task::Awaitable<void> ping_loop()
        {
            using namespace std::chrono_literals;
            while (alive())
            {
                co_await usub::uvent::system::this_coroutine::sleep_for(5s);
                if (!alive()) break;
                UrpcHdr h = make_ping(0);
                h.flags = tr_.transport_bits();
                std::string fr = make_frame(h, {}, {});
                (void)co_await tr_.send_frame(std::move(fr));
                if (hooks_.on_keepalive) hooks_.on_keepalive();
            }
            co_return;
        }

        bool matches_tls() const noexcept
        {
            const uint8_t tp = flags_get_transport(tr_.transport_bits());
            return tp == F_TP_TLS || tp == F_TP_MTLS;
        }

        bool matches_mtls() const noexcept { return flags_get_transport(tr_.transport_bits()) == F_TP_MTLS; }

        void apply_before_send(UrpcHdr& h, std::string& meta, std::string& body)
        {
            for (auto& ix : interceptors_) if (ix.before_send) ix.before_send(h, meta, body);
        }

        void apply_after_recv(ParsedFrame& pf)
        {
            for (auto& ix : interceptors_) if (ix.after_recv) ix.after_recv(pf);
        }

        void push_inbox(ParsedFrame&& pf)
        {
            if (!inbox_) return;
            if (inbox_->try_enqueue(std::move(pf))) return;

            if (drop_oldest_)
            {
                ParsedFrame junk;
                (void)inbox_->try_dequeue(junk);
                if (!inbox_->try_enqueue(std::move(pf)))
                    ovf_count_.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                ovf_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }

    private:
        T tr_;
        std::atomic<uint32_t> next_stream_{1};
        bool opened_{false};
        std::optional<UrpcSettingsMeta> peer_settings_{};

        std::unordered_map<uint32_t, Pending*> pending_;
        std::mutex pend_m_;
        std::atomic<bool> pump_started_{false};
        std::atomic<bool> ka_started_{false};

        std::vector<ClientInterceptor> interceptors_;
        Hooks hooks_{};

        std::unique_ptr<usub::queue::concurrent::SPSCQueue<ParsedFrame>> inbox_;
        std::atomic<bool> inbox_run_{true};
        std::atomic<uint64_t> ovf_count_{0};
        bool drop_oldest_{true};
    };

    // helpers to build channels
    template <RWLike RW>
    inline Channel<RawTransport<RW>> make_raw_channel(RW rw)
    {
        return Channel<RawTransport<RW>>{RawTransport<RW>(std::move(rw))};
    }

    template <RWLike RW>
    inline Channel<TlsTransport<RW>> make_tls_channel(RW rw, TransportMode m = TransportMode::TLS)
    {
        return Channel<TlsTransport<RW>>{TlsTransport<RW>(std::move(rw), m)};
    }
} // namespace urpc

#endif // URPC_CHANNEL_H