//
// Created by Kirill Zhukov on 29.11.2025.
//

#include <cstring>

#include <uvent/utils/buffer/DynamicBuffer.h>

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Endianness.h>

namespace urpc
{
    RpcClient::RpcClient(std::string host, uint16_t port)
        : host_(std::move(host))
          , port_(port)
    {
#if URPC_LOGS
        usub::ulog::info("RpcClient ctor host={} port={}", host_, port_);
#endif
    }

    usub::uvent::task::Awaitable<std::vector<uint8_t>> RpcClient::async_call(
        uint64_t method_id,
        std::span<const uint8_t> request_body)
    {
        using namespace usub::uvent;

        std::vector<uint8_t> empty;

        bool ok = co_await ensure_connected();
        if (!ok)
        {
#if URPC_LOGS
            usub::ulog::error("RpcClient::async_call: ensure_connected() failed");
#endif
            co_return empty;
        }

        uint32_t sid = next_stream_id_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0)
            sid = next_stream_id_.fetch_add(1, std::memory_order_relaxed);
#if URPC_LOGS
        usub::ulog::debug("RpcClient::async_call: method_id={} stream_id={} body_size={}",
                          method_id, sid, request_body.size());
#endif

        auto call = std::make_shared<PendingCall>();
        call->event = std::make_shared<sync::AsyncEvent>(
            sync::Reset::Manual, false);

        {
            auto guard = co_await pending_mutex_.lock();
            pending_calls_[sid] = call;
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
            auto guard = co_await write_mutex_.lock();
            bool sent = co_await send_frame(*stream_, hdr, request_body);
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error("RpcClient::async_call: send_frame failed for sid={}", sid);
#endif
                auto g2 = co_await pending_mutex_.lock();
                pending_calls_.erase(sid);
                co_return empty;
            }
        }

        co_await call->event->wait();

        if (call->error)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "RpcClient::async_call: call error sid={} code={} msg='{}'",
                sid, call->error_code, call->error_message);
#endif
            co_return empty;
        }

        std::vector<uint8_t> resp;
        {
            auto guard = co_await pending_mutex_.lock();
            auto it = pending_calls_.find(sid);
            if (it != pending_calls_.end())
            {
                resp = std::move(it->second->response);
                pending_calls_.erase(it);
            }
            else
            {
                resp = std::move(call->response);
            }
        }

#if URPC_LOGS
        usub::ulog::debug("RpcClient::async_call: completed sid={} resp_size={}",
                          sid, resp.size());
#endif

        co_return resp;
    }

    usub::uvent::task::Awaitable<bool> RpcClient::async_ping()
    {
        using namespace usub::uvent;
#if URPC_LOGS
        usub::ulog::info("RpcClient::async_ping: start");
#endif
        bool ok = co_await ensure_connected();
        if (!ok)
        {
#if URPC_LOGS
            usub::ulog::error("RpcClient::async_ping: ensure_connected() failed");
#endif
            co_return false;
        }

        uint32_t sid = next_stream_id_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0)
            sid = next_stream_id_.fetch_add(1, std::memory_order_relaxed);

        auto evt = std::make_shared<sync::AsyncEvent>(
            sync::Reset::Manual, false);

        {
            auto guard = co_await ping_mutex_.lock();
            ping_waiters_[sid] = evt;
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
            auto guard = co_await write_mutex_.lock();
            bool sent = co_await send_frame(*stream_, hdr, {});
            if (!sent)
            {
#if URPC_LOGS
                usub::ulog::error("RpcClient::async_ping: send_frame failed sid={}", sid);
#endif
                auto g2 = co_await ping_mutex_.lock();
                ping_waiters_.erase(sid);
                co_return false;
            }
        }

        co_await evt->wait();

        bool result = false;
        {
            auto guard = co_await ping_mutex_.lock();
            auto it = ping_waiters_.find(sid);
            if (it != ping_waiters_.end())
            {
                result = true;
                ping_waiters_.erase(it);
            }
        }
#if URPC_LOGS
        usub::ulog::info("RpcClient::async_ping: finished sid={} result={}",
                         sid, result);
#endif
        co_return result;
    }

    void RpcClient::close()
    {
#if URPC_LOGS
        usub::ulog::warn("RpcClient::close()");
#endif
        running_.store(false, std::memory_order_relaxed);
        if (stream_)
        {
            stream_->shutdown();
            stream_.reset();
        }
    }

    usub::uvent::task::Awaitable<bool> RpcClient::ensure_connected()
    {
        using namespace usub::uvent;

        if (stream_)
            co_return true;

        auto guard = co_await connect_mutex_.lock();
        if (stream_)
            co_return true;
#if URPC_LOGS
        usub::ulog::info("RpcClient::ensure_connected: connecting to {}:{}",
                         host_, port_);
#endif

        net::TCPClientSocket sock;
        auto res = co_await sock.async_connect(host_.c_str(),
                                               std::to_string(port_).c_str());
        if (res.has_value())
        {
#if URPC_LOGS
            usub::ulog::error("RpcClient::ensure_connected: async_connect failed, ec={}",
                              res.value());
#endif
            co_return false;
        }

        stream_ = std::make_shared<TcpRpcStream>(std::move(sock));
        running_.store(true, std::memory_order_relaxed);
#if URPC_LOGS
        usub::ulog::info("RpcClient::ensure_connected: connected, spawning reader_loop");
#endif
        system::co_spawn(reader_loop());
        co_return true;
    }

    bool RpcClient::parse_error_payload(const usub::uvent::utils::DynamicBuffer& payload,
                                        uint32_t& out_code,
                                        std::string& out_msg) const
    {
        using urpc::be_to_host;

        const std::size_t sz = payload.size();
        if (sz < 8)
            return false;

        uint32_t code_be = 0;
        uint32_t len_be  = 0;

        const auto* data = reinterpret_cast<const uint8_t*>(payload.data());

        std::memcpy(&code_be, data, 4);
        std::memcpy(&len_be,  data + 4, 4);

        const uint32_t code = be_to_host(code_be);
        const uint32_t len  = be_to_host(len_be);

        if (sz < 8u + len)
            return false;

        out_code = code;
        out_msg.assign(reinterpret_cast<const char*>(data + 8), len);
        return true;
    }

    usub::uvent::task::Awaitable<void> RpcClient::reader_loop()
    {
        using namespace usub::uvent;
#if URPC_LOGS
        usub::ulog::info("RpcClient::reader_loop: started");
#endif
        while (running_.load(std::memory_order_relaxed))
        {
            if (!stream_)
            {
#if URPC_LOGS
                usub::ulog::error("RpcClient::reader_loop: stream_ is null");
#endif
                break;
            }

            utils::DynamicBuffer head;
            head.reserve(sizeof(RpcFrameHeader));
#if URPC_LOGS
            usub::ulog::debug("RpcClient::reader_loop: reading header {} bytes",
                              sizeof(RpcFrameHeader));
#endif
            ssize_t r = co_await stream_->async_read(
                head, sizeof(RpcFrameHeader));
#if URPC_LOGS
            usub::ulog::debug("RpcClient::reader_loop: header async_read r={} size={}",
                              r, head.size());
#endif

            if (r <= 0)
            {
#if URPC_LOGS
                usub::ulog::warn("RpcClient::reader_loop: header read r<=0, breaking");
#endif
                break;
            }

            if (head.size() < sizeof(RpcFrameHeader))
            {
#if URPC_LOGS
                usub::ulog::warn("RpcClient::reader_loop: header size={} < {}",
                                 head.size(), sizeof(RpcFrameHeader));
#endif
                break;
            }

            RpcFrameHeader hdr = parse_header(head.data());
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::reader_loop: parsed header magic={} ver={} type={} len={}",
                static_cast<unsigned>(hdr.magic),
                static_cast<unsigned>(hdr.version),
                static_cast<unsigned>(hdr.type),
                hdr.length);
#endif
            if (hdr.magic != 0x55525043 || hdr.version != 1)
            {
#if URPC_LOGS
                usub::ulog::warn("RpcClient::reader_loop: invalid header magic/ver");
#endif
                break;
            }

            RpcFrame frame;
            frame.header = hdr;

            if (hdr.length > 0)
            {
                frame.payload.reserve(hdr.length);
#if URPC_LOGS
                usub::ulog::debug("RpcClient::reader_loop: reading payload {} bytes",
                                  hdr.length);
#endif

                ssize_t r2 = co_await stream_->async_read(
                    frame.payload, static_cast<size_t>(hdr.length));
#if URPC_LOGS
                usub::ulog::debug("RpcClient::reader_loop: payload async_read r2={} size={}",
                                  r2, frame.payload.size());
#endif
                if (r2 <= 0 || frame.payload.size() < hdr.length)
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "RpcClient::reader_loop: payload read failed r2={} size={} len={}",
                        r2, frame.payload.size(), hdr.length);
#endif
                    break;
                }
            }
            else
            {
#if URPC_LOGS
                usub::ulog::debug("RpcClient::reader_loop: zero-length payload");
#endif
            }

            auto ft = static_cast<FrameType>(frame.header.type);
#if URPC_LOGS
            usub::ulog::debug("RpcClient::reader_loop: got frame type={} sid={} len={}",
                              static_cast<int>(ft),
                              frame.header.stream_id,
                              frame.header.length);
#endif

            switch (ft)
            {
            case FrameType::Response:
                {
                    std::shared_ptr<PendingCall> call;
                    {
                        auto guard = co_await pending_mutex_.lock();
                        auto it = pending_calls_.find(frame.header.stream_id);
                        if (it != pending_calls_.end())
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

                    bool is_error = (frame.header.flags & FLAG_ERROR) != 0;

                    if (is_error)
                    {
                        uint32_t code = 0;
                        std::string msg;
                        if (parse_error_payload(frame.payload, code, msg))
                        {
                            call->error = true;
                            call->error_code = code;
                            call->error_message = std::move(msg);
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: error Response sid={} code={} msg='{}'",
                                frame.header.stream_id, code, call->error_message);
#endif
                        }
                        else
                        {
                            call->error = true;
                            call->error_code = 0;
                            call->error_message = "Malformed error payload";
#if URPC_LOGS
                            usub::ulog::warn(
                                "RpcClient::reader_loop: malformed error payload sid={}",
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
                    usub::ulog::info("RpcClient::reader_loop: received Ping sid={}",
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

                    auto guard = co_await write_mutex_.lock();
                    co_await send_frame(*stream_, resp, {});
                    break;
                }

            case FrameType::Pong:
                {
#if URPC_LOGS
                    usub::ulog::info("RpcClient::reader_loop: received Pong sid={}",
                                     frame.header.stream_id);
#endif
                    std::shared_ptr<sync::AsyncEvent> evt;
                    {
                        auto guard = co_await ping_mutex_.lock();
                        auto it = ping_waiters_.find(frame.header.stream_id);
                        if (it != ping_waiters_.end())
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
        running_.store(false, std::memory_order_relaxed);

        {
            auto guard = co_await pending_mutex_.lock();
            for (auto& [sid, call] : pending_calls_)
            {
                if (call && call->event)
                {
                    call->error = true;
                    call->error_code = 0;
                    call->error_message = "Connection closed";
                    call->event->set();
                }
            }
            pending_calls_.clear();
        }

        {
            auto guard = co_await ping_mutex_.lock();
            for (auto& [sid, evt] : ping_waiters_)
            {
                if (evt) evt->set();
            }
            ping_waiters_.clear();
        }

        co_return;
    }
}