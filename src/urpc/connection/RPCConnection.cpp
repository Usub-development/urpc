#include <urpc/connection/RPCConnection.h>

namespace urpc
{
    using namespace usub::uvent;

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

        ssize_t r = co_await stream.async_read(buf, expected);
#if URPC_LOGS
        usub::ulog::debug(
            "RpcConnection::read_exact: async_read r={} size={}",
            r, buf.size());
#endif

        co_return r > 0;
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
                "sid={} len={}",
                static_cast<unsigned>(hdr.magic),
                static_cast<unsigned>(hdr.version),
                static_cast<unsigned>(hdr.type),
                hdr.stream_id,
                hdr.length);
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
        (void)guard;

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: locked_send type={} sid={} len={}",
            static_cast<void*>(this),
            static_cast<unsigned>(hdr.type),
            hdr.stream_id,
            body.size());
#endif

        co_await send_frame(*this->stream_, hdr, body);

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

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending Response mid={} sid={} len={}",
            static_cast<void*>(this),
            hdr.method_id,
            hdr.stream_id,
            hdr.length);
#endif

        co_await this->locked_send(hdr, body);
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

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending ERROR Response mid={} sid={} len={} code={}",
            static_cast<void*>(this),
            hdr.method_id,
            hdr.stream_id,
            hdr.length,
            error_code);
#endif

        std::span<const uint8_t> span{buf.data(), buf.size()};
        co_await this->locked_send(hdr, span);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_request(RpcFrame frame)
    {
#if URPC_LOGS
        usub::ulog::info(
            "handle_request: sid={} mid={} len={}",
            frame.header.stream_id,
            frame.header.method_id,
            frame.header.length);
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
            "handle_ping: sid={}", frame.header.stream_id);
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
        hdr.flags = FLAG_END_STREAM;
        hdr.stream_id = frame.header.stream_id;
        hdr.method_id = frame.header.method_id;
        hdr.length = 0;

#if URPC_LOGS
        usub::ulog::info(
            "RpcConnection[{}]: sending PONG sid={}",
            static_cast<void*>(this),
            hdr.stream_id);
#endif

        co_await this->locked_send(hdr, {});
#if URPC_LOGS
        usub::ulog::info(
            "handle_ping: pong sent sid={}", frame.header.stream_id);
#endif
        co_return;
    }
}