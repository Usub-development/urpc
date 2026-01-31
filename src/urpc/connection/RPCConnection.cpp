#include <urpc/connection/RPCConnection.h>
#include <urpc/crypto/AppCrypto.h>
#include <urpc/transport/TlsRpcStream.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

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

    static uint16_t build_security_flags(const RpcPeerIdentity* peer)
    {
        uint16_t flags = 0;
        if (peer)
        {
            flags |= FLAG_TLS;
            if (peer->authenticated)
                flags |= FLAG_MTLS;
        }
        return flags;
    }

    static bool aes256_gcm_encrypt(
        const uint8_t* key,
        const uint8_t* iv,
        size_t iv_len,
        const uint8_t* in,
        size_t in_len,
        uint8_t* out,
        uint8_t* tag)
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return false;

        int len = 0;
        int out_len = 0;
        bool ok = true;

        ok = ok && (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1);
        ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv_len), nullptr) == 1);
        ok = ok && (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) == 1);

        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        if (in_len > 0)
        {
            ok = ok && (EVP_EncryptUpdate(ctx, out, &len, in, static_cast<int>(in_len)) == 1);
            out_len = len;
        }

        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        ok = ok && (EVP_EncryptFinal_ex(ctx, out + out_len, &len) == 1);
        out_len += len;
        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
        EVP_CIPHER_CTX_free(ctx);

        if (!ok)
            return false;

        if (out_len != static_cast<int>(in_len))
            return false;

        return true;
    }

    static bool aes256_gcm_decrypt(
        const uint8_t* key,
        const uint8_t* iv,
        size_t iv_len,
        const uint8_t* in,
        size_t in_len,
        const uint8_t* tag,
        uint8_t* out)
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
            return false;

        int len = 0;
        int out_len = 0;
        bool ok = true;

        ok = ok && (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1);
        ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv_len), nullptr) == 1);
        ok = ok && (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) == 1);

        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        if (in_len > 0)
        {
            ok = ok && (EVP_DecryptUpdate(ctx, out, &len, in, static_cast<int>(in_len)) == 1);
            out_len = len;
        }

        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) == 1);
        if (!ok)
        {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        int final_rc = EVP_DecryptFinal_ex(ctx, out + out_len, &len);
        EVP_CIPHER_CTX_free(ctx);

        if (final_rc <= 0)
            return false;

        out_len += len;
        if (out_len != static_cast<int>(in_len))
            return false;

        return true;
    }

    static std::vector<uint8_t> encrypt_body_only(
        IRpcStream& stream,
        uint16_t& flags,
        std::span<const uint8_t> body)
    {
        if (body.empty())
            return {};

        std::array<uint8_t, 32> key{};
        if (!stream.get_app_secret_key(key))
        {
            std::vector<uint8_t> out;
            out.reserve(body.size());
            out.insert(out.end(), body.begin(), body.end());
            return out;
        }

        std::vector<uint8_t> out;
        const std::size_t iv_len = 12;
        const std::size_t tag_len = 16;
        out.resize(iv_len + tag_len + body.size());

        uint8_t* iv = out.data();
        uint8_t* tag = out.data() + iv_len;
        uint8_t* ct = out.data() + iv_len + tag_len;

        if (RAND_bytes(iv, static_cast<int>(iv_len)) != 1)
        {
            out.clear();
            return out;
        }

        if (!aes256_gcm_encrypt(
            key.data(),
            iv,
            iv_len,
            body.data(),
            body.size(),
            ct,
            tag))
        {
            out.clear();
            return out;
        }

        flags |= FLAG_ENCRYPTED;
        return out;
    }

    static std::span<const uint8_t> decrypt_body_if_needed(
        IRpcStream& stream,
        uint16_t flags,
        const usub::uvent::utils::DynamicBuffer& payload,
        std::vector<uint8_t>& tmp)
    {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
        std::size_t sz = payload.size();

        if ((flags & FLAG_ENCRYPTED) == 0)
        {
            return {data, sz};
        }

        if (sz < 12 + 16)
        {
            tmp.clear();
            return {tmp.data(), 0};
        }

        std::array<uint8_t, 32> key{};
        if (!stream.get_app_secret_key(key))
        {
            tmp.clear();
            return {tmp.data(), 0};
        }

        const std::size_t iv_len = 12;
        const std::size_t tag_len = 16;
        const uint8_t* iv = data;
        const uint8_t* tag = data + iv_len;
        const uint8_t* ct = data + iv_len + tag_len;
        std::size_t ct_len = sz - iv_len - tag_len;

        tmp.resize(ct_len);

        if (!aes256_gcm_decrypt(
            key.data(),
            iv,
            iv_len,
            ct,
            ct_len,
            tag,
            tmp.data()))
        {
            tmp.clear();
            return {tmp.data(), 0};
        }

        return {tmp.data(), tmp.size()};
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
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection ctor: stream_={}",
            static_cast<void*>(this->stream_.get()));
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
                co_await this->handle_request(std::move(frame));
                break;

            case FrameType::Cancel:
                co_await this->handle_cancel(std::move(frame));
                break;

            case FrameType::Ping:
                co_await this->handle_ping(std::move(frame));
                break;

            default:
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcConnection::loop: unknown frame type={} sid={}",
                    static_cast<int>(ft),
                    frame.header.stream_id);
#endif
                break;
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
                usub::ulog::warn(
                    "RpcConnection[{}]: app_encrypt_gcm failed, sending "
                    "plaintext for mid={} sid={}",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id);
#endif
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
                usub::ulog::warn(
                    "RpcConnection[{}]: app_encrypt_gcm failed for ERROR, "
                    "sending plaintext mid={} sid={} code={}",
                    static_cast<void*>(this),
                    hdr.method_id,
                    hdr.stream_id,
                    error_code);
#endif
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
            (void)guard;
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
            (void)guard;
            this->cancel_map_.erase(frame.header.stream_id);
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
            (void)guard;
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
            build_security_flags(this->stream_->peer_identity());

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
