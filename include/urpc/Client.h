#ifndef URPC_CLIENT_H
#define URPC_CLIENT_H

#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <utility>

#include "uvent/Uvent.h"

#include "urpc/Wire.h"
#include "urpc/Codec.h"
#include "urpc/Transport.h"
#include "urpc/UrpcSettings.h"
#include "urpc/UrpcSettingsIO.h"
#include "urpc/UrpcSettingsBuilder.h"
#include "urpc/Interceptors.h"
#include "urpc/UrpcOptions.h"
#include "urpc/UrpcChannel.h"

namespace urpc
{
    template <TransportLike T>
    class Client
    {
    public:
        explicit Client(T transport)
            : ch_(Channel<T>(std::move(transport)))
        {
        }

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        Client(Client&&) = default;
        Client& operator=(Client&&) = default;

        usub::uvent::task::Awaitable<bool> open(const UrpcSettingsBuilder& builder)
        {
            co_return co_await ch_.open(builder);
        }

        template <class Req, class Resp>
        usub::uvent::task::Awaitable<std::variant<Resp, RpcError>>
        send(std::string_view topic, const Req& req, MetaOpts opts = {})
        {
            co_return co_await ch_.send<Req, Resp>(topic, req, opts);
        }

        template <class Req, class MetaIn, class Resp, class MetaOut>
        usub::uvent::task::Awaitable<std::variant<Resp, RpcError>>
        send_with_meta(uint64_t method, const MetaIn& meta_in, const Req& req, MetaOut* out_meta, MetaOpts opts = {})
        {
            co_return co_await ch_.template unary_with_opts<Req, MetaIn, Resp, MetaOut>(
                method, meta_in, req, out_meta, opts);
        }

        template <class Req, class Resp, class OnMsg>
        usub::uvent::task::Awaitable<bool>
        server_streaming(std::string_view name, const Req& req, OnMsg on_msg)
        {
            co_return co_await ch_.template server_streaming_by_name<Req, Resp>(
                name, req, std::move(on_msg));
        }

        struct Stream
        {
            typename Channel<T>::Stream inner{};
        };

        usub::uvent::task::Awaitable<std::optional<Stream>>
        stream_open(std::string_view name, uint32_t init_in_credit = 16, uint32_t init_out_credit = 16)
        {
            const uint64_t method = method_id(name);
            auto s = co_await ch_.stream_open(method, init_in_credit, init_out_credit);
            if (!s) co_return std::nullopt;
            Stream w{};
            w.inner = *s;
            co_return w;
        }

        usub::uvent::task::Awaitable<bool>
        stream_send(Stream& s, std::string meta, std::string body, bool last)
        {
            co_return co_await Channel<T>::stream_send(s.inner, std::move(meta), std::move(body), last);
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
        stream_recv(Stream& s)
        {
            co_return co_await Channel<T>::stream_recv(s.inner);
        }

        usub::uvent::task::Awaitable<bool>
        stream_grant_credit(Stream& s, uint32_t credit)
        {
            co_return co_await Channel<T>::stream_grant_credit(s.inner, credit);
        }

        usub::uvent::task::Awaitable<bool> ping() { co_return co_await ch_.ping(); }
        usub::uvent::task::Awaitable<bool> cancel(uint32_t stream) { co_return co_await ch_.cancel(stream); }

        void set_hooks(typename Channel<T>::Hooks h) { ch_.set_hooks(std::move(h)); }
        void add_interceptor(ClientInterceptor ix) { ch_.add_interceptor(std::move(ix)); }
        void clear_interceptors() { ch_.clear_interceptors(); }

        bool alive() const noexcept { return ch_.alive(); }
        int native_handle() const noexcept { return ch_.native_handle(); }
        uint8_t transport_bits() const noexcept { return ch_.transport_bits(); }
        const std::optional<UrpcSettingsMeta>& peer_settings() const noexcept { return ch_.peer_settings(); }

        Channel<T>& channel() noexcept { return ch_; }

    private:
        Channel<T> ch_;
    };
} // namespace urpc

#endif // URPC_CLIENT_H