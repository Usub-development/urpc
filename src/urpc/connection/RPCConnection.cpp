#include <urpc/connection/RPCConnection.h>

namespace urpc
{
    RpcConnection::RpcConnection(std::shared_ptr<IRpcStream> stream,
                                 RpcMethodRegistry& registry)
        : stream_(std::move(stream))
          , registry_(registry)
    {
        usub::ulog::info("RpcConnection ctor: stream_={}",
                         static_cast<void*>(stream_.get()));
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::run_detached(std::shared_ptr<RpcConnection> self)
    {
        using namespace usub::uvent;

        if (!self)
            co_return;

        usub::ulog::info("RpcConnection::run_detached: self={}",
                         static_cast<void*>(self.get()));

        co_await self->loop();

        usub::ulog::warn("RpcConnection::run_detached: finished self={}",
                         static_cast<void*>(self.get()));
        co_return;
    }

    usub::uvent::task::Awaitable<void> RpcConnection::loop()
    {
        using namespace usub::uvent;

        if (!stream_)
        {
            usub::ulog::error("RpcConnection::loop: stream_ is null");
            co_return;
        }

        usub::ulog::info(
            "RpcConnection::loop: started, this={} stream_={}",
            static_cast<void*>(this),
            static_cast<void*>(stream_.get()));

        for (;;)
        {
            utils::DynamicBuffer head;
            head.reserve(sizeof(RpcFrameHeader));

            usub::ulog::debug(
                "RpcConnection::loop: reading header {} bytes",
                sizeof(RpcFrameHeader));

            ssize_t r = co_await stream_->async_read(head, sizeof(RpcFrameHeader));

            usub::ulog::debug(
                "RpcConnection::loop: header async_read r={} size={}",
                r, head.size());

            if (r <= 0)
            {
                usub::ulog::warn(
                    "RpcConnection::loop: header read r<=0, shutting down stream");
                stream_->shutdown();
                break;
            }

            if (head.size() < sizeof(RpcFrameHeader))
            {
                usub::ulog::warn(
                    "RpcConnection::loop: header size={} < {}, dropping",
                    head.size(), sizeof(RpcFrameHeader));
                stream_->shutdown();
                break;
            }

            RpcFrameHeader hdr = parse_header(head.data());
            usub::ulog::debug(
                "RpcConnection::loop: parsed header magic={} ver={} type={} len={}",
                static_cast<unsigned>(hdr.magic),
                static_cast<unsigned>(hdr.version),
                static_cast<unsigned>(hdr.type),
                hdr.length);

            if (hdr.magic != 0x55525043 || hdr.version != 1)
            {
                usub::ulog::warn(
                    "RpcConnection::loop: invalid header magic/ver, dropping");
                stream_->shutdown();
                break;
            }

            RpcFrame frame;
            frame.header = hdr;

            if (hdr.length > 0)
            {
                frame.payload.reserve(hdr.length);
                usub::ulog::debug(
                    "RpcConnection::loop: reading payload {} bytes",
                    hdr.length);

                ssize_t r2 = co_await stream_->async_read(
                    frame.payload, static_cast<size_t>(hdr.length));

                usub::ulog::debug(
                    "RpcConnection::loop: payload async_read r2={} size={}",
                    r2, frame.payload.size());

                if (r2 <= 0 || frame.payload.size() < hdr.length)
                {
                    usub::ulog::warn(
                        "RpcConnection::loop: payload read failed r2={} size={} len={}",
                        r2, frame.payload.size(), hdr.length);
                    stream_->shutdown();
                    break;
                }
            }
            else
            {
                usub::ulog::debug("RpcConnection::loop: zero-length payload");
            }

            FrameType ft = static_cast<FrameType>(frame.header.type);
            usub::ulog::debug(
                "RpcConnection::loop: got frame type={} sid={} len={}",
                static_cast<int>(ft),
                frame.header.stream_id,
                frame.header.length);

            switch (ft)
            {
            case FrameType::Request:
                co_await handle_request(std::move(frame));
                break;

            case FrameType::Cancel:
                co_await handle_cancel(std::move(frame));
                break;

            case FrameType::Ping:
                co_await handle_ping(std::move(frame));
                break;

            default:
                usub::ulog::warn(
                    "RpcConnection::loop: unknown frame type={} sid={}",
                    static_cast<int>(ft),
                    frame.header.stream_id);
                break;
            }
        }

        usub::ulog::warn("RpcConnection::loop: exiting");
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::locked_send(const RpcFrameHeader& hdr,
                               std::span<const uint8_t> body)
    {
        auto guard = co_await write_mutex_.lock();
        co_await send_frame(*stream_, hdr, body);
        (void)guard;
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
        hdr.flags = 0x0001; // END_STREAM
        hdr.stream_id = ctx.stream_id;
        hdr.method_id = ctx.method_id;
        hdr.length = static_cast<uint32_t>(body.size());

        co_await locked_send(hdr, body);
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
                   reinterpret_cast<const uint8_t*>(&code_be) + sizeof(code_be));

        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t*>(&msg_len_be),
                   reinterpret_cast<const uint8_t*>(&msg_len_be) + sizeof(msg_len_be));

        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t*>(message.data()),
                   reinterpret_cast<const uint8_t*>(message.data()) + message.size());

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

        std::span<const uint8_t> span{buf.data(), buf.size()};
        co_await locked_send(hdr, span);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_request(RpcFrame frame)
    {
        using namespace usub::uvent;

        RpcHandlerPtr fn = registry_.find(frame.header.method_id);
        if (!fn)
        {
            RpcContext tmp{
                .stream       = *stream_,
                .stream_id    = frame.header.stream_id,
                .method_id    = frame.header.method_id,
                .flags        = frame.header.flags,
                .cancel_token = usub::uvent::sync::CancellationToken{},
            };

            co_await send_simple_error(tmp, 404, "Unknown method");
            co_return;
        }

        auto src = std::make_shared<sync::CancellationSource>();
        {
            auto guard = co_await cancel_map_mutex_.lock();
            cancel_map_[frame.header.stream_id] = src;
            (void)guard;
        }

        RpcContext ctx{
            .stream = *stream_,
            .stream_id = frame.header.stream_id,
            .method_id = frame.header.method_id,
            .flags = frame.header.flags,
            .cancel_token = src->token(),
        };

        std::span<const uint8_t> body{
            frame.payload.data(),
            frame.payload.size(),
        };

        std::vector<uint8_t> resp = co_await (*fn)(ctx, body);

        {
            auto guard = co_await cancel_map_mutex_.lock();
            cancel_map_.erase(frame.header.stream_id);
            (void)guard;
        }

        std::span<const uint8_t> resp_span{resp.data(), resp.size()};
        co_await send_response(ctx, resp_span);
        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_cancel(RpcFrame frame)
    {
        using namespace usub::uvent;

        usub::ulog::info("handle_cancel: sid={}", frame.header.stream_id);

        std::shared_ptr<sync::CancellationSource> src;
        {
            auto guard = co_await cancel_map_mutex_.lock();
            auto it = cancel_map_.find(frame.header.stream_id);
            if (it != cancel_map_.end())
            {
                src = it->second;
                cancel_map_.erase(it);
            }
            (void)guard;
        }

        if (src)
        {
            src->request_cancel();
            usub::ulog::info(
                "handle_cancel: requested cancel for sid={}",
                frame.header.stream_id);
        }
        else
        {
            usub::ulog::warn(
                "handle_cancel: no cancel source for sid={}",
                frame.header.stream_id);
        }

        co_return;
    }

    usub::uvent::task::Awaitable<void>
    RpcConnection::handle_ping(RpcFrame frame)
    {
        using namespace usub::uvent;

        usub::ulog::info("handle_ping: sid={}", frame.header.stream_id);

        if (!stream_)
        {
            usub::ulog::error("handle_ping: stream_ is null");
            co_return;
        }

        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Pong);
        hdr.flags = 0x0001; // END_STREAM
        hdr.stream_id = frame.header.stream_id;
        hdr.method_id = frame.header.method_id;
        hdr.length = 0;

        co_await locked_send(hdr, {});
        usub::ulog::info("handle_ping: pong sent sid={}", frame.header.stream_id);
        co_return;
    }
}
