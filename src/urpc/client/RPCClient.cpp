//
// Created by Kirill Zhukov on 29.11.2025.
//

#include <cstring>

#include <uvent/utils/buffer/DynamicBuffer.h>

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Endianness.h>
#include <urpc/transport/TCPStreamFactory.h>

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
                "RpcClient::read_exact: EOF (r=0) -> stop reading");
#endif
            co_return false; // сигнал наверх "дальше читать нечего"
        }

        if (r < 0)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::read_exact: async_read error r={}", r);
#endif
            co_return false;
        }

        co_return true;
    }

    RpcClient::RpcClient(std::string host, uint16_t port)
        : RpcClient(RpcClientConfig{
            std::move(host),
            port,
            nullptr
        })
    {
    }

    RpcClient::RpcClient(RpcClientConfig cfg)
        : config_(std::move(cfg))
    {
#if URPC_LOGS
        usub::ulog::info(
            "RpcClient ctor host={} port={}",
            this->config_.host,
            this->config_.port);
#endif
        if (!this->config_.stream_factory)
        {
            this->config_.stream_factory =
                std::make_shared<TcpRpcStreamFactory>();
        }
    }

    usub::uvent::task::Awaitable<std::vector<uint8_t>> RpcClient::async_call(
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
            usub::ulog::error("RpcClient::async_call: ensure_connected() failed");
#endif
            co_return empty;
        }

        uint32_t sid =
            this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0)
            sid = this->next_stream_id_.fetch_add(1, std::memory_order_relaxed);

#if URPC_LOGS
        usub::ulog::debug(
            "RpcClient::async_call: BEFORE register pending sid={}",
            sid);
#endif

        auto call = std::make_shared<PendingCall>();
        call->event = std::make_shared<sync::AsyncEvent>(
            sync::Reset::Manual, false);
        {
            auto guard = co_await this->pending_mutex_.lock();
            (void)guard;
            this->pending_calls_[sid] = call;
        }

        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Request);
        hdr.flags = FLAG_END_STREAM;
        hdr.stream_id = sid;
        hdr.method_id = method_id;
        hdr.length = static_cast<uint32_t>(request_body.size());
        {
            auto guard = co_await this->write_mutex_.lock();
            (void)guard;

            auto stream = this->stream_;
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_call: stream_ is null before send_frame sid={}",
                    sid);
#endif
                auto g2 = co_await this->pending_mutex_.lock();
                (void)g2;
                this->pending_calls_.erase(sid);
                co_return empty;
            }

#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_call: BEFORE send_frame sid={} len={}",
                sid, hdr.length);
#endif
            bool sent = co_await send_frame(*stream, hdr, request_body);
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_call: send_frame failed for sid={}",
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
                this->pending_calls_.erase(it);
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
                "RpcClient::async_ping: register waiter sid={}", sid);
#endif
            this->ping_waiters_[sid] = evt;
        }

        RpcFrameHeader hdr{};
        hdr.magic = 0x55525043;
        hdr.version = 1;
        hdr.type = static_cast<uint8_t>(FrameType::Ping);
        hdr.flags = FLAG_END_STREAM;
        hdr.stream_id = sid;
        hdr.method_id = 0;
        hdr.length = 0;

        {
            auto guard = co_await this->write_mutex_.lock();
            (void)guard;

            auto stream = this->stream_;
            if (!stream)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_ping: stream_ is null before send_frame sid={}",
                    sid);
#endif
                auto g2 = co_await this->ping_mutex_.lock();
                (void)g2;
                this->ping_waiters_.erase(sid);
                co_return false;
            }

#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_ping: BEFORE send_frame sid={}", sid);
#endif
            const bool sent =
                co_await send_frame(*stream, hdr, {});
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error(
                    "RpcClient::async_ping: send_frame failed sid={}",
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
                std::make_shared<TcpRpcStreamFactory>();
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

        co_return true;
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
        uint32_t len_be = 0;

        const auto* data =
            reinterpret_cast<const uint8_t*>(payload.data());

        std::memcpy(&code_be, data, 4);
        std::memcpy(&len_be, data + 4, 4);

        const uint32_t code = be_to_host(code_be);
        const uint32_t len = be_to_host(len_be);

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
                    "RpcClient::reader_loop: header read_exact failed");
#endif
                break;
            }

            if (head.size() != RpcFrameHeaderSize)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "RpcClient::reader_loop: header size={} != {}",
                    head.size(), RpcFrameHeaderSize);
#endif
                break;
            }

            RpcFrameHeader hdr = parse_header(
                reinterpret_cast<const uint8_t*>(head.data()));
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::reader_loop: parsed header magic={} ver={} "
                "type={} sid={} len={}",
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
                    "RpcClient::reader_loop: invalid header magic/ver "
                    "(magic={} ver={})",
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
                        "size={} len={}",
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
                        "RpcClient::reader_loop: handling Response sid={} len={} flags={}",
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
                            call = it->second;
                    }

                    if (!call)
                    {
#if URPC_LOGS
                        usub::ulog::warn(
                            "RpcClient::reader_loop: Response for unknown sid={}",
                            frame.header.stream_id);
#endif
                        break;
                    }

                    const bool is_error =
                        (frame.header.flags & FLAG_ERROR) != 0;

                    if (is_error)
                    {
                        uint32_t code = 0;
                        std::string msg;
                        if (this->parse_error_payload(
                            frame.payload, code, msg))
                        {
                            call->error = true;
                            call->error_code = code;
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
                            call->error = true;
                            call->error_code = 0;
                            call->error_message = "Malformed error payload";
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
                        auto sz = frame.payload.size();
                        call->response.resize(sz);
                        if (sz > 0)
                        {
                            std::memcpy(call->response.data(),
                                        frame.payload.data(),
                                        sz);
                        }
                        call->error = false;
                        if (call->event)
                            call->event->set();
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
                    resp.magic = 0x55525043;
                    resp.version = 1;
                    resp.type = static_cast<uint8_t>(FrameType::Pong);
                    resp.flags = FLAG_END_STREAM;
                    resp.stream_id = frame.header.stream_id;
                    resp.method_id = frame.header.method_id;
                    resp.length = 0;

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
                        "RpcClient::reader_loop: sending Pong sid={}",
                        resp.stream_id);
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

#if URPC_LOGS
        usub::ulog::warn("RpcClient::reader_loop: exiting");
#endif
        this->running_.store(false, std::memory_order_relaxed);

        {
            auto guard = co_await this->pending_mutex_.lock();
            (void)guard;
            for (auto& call : this->pending_calls_ | std::views::values)
            {
                if (call && call->event)
                {
                    call->error = true;
                    call->error_code = 0;
                    call->error_message = "Connection closed";
                    call->event->set();
                }
            }
            this->pending_calls_.clear();
        }

        {
            auto guard = co_await this->ping_mutex_.lock();
            (void)guard;
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
            this->stream_.reset();
        }

        co_return;
    }
}
