#ifndef URPCCHANNEL_H
#define URPCCHANNEL_H

#include <string>
#include <optional>
#include <cstdint>
#include <atomic>
#include <utility>
#include <variant>
#include <deque>
#include <memory>
#include <chrono>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "UrpcSettings.h"
#include "UrpcSettingsIO.h"
#include "UrpcSettingsBuilder.h"
#include "uvent/Uvent.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

namespace urpc
{
    struct CallOpts
    {
        std::optional<std::chrono::steady_clock::time_point> deadline;
        bool send_cancel_on_deadline{true};
    };

    template <TransportLike T>
    class Channel
    {
    public:
        explicit Channel(T transport) : tr_(std::move(transport))
        {
        }

        [[nodiscard]] bool alive() const noexcept { return this->tr_.alive(); }
        [[nodiscard]] int native_handle() const noexcept { return this->tr_.native_handle(); }
        [[nodiscard]] uint8_t transport_bits() const noexcept { return this->tr_.transport_bits(); }

        usub::uvent::task::Awaitable<bool> open(const UrpcSettingsBuilder& builder)
        {
            if (!(co_await builder.send(this->tr_))) co_return false;
            auto pf = co_await this->tr_.recv_frame();
            if (!pf) co_return false;
            if (!validate_first_settings(*pf, this->matches_tls(), this->matches_mtls())) co_return false;
            if (!pf->meta.empty())
            {
                Buf b{pf->meta};
                UrpcSettingsMeta meta{};
                if (decode(b, meta)) this->peer_settings_ = std::move(meta);
            }
            this->opened_ = true;
            co_return true;
        }

        template <class Req, class Resp>
        usub::uvent::task::Awaitable<std::optional<Resp>> unary_by_name(std::string_view name, const Req& req,
                                                                        const CallOpts* opts = nullptr)
        {
            co_return co_await this->unary<Req, std::monostate, Resp, std::monostate>(
                method_id(name), std::monostate{}, req, nullptr, opts);
        }

        template <class Req, class MetaIn = std::monostate, class Resp = std::monostate, class MetaOut = std::monostate>
        usub::uvent::task::Awaitable<std::optional<Resp>>
        unary(uint64_t method, const MetaIn& meta_in, const Req& req, MetaOut* out_meta, const CallOpts* opts = nullptr)
        {
            if (this->busy_.exchange(true, std::memory_order_acq_rel)) co_return std::nullopt;
            const uint32_t stream = this->next_stream_.fetch_add(1, std::memory_order_relaxed);

            std::string meta_bin, body_bin;
            encode(meta_bin, meta_in);
            encode(body_bin, req);

            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::REQUEST);
            h.flags = this->tr_.transport_bits();
            h.stream = stream;
            h.method = method;

            std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
            if (opts && opts->deadline.has_value())
            {
                auto deadline = *opts->deadline;
                usub::uvent::system::co_spawn(
                    [this, stream, done, opts, deadline]() -> usub::uvent::task::Awaitable<void>
                    {
                        auto now = std::chrono::steady_clock::now();
                        if (deadline > now) co_await usub::uvent::system::this_coroutine::sleep_for(deadline - now);
                        if (!done->load(std::memory_order_acquire) && opts->send_cancel_on_deadline) (void)co_await this
                            ->cancel(stream);
                        co_return;
                    }());
            }

            std::string frame = make_frame(h, std::move(meta_bin), std::move(body_bin));
            if (!(co_await this->tr_.send_frame(std::move(frame))))
            {
                this->busy_.store(false, std::memory_order_release);
                if (done) done->store(true, std::memory_order_release);
                co_return std::nullopt;
            }

            auto pfopt = co_await this->tr_.recv_frame();
            if (done) done->store(true, std::memory_order_release);
            this->busy_.store(false, std::memory_order_release);
            if (!pfopt) co_return std::nullopt;

            const auto& pf = *pfopt;
            if (pf.h.stream != stream) co_return std::nullopt;
            if (pf.h.type == static_cast<uint8_t>(MsgType::ERROR)) co_return std::nullopt;
            if (pf.h.type != static_cast<uint8_t>(MsgType::RESPONSE)) co_return std::nullopt;

            if (out_meta)
            {
                Buf mb{pf.meta};
                if (!decode(mb, *out_meta)) co_return std::nullopt;
            }

            Resp resp{};
            Buf bb{pf.body};
            if (!decode(bb, resp)) co_return std::nullopt;
            co_return resp;
        }

        struct Stream
        {
            T* tr{};
            uint32_t stream{};
            uint64_t method{};
            uint32_t out_credit{16};
            uint32_t in_credit{16};
        };

        usub::uvent::task::Awaitable<std::optional<Stream>>
        stream_open(uint64_t method, uint32_t init_in_credit = 16, uint32_t init_out_credit = 16)
        {
            const uint32_t stream = this->next_stream_.fetch_add(1, std::memory_order_relaxed);
            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = this->tr_.transport_bits();
            h.stream = stream;
            h.method = method;
            std::string frame = make_frame(h, {}, {});
            if (!(co_await this->tr_.send_frame(std::move(frame)))) co_return std::nullopt;
            Stream s{&this->tr_, stream, method, init_out_credit, init_in_credit};
            co_return s;
        }

        static usub::uvent::task::Awaitable<bool>
        stream_send(Stream& s, std::string meta, std::string body, bool last)
        {
            if (s.out_credit == 0) co_return false;
            --s.out_credit;
            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = s.tr->transport_bits();
            if (last) h.flags |= F_STREAM_LAST;
            h.stream = s.stream;
            h.method = s.method;
            std::string frame = make_frame(h, std::move(meta), std::move(body));
            co_return co_await s.tr->send_frame(std::move(frame));
        }

        static usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
        stream_recv(Stream& s)
        {
            auto pf = co_await s.tr->recv_frame();
            if (!pf) co_return std::nullopt;
            if (pf->h.stream != s.stream) co_return std::nullopt;
            co_return pf;
        }

        static usub::uvent::task::Awaitable<bool>
        stream_grant_credit(Stream& s, uint32_t credit)
        {
            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = s.tr->transport_bits() | F_FLOW_CREDIT;
            h.stream = s.stream;
            h.method = s.method;
            std::string body;
            put_varu(body, credit);
            std::string frame = make_frame(h, {}, std::move(body));
            co_return co_await s.tr->send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<bool> ping()
        {
            UrpcHdr h = make_ping(0);
            h.flags = this->tr_.transport_bits();
            std::string frame = make_frame(h, {}, {});
            if (!(co_await this->tr_.send_frame(std::move(frame)))) co_return false;
            auto pf = co_await this->tr_.recv_frame();
            if (!pf) co_return false;
            co_return pf->h.type == uint8_t(MsgType::PONG);
        }

        usub::uvent::task::Awaitable<bool> cancel(uint32_t stream)
        {
            UrpcHdr h{};
            h.type = uint8_t(MsgType::CANCEL);
            h.flags = this->tr_.transport_bits();
            h.stream = stream;
            h.method = 0;
            std::string frame = make_frame(h, {}, {});
            co_return co_await this->tr_.send_frame(std::move(frame));
        }

        [[nodiscard]] const std::optional<UrpcSettingsMeta>& peer_settings() const noexcept
        {
            return this->peer_settings_;
        }

        bool enable_inbox(size_t capacity_pow2 = 1024, bool drop_oldest = true)
        {
            if (this->inbox_) return false;
            using usub::queue::concurrent::SPSCQueue;
            this->inbox_.reset(new SPSCQueue<ParsedFrame>(capacity_pow2));
            this->drop_oldest_ = drop_oldest;
            this->ovf_count_.store(0, std::memory_order_relaxed);
            return true;
        }

        bool try_recv_inbox(ParsedFrame& out)
        {
            if (!this->inbox_) return false;
            return this->inbox_->try_dequeue(out);
        }

        uint64_t inbox_overflow_count() const noexcept
        {
            return this->ovf_count_.load(std::memory_order_relaxed);
        }

        void stop_inbox() noexcept
        {
            this->inbox_run_.store(false, std::memory_order_release);
        }

        usub::uvent::task::Awaitable<void> pump_inbox()
        {
            if (!this->inbox_) co_return;
            if (this->inbox_run_.exchange(true, std::memory_order_acq_rel)) co_return;

            for (;;)
            {
                if (!this->alive() || !this->inbox_run_.load(std::memory_order_acquire)) break;
                auto pf = co_await this->tr_.recv_frame();
                if (!pf) break;

                if (pf->h.type == uint8_t(MsgType::PING))
                {
                    UrpcHdr pong = make_pong(pf->h);
                    std::string frame = make_frame(pong, {}, {});
                    (void)co_await this->tr_.send_frame(std::move(frame));
                    continue;
                }

                if (!this->inbox_->try_enqueue(std::move(*pf)))
                {
                    if (this->drop_oldest_)
                    {
                        ParsedFrame junk;
                        (void)this->inbox_->try_dequeue(junk);
                        if (!this->inbox_->try_enqueue(std::move(*pf))) this->ovf_count_.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    else
                    {
                        this->ovf_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            this->inbox_run_.store(false, std::memory_order_release);
            co_return;
        }

    private:
        bool matches_tls() const noexcept
        {
            const uint8_t tp = flags_get_transport(this->tr_.transport_bits());
            return tp == F_TP_TLS || tp == F_TP_MTLS;
        }

        bool matches_mtls() const noexcept
        {
            return flags_get_transport(this->tr_.transport_bits()) == F_TP_MTLS;
        }

    private:
        T tr_;
        std::atomic<uint32_t> next_stream_{1};
        std::atomic<bool> busy_{false};
        bool opened_{false};
        std::optional<UrpcSettingsMeta> peer_settings_{};
        std::unique_ptr<usub::queue::concurrent::SPSCQueue<ParsedFrame>> inbox_;
        std::atomic<bool> inbox_run_{false};
        std::atomic<uint64_t> ovf_count_{0};
        bool drop_oldest_{true};
    };

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

#endif // URPCCHANNEL_H