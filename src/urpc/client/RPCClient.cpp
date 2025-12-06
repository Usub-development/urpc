//
// Created by Kirill Zhukov on 29.11.2025.
//

#include <cstring>
#include <array>
#include <ranges>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <uvent/utils/buffer/DynamicBuffer.h>

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Endianness.h>
#include <urpc/transport/TCPStreamFactory.h>
#include <urpc/crypto/AppCrypto.h>
#include <urpc/transport/TlsRpcStream.h>

namespace urpc
{
    using namespace usub::uvent;

    static const AppCipherContext* get_cipher_for_stream(
        const std::shared_ptr<IRpcStream>& s)
    {
        auto tls = std::dynamic_pointer_cast<TlsRpcStream>(s);
        if (!tls)
            return nullptr;
        return tls->app_cipher();
    }

    static uint16_t build_security_flags_client(
        const std::shared_ptr<IRpcStream>& stream)
    {
        const RpcPeerIdentity* peer = nullptr;
        if (stream)
            peer = stream->peer_identity();

        uint16_t flags = 0;
        if (peer)
        {
            flags |= FLAG_TLS;
            if (peer->authenticated)
                flags |= FLAG_MTLS;
        }
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
            "RpcClient::read_exact: cur={} expected={}",
            buf.size(), expected);
#endif
        ssize_t r = co_await stream.async_read(buf, expected);
#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::read_exact: async_read r={} size={}",
            r, buf.size());
#endif

        if (r == 0)
        {
#if URPC_LOGS
            usub::ulog::info(
                "RpcClient::read_exact: peer closed connection "
                "(EOF / server idle-timeout)");
#endif
            co_return false;
        }

        if (r < 0)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::read_exact: async_read error r={} "
                "(treat as connection close / timeout)",
                r);
#endif
            co_return false;
        }

        co_return true;
    }

    RpcClient::RpcClient(std::string host, uint16_t port)
        : RpcClient(RpcClientConfig{
              std::move(host),
              port,
              nullptr})
    {
    }

    RpcClient::RpcClient(RpcClientConfig cfg)
        : config_(std::move(cfg))
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcClient ctor host={} port={} timeout_ms={} ping_interval_ms={}",
            this->config_.host,
            this->config_.port,
            this->config_.socket_timeout_ms,
            this->config_.ping_interval_ms);
#endif
        if (!this->config_.stream_factory)
        {
            this->config_.stream_factory =
                std::make_shared<TcpRpcStreamFactory>(
                    this->config_.socket_timeout_ms);
        }
    }

    usub::uvent::task::Awaitable<std::vector<uint8_t>>
    RpcClient::async_call(
        uint64_t method_id,
        std::span<const uint8_t> request_body)
    {
        using namespace usub::uvent;

        std::vector<uint8_t> empty;

#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_call: method_id={} body_size={}",
            method_id, request_body.size());
#endif

        bool ok = co_await this->ensure_connected();
        if (!ok)
        {
#if URPC_LOGS
            usub::ulog::error(
                "RpcClient::async_call: ensure_connected() failed");
#endif
            co_return empty;
        }

        uint32_t sid =
            this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0)
            sid = this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);

        auto call = std::make_shared<PendingCall>();
        call->event = std::make_shared<sync::AsyncEvent>(
            sync::Reset::Manual, false);

        {
            auto guard = co_await this->pending_mutex_.lock();
            (void)guard;
            this->pending_calls_[sid] = call;
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_call: registered PendingCall sid={} "
                "pending_size={}",
                sid,
                this->pending_calls_.size());
#endif
        }

        RpcFrameHeader hdr{};
        hdr.magic     = 0x55525043;
        hdr.version   = 1;
        hdr.type      = static_cast<uint8_t>(FrameType::Request);
        hdr.flags     = FLAG_END_STREAM;
        hdr.stream_id = sid;
        hdr.method_id = method_id;
        hdr.length    =
            static_cast<uint32_t>(request_body.size());

        std::vector<uint8_t> enc_buf;

        {
            auto guard = co_await this->write_mutex_.lock();
            (void)guard;

            auto stream = this->stream_;
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_call: stream_ is null before send_frame "
                    "sid={} – removing PendingCall",
                    sid);
#endif
                auto g2 = co_await this->pending_mutex_.lock();
                (void)g2;
                this->pending_calls_.erase(sid);
                co_return empty;
            }

            const AppCipherContext* cipher =
                get_cipher_for_stream(stream);

            std::span<const uint8_t> to_send = request_body;

            if (cipher && !request_body.empty())
            {
                bool enc_ok = app_encrypt_gcm(
                    *cipher,
                    request_body,
                    enc_buf);
                if (enc_ok)
                {
                    hdr.flags |= FLAG_ENCRYPTED;
                    hdr.length =
                        static_cast<uint32_t>(enc_buf.size());
                    to_send = std::span<const uint8_t>{
                        enc_buf.data(),
                        enc_buf.size()};
#if URPC_LOGS
                    usub::ulog::debug(
                        "RpcClient::async_call: encrypted body sid={} "
                        "plain_len={} enc_len={}",
                        sid,
                        request_body.size(),
                        enc_buf.size());
#endif
                }
                else
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "RpcClient::async_call: app_encrypt_gcm failed, "
                        "sending plaintext sid={}",
                        sid);
#endif
                }
            }

#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_call: BEFORE send_frame sid={} len={} "
                "flags=0x{:x}",
                sid, hdr.length, hdr.flags);
#endif
            bool sent = co_await send_frame(*stream, hdr, to_send);
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_call: send_frame failed for sid={} – "
                    "removing PendingCall",
                    sid);
#endif
                auto g2 = co_await this->pending_mutex_.lock();
                (void)g2;
                this->pending_calls_.erase(sid);
                co_return empty;
            }
        }

#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_call: BEFORE wait sid={}", sid);
#endif
        co_await call->event->wait();
#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_call: AFTER wait sid={}", sid);
#endif

        {
            auto guard = co_await this->pending_mutex_.lock();
            (void)guard;
            auto it = this->pending_calls_.find(sid);
            if (it != this->pending_calls_.end())
            {
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcClient::async_call: erasing PendingCall sid={} "
                    "pending_size_before={}",
                    sid,
                    this->pending_calls_.size());
#endif
                this->pending_calls_.erase(it);
            }
#if URPC_LOGS
            else
            {
                usub::ulog::warn(
                    "RpcClient::async_call: PendingCall already erased for "
                    "sid={} (by reader_loop cleanup?)",
                    sid);
            }
#endif
        }

        if (call->error)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::async_call: call error sid={} code={} msg='{}'",
                sid, call->error_code, call->error_message);
#endif
            co_return empty;
        }

        std::vector<uint8_t> resp = std::move(call->response);
#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_call: completed sid={} resp_size={}",
            sid, resp.size());
#endif

        co_return resp;
    }

    usub::uvent::task::Awaitable<bool> RpcClient::async_ping()
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient::async_ping: start");
#endif
        const bool ok = co_await this->ensure_connected();
        if (!ok)
        {
#if URPC_LOGS
            usub::ulog::error(
                "RpcClient::async_ping: ensure_connected() failed");
#endif
            co_return false;
        }

        uint32_t sid =
            this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0)
            sid = this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);

        auto evt = std::make_shared<sync::AsyncEvent>(
            sync::Reset::Manual, false);

        {
            auto guard = co_await this->ping_mutex_.lock();
            (void)guard;
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_ping: register waiter sid={} ping_waiters={}",
                sid,
                this->ping_waiters_.size() + 1);
#endif
            this->ping_waiters_[sid] = evt;
        }

        RpcFrameHeader hdr{};
        hdr.magic   = 0x55525043;
        hdr.version = 1;
        hdr.type    = static_cast<uint8_t>(FrameType::Ping);

        uint16_t flags = FLAG_END_STREAM |
            build_security_flags_client(this->stream_);

        hdr.flags     = flags;
        hdr.stream_id = sid;
        hdr.method_id = 0;
        hdr.length    = 0;

        {
            auto guard = co_await this->write_mutex_.lock();
            (void)guard;

            auto stream = this->stream_;
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_ping: stream_ is null before send_frame "
                    "sid={} – removing waiter",
                    sid);
#endif
                auto g2 = co_await this->ping_mutex_.lock();
                (void)g2;
                this->ping_waiters_.erase(sid);
                co_return false;
            }

#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_ping: BEFORE send_frame sid={} flags=0x{:x}",
                sid, hdr.flags);
#endif
            const bool sent =
                co_await send_frame(*stream, hdr, {});
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_ping: send_frame failed sid={} – "
                    "removing waiter",
                    sid);
#endif
                auto g2 = co_await this->ping_mutex_.lock();
                (void)g2;
                this->ping_waiters_.erase(sid);
                co_return false;
            }
        }

#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_ping: BEFORE wait sid={}", sid);
#endif
        co_await evt->wait();
#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_ping: AFTER wait sid={}", sid);
#endif

        bool result = false;
        {
            auto guard = co_await this->ping_mutex_.lock();
            (void)guard;

            auto it = this->ping_waiters_.find(sid);
            if (it != this->ping_waiters_.end())
            {
                result = true;
                this->ping_waiters_.erase(it);
            }
        }

#if URPC_LOGS
        usub::ulog::info(
            "RpcClient::async_ping: finished sid={} result={}",
            sid, result);
#endif
        co_return result;
    }

    void RpcClient::close()
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient::close()");
#endif
        this->running_.store(false, std::memory_order_relaxed);

        std::shared_ptr<IRpcStream> stream;
        stream.swap(this->stream_);

        if (stream)
            stream->shutdown();
    }

    usub::uvent::task::Awaitable<bool> RpcClient::ensure_connected()
    {
        using namespace usub::uvent;

        if (this->stream_ &&
            this->running_.load(std::memory_order_relaxed))
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::ensure_connected: already connected");
#endif
            co_return true;
        }

        auto guard = co_await this->connect_mutex_.lock();
        (void)guard;

        if (this->stream_ &&
            this->running_.load(std::memory_order_relaxed))
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::ensure_connected: already connected (after lock)");
#endif
            co_return true;
        }

        this->stream_.reset();

#if URPC_LOGS
        usub::ulog::info(
            "RpcClient::ensure_connected: connecting to {}:{}",
            this->config_.host, this->config_.port);
#endif

        if (!this->config_.stream_factory)
        {
            this->config_.stream_factory =
                std::make_shared<TcpRpcStreamFactory>(
                    this->config_.socket_timeout_ms);
        }

        auto stream =
            co_await this->config_.stream_factory->create_client_stream(
                this->config_.host,
                this->config_.port);
        if (!stream)
        {
#if URPC_LOGS
            usub::ulog::error(
                "RpcClient::ensure_connected: stream_factory returned nullptr");
#endif
            co_return false;
        }

        this->stream_ = std::move(stream);
        this->running_.store(true, std::memory_order_relaxed);

#if URPC_LOGS
        usub::ulog::info(
            "RpcClient::ensure_connected: connected, spawning reader_loop");
#endif

        auto self = this->shared_from_this();
        usub::uvent::system::co_spawn(
            RpcClient::run_reader_detached(std::move(self)));

        if (this->config_.ping_interval_ms > 0)
        {
            auto self2 = this->shared_from_this();
            usub::uvent::system::co_spawn(
                RpcClient::run_ping_detached(std::move(self2)));
        }

        co_return true;
    }

    usub::uvent::task::Awaitable<void>
    RpcClient::run_ping_detached(std::shared_ptr<RpcClient> self)
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient::ping_loop wrapper: start");
#endif
        co_await self->ping_loop();
#if URPC_LOGS
        usub::ulog::info("RpcClient::ping_loop wrapper: end");
#endif
        co_return;
    }

    usub::uvent::task::Awaitable<void> RpcClient::ping_loop()
    {
        using namespace usub::uvent;
        using namespace std::chrono_literals;

        const auto interval_ms = this->config_.ping_interval_ms;
        if (interval_ms == 0)
            co_return;

#if URPC_LOGS
        usub::ulog::info(
            "RpcClient::ping_loop: started, interval={}ms",
            interval_ms);
#endif

        const auto interval = std::chrono::milliseconds(interval_ms);

        while (this->running_.load(std::memory_order_relaxed))
        {
            co_await system::this_coroutine::sleep_for(interval);

            if (!this->running_.load(std::memory_order_relaxed))
                break;

#if URPC_LOGS
            usub::ulog::debug("RpcClient::ping_loop: sending async_ping");
#endif
            const bool ok = co_await this->async_ping();
            if (!ok)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcClient::ping_loop: async_ping failed, closing connection");
#endif
                this->close();
                break;
            }
        }

#if URPC_LOGS
        usub::ulog::info("RpcClient::ping_loop: exit");
#endif
        co_return;
    }

    bool RpcClient::parse_error_payload(
        const usub::uvent::utils::DynamicBuffer& payload,
        uint32_t& out_code,
        std::string& out_msg) const
    {
        using urpc::be_to_host;

        const std::size_t sz = payload.size();
#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::parse_error_payload: payload_size={}", sz);
#endif
        if (sz < 8)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::parse_error_payload: size<8 (sz={})", sz);
#endif
            return false;
        }

        uint32_t code_be = 0;
        uint32_t len_be  = 0;

        const auto* data =
            reinterpret_cast<const uint8_t*>(payload.data());

        std::memcpy(&code_be, data, 4);
        std::memcpy(&len_be,  data + 4, 4);

        const uint32_t code = be_to_host(code_be);
        const uint32_t len  = be_to_host(len_be);

        if (sz < 8u + len)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::parse_error_payload: sz({}) < 8+len({})",
                sz, 8u + len);
#endif
            return false;
        }

        out_code = code;
        out_msg.assign(
            reinterpret_cast<const char*>(data + 8),
            len);

#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::parse_error_payload: parsed code={} msg_len={}",
            out_code, out_msg.size());
#endif
        return true;
    }

    usub::uvent::task::Awaitable<void> RpcClient::run_reader_detached(
        std::shared_ptr<RpcClient> self)
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient::reader_loop wrapper: start");
#endif
        co_await self->reader_loop();
#if URPC_LOGS
        usub::ulog::info("RpcClient::reader_loop wrapper: end");
#endif
        co_return;
    }

    usub::uvent::task::Awaitable<void> RpcClient::reader_loop()
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient::reader_loop: started");
#endif
        while (this->running_.load(std::memory_order_relaxed))
        {
            auto stream = this->stream_;
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::reader_loop: stream_ is null");
#endif
                break;
            }

            utils::DynamicBuffer head;
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::reader_loop: reading header {} bytes",
                RpcFrameHeaderSize);
#endif
            const bool ok_hdr = co_await read_exact(
                *stream, head, RpcFrameHeaderSize);
            if (!ok_hdr)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcClient::reader_loop: header read_exact failed "
                    "(peer closed connection or server timeout)");
#endif
                break;
            }

            if (head.size() != RpcFrameHeaderSize)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcClient::reader_loop: header size={} != {} "
                    "(treat as connection close)",
                    head.size(), RpcFrameHeaderSize);
#endif
                break;
            }

            RpcFrameHeader hdr = parse_header(
                reinterpret_cast<const uint8_t*>(head.data()));
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::reader_loop: parsed header magic={} ver={} "
                "type={} sid={} len={} flags=0x{:x}",
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
                    "RpcClient::reader_loop: invalid header magic/ver "
                    "(magic={} ver={}) – closing connection",
                    static_cast<unsigned>(hdr.magic),
                    static_cast<unsigned>(hdr.version));
#endif
                break;
            }

            RpcFrame frame;
            frame.header = hdr;

            if (hdr.length > 0)
            {
                const std::size_t len = hdr.length;
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcClient::reader_loop: reading payload {} bytes",
                    len);
#endif
                const bool ok_body = co_await read_exact(
                    *stream, frame.payload, len);
                if (!ok_body || frame.payload.size() != len)
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "RpcClient::reader_loop: payload read_exact failed "
                        "size={} len={} (peer close / timeout)",
                        frame.payload.size(), len);
#endif
                    break;
                }
            }
            else
            {
#if URPC_LOGS
                usub::ulog::debug(
                    "RpcClient::reader_loop: zero-length payload");
#endif
            }

            auto ft = static_cast<FrameType>(frame.header.type);
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::reader_loop: got frame type={} sid={} len={}",
                static_cast<int>(ft),
                frame.header.stream_id,
                frame.header.length);
#endif

            switch (ft)
            {
            case FrameType::Response:
                {
#if URPC_LOGS
                    usub::ulog::debug(
                        "RpcClient::reader_loop: handling Response sid={} len={} "
                        "flags=0x{:x}",
                        frame.header.stream_id,
                        frame.header.length,
                        frame.header.flags);
#endif
                    std::shared_ptr<PendingCall> call;
                    {
                        auto guard = co_await this->pending_mutex_.lock();
                        (void)guard;

                        auto it =
                            this->pending_calls_.find(frame.header.stream_id);
                        if (it != this->pending_calls_.end())
                        {
                            call = it->second;
#if URPC_LOGS
                            usub::ulog::debug(
                                "RpcClient::reader_loop: found PendingCall "
                                "sid={} pending_size={}",
                                frame.header.stream_id,
                                this->pending_calls_.size());
#endif
                        }
                        else
                        {
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: Response for unknown "
                                "sid={} pending_size={}",
                                frame.header.stream_id,
                                this->pending_calls_.size());
#endif
                        }
                    }

                    if (!call)
                    {
#if URPC_LOGS
                        usub::ulog::error(
                            "RpcClient::reader_loop: no PendingCall for "
                            "sid={} -> protocol error, closing connection",
                            frame.header.stream_id);
#endif
                        this->close();
                        goto reader_loop_exit;
                    }

                    const bool is_error =
                        (frame.header.flags & FLAG_ERROR) != 0;
                    const bool encrypted =
                        (frame.header.flags & FLAG_ENCRYPTED) != 0;

                    std::span<const uint8_t> payload_view{
                        reinterpret_cast<const uint8_t*>(frame.payload.data()),
                        frame.payload.size()
                    };

                    std::vector<uint8_t> decrypted;

                    if (encrypted)
                    {
                        const AppCipherContext* cipher =
                            get_cipher_for_stream(this->stream_);
                        if (!cipher)
                        {
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: encrypted Response "
                                "but no cipher sid={}",
                                frame.header.stream_id);
#endif
                            call->error = true;
                            call->error_code = 0;
                            call->error_message =
                                "Encrypted response but cipher not available";
                            if (call->event)
                                call->event->set();
                            break;
                        }

                        bool ok_dec = app_decrypt_gcm(
                            *cipher,
                            payload_view,
                            decrypted);
                        if (!ok_dec)
                        {
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: app_decrypt_gcm failed "
                                "sid={}",
                                frame.header.stream_id);
#endif
                            call->error = true;
                            call->error_code = 0;
                            call->error_message =
                                "Failed to decrypt response";
                            if (call->event)
                                call->event->set();
                            break;
                        }

                        payload_view = std::span<const uint8_t>{
                            decrypted.data(),
                            decrypted.size()
                        };

#if URPC_LOGS
                        usub::ulog::debug(
                            "RpcClient::reader_loop: decrypted Response sid={} "
                            "enc_len={} plain_len={}",
                            frame.header.stream_id,
                            frame.payload.size(),
                            decrypted.size());
#endif
                    }

                    if (is_error)
                    {
                        usub::uvent::utils::DynamicBuffer tmp;
                        if (!payload_view.empty())
                        {
                            tmp.append(payload_view.data(),
                                       payload_view.size());
                        }

                        uint32_t code = 0;
                        std::string msg;
                        if (this->parse_error_payload(tmp, code, msg))
                        {
                            call->error        = true;
                            call->error_code   = code;
                            call->error_message = std::move(msg);
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: error Response "
                                "sid={} code={} msg='{}'",
                                frame.header.stream_id,
                                code,
                                call->error_message);
#endif
                        }
                        else
                        {
                            call->error        = true;
                            call->error_code   = 0;
                            call->error_message =
                                "Malformed error payload";
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: malformed error "
                                "payload sid={}",
                                frame.header.stream_id);
#endif
                        }

                        if (call->event)
                            call->event->set();
                    }
                    else
                    {
                        auto sz = payload_view.size();
                        call->response.resize(sz);
                        if (sz > 0)
                        {
                            std::memcpy(call->response.data(),
                                        payload_view.data(),
                                        sz);
                        }
                        call->error = false;
                        if (call->event)
                            call->event->set();
#if URPC_LOGS
                        usub::ulog::debug(
                            "RpcClient::reader_loop: Response delivered "
                            "sid={} body_size={}",
                            frame.header.stream_id,
                            sz);
#endif
                    }

                    break;
                }

            case FrameType::Ping:
                {
#if URPC_LOGS
                    usub::ulog::info(
                        "RpcClient::reader_loop: received Ping sid={}",
                        frame.header.stream_id);
#endif
                    RpcFrameHeader resp{};
                    resp.magic   = 0x55525043;
                    resp.version = 1;
                    resp.type    = static_cast<uint8_t>(FrameType::Pong);
                    resp.flags   = FLAG_END_STREAM |
                        build_security_flags_client(this->stream_);
                    resp.stream_id = frame.header.stream_id;
                    resp.method_id = frame.header.method_id;
                    resp.length    = 0;

                    auto guard = co_await this->write_mutex_.lock();
                    (void)guard;

                    auto stream2 = this->stream_;
                    if (!stream2)
                    {
#if URPC_LOGS
                        usub::ulog::warn(
                            "RpcClient::reader_loop: stream_ is null in Ping handler");
#endif
                        break;
                    }

#if URPC_LOGS
                    usub::ulog::debug(
                        "RpcClient::reader_loop: sending Pong sid={} flags=0x{:x}",
                        resp.stream_id,
                        resp.flags);
#endif
                    co_await send_frame(*stream2, resp, {});
                    break;
                }

            case FrameType::Pong:
                {
#if URPC_LOGS
                    usub::ulog::info(
                        "RpcClient::reader_loop: received Pong sid={}",
                        frame.header.stream_id);
#endif
                    std::shared_ptr<sync::AsyncEvent> evt;
                    {
                        auto guard = co_await this->ping_mutex_.lock();
                        (void)guard;

                        auto it =
                            this->ping_waiters_.find(frame.header.stream_id);
                        if (it != this->ping_waiters_.end())
                            evt = it->second;
                    }
                    if (evt)
                        evt->set();
                    break;
                }

            case FrameType::Request:
            case FrameType::Stream:
            case FrameType::Cancel:
            default:
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcClient::reader_loop: unexpected frame type={} sid={}",
                    static_cast<int>(ft),
                    frame.header.stream_id);
#endif
                break;
            }
        }

reader_loop_exit:
#if URPC_LOGS
        usub::ulog::warn("RpcClient::reader_loop: exiting, running_ was={}",
                         this->running_.load(std::memory_order_relaxed));
#endif
        this->running_.store(false, std::memory_order_relaxed);

        {
            auto guard = co_await this->pending_mutex_.lock();
            (void)guard;
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::reader_loop: cleaning {} pending calls "
                "(connection closed by peer/timeout)",
                this->pending_calls_.size());
#endif
            for (auto& call : this->pending_calls_ | std::views::values)
            {
                if (call && call->event)
                {
                    call->error        = true;
                    call->error_code   = 0;
                    call->error_message =
                        "Connection closed by peer (timeout/idle)";
                    call->event->set();
                }
            }
            this->pending_calls_.clear();
        }

        {
            auto guard = co_await this->ping_mutex_.lock();
            (void)guard;
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::reader_loop: cleaning {} ping waiters",
                this->ping_waiters_.size());
#endif
            for (auto& evt : this->ping_waiters_ | std::views::values)
            {
                if (evt)
                    evt->set();
            }
            this->ping_waiters_.clear();
        }

        {
            auto guard = co_await this->connect_mutex_.lock();
            (void)guard;
#if URPC_LOGS
            usub::ulog::info(
                "RpcClient::reader_loop: resetting stream_ after close/timeout");
#endif
            this->stream_.reset();
        }

        co_return;
    }
}