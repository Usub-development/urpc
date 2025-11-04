#ifndef URPCCHANNEL_H
#define URPCCHANNEL_H

#include <string>
#include <optional>
#include <cstdint>
#include <atomic>
#include <utility>
#include <variant>
#include <iostream>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "UrpcSettings.h"
#include "UrpcSettingsIO.h"
#include "UrpcSettingsBuilder.h"

namespace urpc
{
    struct RpcError
    {
        uint32_t code{0};
        std::string message;
    };

    template <TransportLike T>
    class Channel
    {
    public:
        explicit Channel(T transport)
            : tr_(std::move(transport))
        {
        }

        [[nodiscard]] bool alive() const noexcept { return this->tr_.alive(); }
        [[nodiscard]] int native_handle() const noexcept { return this->tr_.native_handle(); }
        [[nodiscard]] uint8_t transport_bits() const noexcept { return this->tr_.transport_bits(); }

        usub::uvent::task::Awaitable<bool> open(const UrpcSettingsBuilder& builder)
        {
            std::string meta_bin;
            encode(meta_bin, builder.meta());

            std::cout << "[client] sending SETTINGS (" << meta_bin.size() << " B)" << std::endl;
            if (!(co_await this->tr_.send_settings(std::move(meta_bin))))
            {
                std::cout << "[client] send_settings failed" << std::endl;
                co_return false;
            }

            auto pf = co_await this->tr_.recv_frame();
            if (!pf)
            {
                std::cout << "[client] recv SETTINGS ack failed" << std::endl;
                co_return false;
            }
            std::cout << "[client] got first frame: type=" << int(pf->h.type)
                << " stream=" << pf->h.stream
                << " method=" << pf->h.method
                << " meta=" << pf->meta.size() << " body=" << pf->body.size() << std::endl;

            if (!validate_first_settings(*pf, this->matches_tls(), this->matches_mtls()))
            {
                std::cout << "[client] SETTINGS ack validate failed" << std::endl;
                co_return false;
            }

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
        unary(uint64_t method, const Req& req)
        {
            return this->unary<Req, std::monostate, Resp, std::monostate>(
                method, std::monostate{}, req, nullptr);
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
            std::cout << "[client] -> REQUEST stream=" << stream << " method=" << method
                << " body=" << frame.size() << "B" << std::endl;

            if (!(co_await this->tr_.send_frame(std::move(frame))))
            {
                this->busy_ = false;
                std::cout << "[client] send_frame failed" << std::endl;
                co_return std::nullopt;
            }

            auto pfopt = co_await this->tr_.recv_frame();
            this->busy_ = false;
            if (!pfopt)
            {
                std::cout << "[client] recv_frame failed" << std::endl;
                co_return std::nullopt;
            }

            const auto& pf = *pfopt;
            std::cout << "[client] <- frame type=" << int(pf.h.type)
                << " stream=" << pf.h.stream
                << " method=" << pf.h.method
                << " meta=" << pf.meta.size()
                << " body=" << pf.body.size() << std::endl;

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

        [[nodiscard]] const std::optional<UrpcSettingsMeta>& peer_settings() const noexcept
        {
            return this->peer_settings_;
        }

        template <class Req, class Resp>
        usub::uvent::task::Awaitable<std::optional<Resp>>
        unary_by_name(std::string_view name, const Req& req)
        {
            const uint64_t m = method_id(name);
            co_return co_await this->unary<Req, Resp>(m, req);
        }

    private:
        bool matches_tls() const noexcept
        {
            const uint8_t tp = flags_get_transport(this->tr_.transport_bits());
            return tp == F_TP_TLS || tp == F_TP_MTLS;
        }

        bool matches_mtls() const noexcept
        {
            const uint8_t tp = flags_get_transport(this->tr_.transport_bits());
            return tp == F_TP_MTLS;
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