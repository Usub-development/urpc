#ifndef URPCCHANNEL_H
#define URPCCHANNEL_H

#include <string>
#include <optional>
#include <cstdint>
#include <atomic>
#include <utility>
#include <variant>
#include <deque>
#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "UrpcSettings.h"
#include "UrpcSettingsIO.h"
#include "UrpcSettingsBuilder.h"

namespace urpc
{
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
            if (!validate_first_settings(*pf, this->matches_tls(), this->matches_mtls()))
                co_return false;

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
        usub::uvent::task::Awaitable<std::optional<Resp>>
        unary_by_name(std::string_view name, const Req& req)
        {
            return unary<Req, std::monostate, Resp, std::monostate>(method_id(name), std::monostate{}, req, nullptr);
        }

        template <class Req, class MetaIn = std::monostate, class Resp = std::monostate, class MetaOut = std::monostate>
        usub::uvent::task::Awaitable<std::optional<Resp>>
        unary(uint64_t method, const MetaIn& meta_in, const Req& req, MetaOut* out_meta)
        {
            if (this->busy_) co_return std::nullopt;
            this->busy_ = true;

            const uint32_t stream = this->next_stream_++;

            std::string meta_bin, body_bin;
            encode(meta_bin, meta_in);
            encode(body_bin, req);

            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::REQUEST);
            h.flags = this->tr_.transport_bits();
            h.stream = stream;
            h.method = method;

            std::string frame = make_frame(h, std::move(meta_bin), std::move(body_bin));
            if (!(co_await this->tr_.send_frame(std::move(frame))))
            {
                this->busy_ = false;
                co_return std::nullopt;
            }

            auto pfopt = co_await this->tr_.recv_frame();
            this->busy_ = false;
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

        // ------------------ streaming API ------------------

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
            const uint32_t stream = this->next_stream_++;

            UrpcHdr h{};
            h.type = uint8_t(MsgType::REQUEST);
            h.flags = this->tr_.transport_bits();
            h.stream = stream;
            h.method = method;

            std::string frame = make_frame(h, {}, {});
            if (!(co_await this->tr_.send_frame(std::move(frame))))
                co_return std::nullopt;

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

        [[nodiscard]] const std::optional<UrpcSettingsMeta>& peer_settings() const noexcept
        {
            return this->peer_settings_;
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
        bool opened_{false};
        bool busy_{false};
        std::optional<UrpcSettingsMeta> peer_settings_{};
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