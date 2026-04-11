#include <urpc/connection/RPCConnection.h>
#include <urpc/crypto/AppCrypto.h>
#include <urpc/transport/TlsRpcStream.h>

namespace urpc
{
    using namespace usub::uvent;

    static const AppCipherContext* get_cipher_for_stream(IRpcStream* s)
    {
        auto* tls = dynamic_cast<TlsRpcStream*>(s);
        if (!tls)
            return nullptr;
        return tls->app_cipher();
    }

    static bool stream_is_tls(IRpcStream* s)
    {
        return dynamic_cast<TlsRpcStream*>(s) != nullptr;
    }

    static uint16_t build_security_flags(IRpcStream* stream,
                                         const RpcPeerIdentity* peer)
    {
        uint16_t flags = 0;
        if (stream_is_tls(stream))
            flags |= FLAG_TLS;
        if (peer && peer->authenticated)
            flags |= FLAG_MTLS;
        return flags;
    }

    static task::Awaitable<bool> read_exact(
        IRpcStream& stream,
        utils::DynamicBuffer& buf,
        std::size_t expected)
    {
        buf.clear();
        buf.reserve(expected);

#if URPC_LOGS
        usub::ulog::debug(
            "RpcConnection::read_exact: cur={} expected={}",
            buf.size(), expected);
#endif

        while (buf.size() < expected) {
            const std::size_t want = expected - buf.size();
            const ssize_t r = co_await stream.async_read(buf, want);
            if (r <= 0) co_return false;
        }
        co_return true;
#if URPC_LOGS
        usub::ulog::debug(
            "RpcConnection::read_exact: size={}",
            buf.size());
#endif
    }

    RpcConnection::RpcConnection(std::shared_ptr<IRpcStream> stream,
                                 RpcMethodRegistry& registry)
        : stream_(std::move(stream))
          , registry_(registry)
          , on_cancel_()
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection ctor: stream_={}",
            static_cast<void*>(this->stream_.get()));
#endif
    }

    RpcConnection::RpcConnection(std::shared_ptr<IRpcStream> stream,
                                 RpcMethodRegistry& registry,
                                 RpcCancelCallback on_cancel)
        : stream_(std::move(stream))
          , registry_(registry)
          , on_cancel_(std::move(on_cancel))
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection ctor (with cancel cb): stream_={} cb_set={}",
            static_cast<void*>(this->stream_.get()),
            static_cast<bool>(this->on_cancel_));
#endif
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::run_detached(std::shared_ptr<RpcConnection> self)
    {
        if (!self)
            co_return;
#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection::run_detached: self={}",
            static_cast<void*>(self.get()));
#endif
        co_await self->loop();
#if URPC_LOGS
        usub::ulog::warn(
            "RpcConnection::run_detached: finished self={}",
            static_cast<void*>(self.get()));
#endif
        co_return;
    }

    usub::uvent::task::Awaitable<void> RpcConnection::loop()
    {
        if (!this->stream_)
        {
#if URPC_LOGS
            usub::ulog::error("RpcConnection::loop: stream_ is null");
#endif
            co_return;
        }

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection::loop: started, this={} stream_={}",
            static_cast<void*>(this),
            static_cast<void*>(this->stream_.get()));
#endif

        for (;;)
        {
            if (!this->stream_)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: stream_ became null, exiting");
#endif
                break;
            }

            utils::DynamicBuffer head;
#if URPC_LOGS
            usub::ulog::debug(
                "RpcConnection::loop: reading header {} bytes",
                RpcFrameHeaderSize);
#endif
            const bool ok_hdr = co_await read_exact(
                *this->stream_, head, RpcFrameHeaderSize);
            if (!ok_hdr)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: header read_exact failed, "
                    "shutting down stream");
#endif
                this->stream_->shutdown();
                break;
            }

            if (head.size() != RpcFrameHeaderSize)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: header size={} != {}, dropping",
                    head.size(), RpcFrameHeaderSize);
#endif
                this->stream_->shutdown();
                break;
            }

            RpcFrameHeader hdr = parse_header(
                reinterpret_cast<const uint8_t*>(head.data()));
#if URPC_LOGS
            usub::ulog::debug(
                "RpcConnection::loop: parsed header magic={} ver={} type={} "
                "sid={} len={} flags=0x{:x}",
                static_cast<unsigned>(hdr.magic),
                static_cast<unsigned>(hdr.version),
                static_cast<unsigned>(hdr.type),
                hdr.stream_id,
                hdr.length,
                hdr.flags);
#endif
            if (hdr.magic != 0x55525043 || hdr.version != 1)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: invalid header magic/ver, dropping");
#endif
                this->stream_->shutdown();
                break;
            }

            if (hdr.length > kMaxFrameBodyLength)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: frame body length {} exceeds "
                    "kMaxFrameBodyLength {}, dropping connection",
                    static_cast<unsigned long long>(hdr.length),
                    static_cast<unsigned long long>(kMaxFrameBodyLength));
#endif
                this->stream_->shutdown();
                break;
            }

            RpcFrame frame;
            frame.header = hdr;

            if (hdr.length > 0)
            {
                const std::size_t len = hdr.length;
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcConnection::loop: reading payload {} bytes", len);
#endif
                const bool ok_body = co_await read_exact(
                    *this->stream_, frame.payload, len);

                if (!ok_body || frame.payload.size() != len)
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "RpcConnection::loop: payload read_exact failed, "
                        "size={} len={}",
                        frame.payload.size(), len);
#endif
                    this->stream_->shutdown();
                    break;
                }
            }
            else
            {
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcConnection::loop: zero-length payload");
#endif
            }

            FrameType ft = static_cast<FrameType>(frame.header.type);
#if URPC_LOGS
            usub::ulog::debug(
                "RpcConnection::loop: got frame type={} sid={} len={}",
                static_cast<int>(ft),
                frame.header.stream_id,
                frame.header.length);

            usub::ulog::info(
                "RpcConnection[{}]: got frame type={} sid={} len={}",
                static_cast<void*>(this),
                static_cast<int>(ft),
                frame.header.stream_id,
                frame.header.length);
#endif

            switch (ft)
            {
            case FrameType::Request:
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcConnection::loop: handling Request sid={} method_id={}",
                    frame.header.stream_id,
                    frame.header.method_id);
#endif
                usub::uvent::system::co_spawn(
                    RpcConnection::handle_request_detached(
                        this->shared_from_this(),
                        std::move(frame)));
                break;

            case FrameType::Cancel:
                co_await this->handle_cancel(std::move(frame));
                break;

            case FrameType::Ping:
                co_await this->handle_ping(std::move(frame));
                break;

            case FrameType::Response:
            case FrameType::Stream:
            case FrameType::Pong:
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: unexpected client->server frame "
                    "type={} sid={}, dropping connection",
                    static_cast<int>(ft),
                    frame.header.stream_id);
#endif
                this->stream_->shutdown();
                co_return;

            default:
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: unknown frame type={} sid={}, "
                    "dropping connection",
                    static_cast<int>(ft),
                    frame.header.stream_id);
#endif
                this->stream_->shutdown();
                co_return;
            }
        }

#if URPC_LOGS
        usub::ulog::warn("RpcConnection::loop: exiting");
#endif
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::locked_send(const RpcFrameHeader& hdr,
                               std::span<const uint8_t> body)
    {
        auto guard = co_await this->write_mutex_.lock();

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: locked_send type={} sid={} len={} flags=0x{:x}",
            static_cast<void*>(this),
            static_cast<unsigned>(hdr.type),
            hdr.stream_id,
            body.size(),
            hdr.flags);
#endif

        const bool ok = co_await send_frame(*this->stream_, hdr, body);
        if (!ok)
            this->stream_->shutdown();

#if URPC_LOGS
        usub::ulog::debug(
            "RpcConnection[{}]: locked_send finished type={} sid={} len={}",
            static_cast<void*>(this),
            static_cast<unsigned>(hdr.type),
            hdr.stream_id,
            body.size());
#endif

        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::send_response(RpcContext& ctx,
                                 std::span<const uint8_t> body)
    {
        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Response);
        hdr.flags = FLAG_END_STREAM;
        hdr.stream_id = ctx.stream_id;
        hdr.method_id = ctx.method_id;
        hdr.length = static_cast<uint32_t>(body.size());

        std::vector<uint8_t> enc_buf;
        std::span<const uint8_t> to_send = body;

        const AppCipherContext* cipher =
            get_cipher_for_stream(&ctx.stream);

        if (cipher && !body.empty())
        {
            bool ok = app_encrypt_gcm(*cipher, body, enc_buf);
            if (ok)
            {
                hdr.flags |= FLAG_ENCRYPTED;
                hdr.length = static_cast<uint32_t>(enc_buf.size());
                to_send = std::span<const uint8_t>{
                    enc_buf.data(),
                    enc_buf.size()
                };
#if URPC_LOGS
                usub::ulog::info(
                    "RpcConnection[{}]: encrypting Response mid={} sid={} "
                    "plain_len={} enc_len={}",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id,
                    body.size(),
                    enc_buf.size());
#endif
            }
            else
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcConnection[{}]: app_encrypt_gcm failed for Response "
                    "mid={} sid={}; failing closed (no plaintext fallback)",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id);
#endif
                this->stream_->shutdown();
                co_return;
            }
        }

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending Response mid={} sid={} len={} flags=0x{:x}",
            static_cast<void*>(this),
            hdr.method_id,
            hdr.stream_id,
            hdr.length,
            hdr.flags);
#endif

        co_await this->locked_send(hdr, to_send);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::send_simple_error(RpcContext& ctx,
                                     uint32_t error_code,
                                     std::string_view message,
                                     std::span<const uint8_t> details)
    {
        using urpc::host_to_be;

        const uint32_t code_be = host_to_be<uint32_t>(error_code);
        const uint32_t msg_len = static_cast<uint32_t>(message.size());
        const uint32_t msg_len_be = host_to_be<uint32_t>(msg_len);

        std::vector<uint8_t> buf;
        buf.reserve(8 + message.size() + details.size());

        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t*>(&code_be),
                   reinterpret_cast<const uint8_t*>(&code_be)
                   + sizeof(code_be));

        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t*>(&msg_len_be),
                   reinterpret_cast<const uint8_t*>(&msg_len_be)
                   + sizeof(msg_len_be));

        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t*>(message.data()),
                   reinterpret_cast<const uint8_t*>(message.data())
                   + message.size());

        if (!details.empty())
        {
            buf.insert(buf.end(), details.begin(), details.end());
        }

        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Response);
        hdr.flags = FLAG_END_STREAM | FLAG_ERROR;
        hdr.stream_id = ctx.stream_id;
        hdr.method_id = ctx.method_id;
        hdr.length = static_cast<uint32_t>(buf.size());

        std::vector<uint8_t> enc_buf;
        std::span<const uint8_t> to_send{buf.data(), buf.size()};

        const AppCipherContext* cipher =
            get_cipher_for_stream(&ctx.stream);

        if (cipher && !buf.empty())
        {
            bool ok = app_encrypt_gcm(*cipher,
                                      std::span<const uint8_t>{
                                          buf.data(),
                                          buf.size()
                                      },
                                      enc_buf);
            if (ok)
            {
                hdr.flags |= FLAG_ENCRYPTED;
                hdr.length = static_cast<uint32_t>(enc_buf.size());
                to_send = std::span<const uint8_t>{
                    enc_buf.data(),
                    enc_buf.size()
                };
#if URPC_LOGS
                usub::ulog::info(
                    "RpcConnection[{}]: encrypting ERROR mid={} sid={} "
                    "plain_len={} enc_len={} code={}",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id,
                    buf.size(),
                    enc_buf.size(),
                    error_code);
#endif
            }
            else
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcConnection[{}]: app_encrypt_gcm failed for ERROR "
                    "mid={} sid={} code={}; failing closed (no plaintext "
                    "fallback)",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id,
                    error_code);
#endif
                this->stream_->shutdown();
                co_return;
            }
        }

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending ERROR Response mid={} sid={} len={} code={} flags=0x{:x}",
            static_cast<void*>(this),
            hdr.method_id,
            hdr.stream_id,
            hdr.length,
            error_code,
            hdr.flags);
#endif

        co_await this->locked_send(hdr, to_send);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_request_detached(
        std::shared_ptr<RpcConnection> self,
        RpcFrame frame)
    {
        if (!self)
            co_return;
        co_await self->handle_request(std::move(frame));
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_request(RpcFrame frame)
    {
#if URPC_LOGS
        usub::ulog::info(
            "handle_request: sid={} mid={} len={} flags=0x{:x}",
            frame.header.stream_id,
            frame.header.method_id,
            frame.header.length,
            frame.header.flags);
#endif

        RpcHandlerPtr fn = this->registry_.find(frame.header.method_id);
        if (!fn)
        {
#if URPC_LOGS
            usub::ulog::error(
                "handle_request: no handler for mid={} sid={}",
                frame.header.method_id,
                frame.header.stream_id);
#endif

            RpcContext tmp{
                .stream = *this->stream_,
                .stream_id = frame.header.stream_id,
                .method_id = frame.header.method_id,
                .flags = frame.header.flags,
                .cancel_token = usub::uvent::sync::CancellationToken{},
                .peer = this->stream_->peer_identity(),
            };

            co_await this->send_simple_error(tmp, 404, "Unknown method");
            co_return;
        }

        auto src = std::make_shared<sync::CancellationSource>();
        {
            auto guard = co_await this->cancel_map_mutex_.lock();
            this->cancel_map_[frame.header.stream_id] = src;
        }

        RpcContext ctx{
            .stream = *this->stream_,
            .stream_id = frame.header.stream_id,
            .method_id = frame.header.method_id,
            .flags = frame.header.flags,
            .cancel_token = src->token(),
            .peer = this->stream_->peer_identity(),
        };

        std::span<const uint8_t> body{
            reinterpret_cast<const uint8_t*>(frame.payload.data()),
            frame.payload.size(),
        };

        std::vector<uint8_t> decrypted;
        const bool encrypted =
            (frame.header.flags & FLAG_ENCRYPTED) != 0;

        if (encrypted)
        {
            const AppCipherContext* cipher =
                get_cipher_for_stream(&ctx.stream);
            if (!cipher)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "handle_request: got encrypted payload but no cipher "
                    "available sid={} mid={}",
                    ctx.stream_id,
                    ctx.method_id);
#endif
                co_await this->send_simple_error(
                    ctx,
                    400,
                    "Encrypted payload but cipher not available");
                co_return;
            }

            bool ok = app_decrypt_gcm(
                *cipher,
                body,
                decrypted);
            if (!ok)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "handle_request: app_decrypt_gcm failed sid={} mid={}",
                    ctx.stream_id,
                    ctx.method_id);
#endif
                co_await this->send_simple_error(
                    ctx,
                    400,
                    "Invalid encrypted payload");
                co_return;
            }

            body = std::span<const uint8_t>{
                decrypted.data(),
                decrypted.size()
            };

#if URPC_LOGS
            usub::ulog::info(
                "handle_request: decrypted body sid={} mid={} "
                "enc_len={} plain_len={}",
                ctx.stream_id,
                ctx.method_id,
                frame.payload.size(),
                decrypted.size());
#endif
        }

#if URPC_LOGS
        usub::ulog::info(
            "handle_request: invoking handler mid={} sid={} body_size={}",
            ctx.method_id,
            ctx.stream_id,
            body.size());
#endif

        if (ctx.cancel_token.stop_requested())
        {
#if URPC_LOGS
            usub::ulog::info(
                "handle_request: cancel was already requested for "
                "sid={} mid={} before handler started; skipping handler "
                "invocation entirely",
                ctx.stream_id,
                ctx.method_id);
#endif
            {
                auto guard = co_await this->cancel_map_mutex_.lock();
                this->cancel_map_.erase(frame.header.stream_id);
            }

            if (this->on_cancel_)
            {
                RpcCancelEvent ev{
                    .stage                  = RpcCancelStage::BeforeHandler,
                    .stream_id              = ctx.stream_id,
                    .method_id              = ctx.method_id,
                    .dropped_response_bytes = 0,
                };
                this->on_cancel_(ev);
            }

            co_return;
        }

        std::vector<uint8_t> resp = co_await (*fn)(ctx, body);

#if URPC_LOGS
        usub::ulog::info(
            "handle_request: handler finished mid={} sid={} resp_size={}",
            ctx.method_id,
            ctx.stream_id,
            resp.size());
#endif

        {
            auto guard = co_await this->cancel_map_mutex_.lock();
            this->cancel_map_.erase(frame.header.stream_id);
        }

        if (ctx.cancel_token.stop_requested())
        {
#if URPC_LOGS
            usub::ulog::info(
                "handle_request: cancel was requested for sid={} mid={} "
                "while handler was running; dropping response "
                "(resp_size={} bytes saved)",
                ctx.stream_id,
                ctx.method_id,
                resp.size());
#endif
            if (this->on_cancel_)
            {
                RpcCancelEvent ev{
                    .stage                  = RpcCancelStage::AfterHandler,
                    .stream_id              = ctx.stream_id,
                    .method_id              = ctx.method_id,
                    .dropped_response_bytes = resp.size(),
                };
                this->on_cancel_(ev);
            }
            co_return;
        }

        std::span<const uint8_t> resp_span{resp.data(), resp.size()};

#if URPC_LOGS
        usub::ulog::info(
            "handle_request: sending response mid={} sid={} len={}",
            ctx.method_id,
            ctx.stream_id,
            resp_span.size());
#endif
        co_await this->send_response(ctx, resp_span);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_cancel(RpcFrame frame)
    {
#if URPC_LOGS
        usub::ulog::info(
            "handle_cancel: sid={}", frame.header.stream_id);
#endif
        std::shared_ptr<sync::CancellationSource> src;
        {
            auto guard = co_await this->cancel_map_mutex_.lock();
            auto it = this->cancel_map_.find(frame.header.stream_id);
            if (it != this->cancel_map_.end())
            {
                src = it->second;
                this->cancel_map_.erase(it);
            }
        }

        if (src)
        {
            src->request_cancel();
#if URPC_LOGS
            usub::ulog::info(
                "handle_cancel: requested cancel for sid={}",
                frame.header.stream_id);
#endif
        }
        else
        {
#if URPC_LOGS
            usub::ulog::warn(
                "handle_cancel: no cancel source for sid={}",
                frame.header.stream_id);
#endif
        }

        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_ping(RpcFrame frame)
    {
#if URPC_LOGS
        usub::ulog::info(
            "handle_ping: sid={} flags=0x{:x}",
            frame.header.stream_id,
            frame.header.flags);
#endif
        if (!this->stream_)
        {
#if URPC_LOGS
            usub::ulog::error("handle_ping: stream_ is null");
#endif
            co_return;
        }

        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Pong);

        uint16_t flags = FLAG_END_STREAM |
            build_security_flags(this->stream_.get(),
                                 this->stream_->peer_identity());

        hdr.flags = flags;
        hdr.stream_id = frame.header.stream_id;
        hdr.method_id = frame.header.method_id;
        hdr.length = 0;

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending PONG sid={} flags=0x{:x}",
            static_cast<void*>(this),
            hdr.stream_id,
            hdr.flags);
#endif

        co_await this->locked_send(hdr, {});
#if URPC_LOGS
        usub::ulog::info(
            "handle_ping: pong sent sid={}", hdr.stream_id);
#endif
        co_return;
    }
}
