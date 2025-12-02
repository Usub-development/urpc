//
// Created by root on 11/29/25.
//

#ifndef RPCCLIENT_H
#define RPCCLIENT_H

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <uvent/Uvent.h>
#include <uvent/tasks/Awaitable.h>
#include <uvent/sync/AsyncMutex.h>
#include <uvent/sync/AsyncEvent.h>
#include <uvent/system/SystemContext.h>
#include <uvent/utils/buffer/DynamicBuffer.h>

#include <ulog/ulog.h>

#include <urpc/config/Config.h>
#include <urpc/datatypes/Frame.h>
#include <urpc/transport/IRPCStream.h>
#include <urpc/transport/IOOps.h>
#include <urpc/utils/Hash.h>

namespace urpc
{
    struct PendingCall
    {
        std::shared_ptr<usub::uvent::sync::AsyncEvent> event;
        std::vector<uint8_t> response;
        bool error{false};
        uint32_t error_code{0};
        std::string error_message;
    };

    class RpcClient : public std::enable_shared_from_this<RpcClient>
    {
    public:
        RpcClient(std::string host, uint16_t port);

        explicit RpcClient(RpcClientConfig cfg);

        usub::uvent::task::Awaitable<std::vector<uint8_t>> async_call(
            uint64_t method_id,
            std::span<const uint8_t> request_body);

        template <size_t N>
        usub::uvent::task::Awaitable<std::vector<uint8_t>> async_call(
            const char (&name)[N],
            std::span<const uint8_t> request_body)
        {
            uint64_t mid = fnv1a64_rt(std::string_view{name, N - 1});
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_call(name): name={} hash={}",
                name, mid);
#endif
            co_return co_await this->async_call(mid, request_body);
        }

        template <uint64_t MethodId>
        usub::uvent::task::Awaitable<std::vector<uint8_t>> async_call_ct(
            std::span<const uint8_t> request_body)
        {
#if URPC_LOGS
            usub::ulog::debug(
                "RpcClient::async_call_ct: MethodId={}",
                MethodId);
#endif
            co_return co_await this->async_call(MethodId, request_body);
        }

        usub::uvent::task::Awaitable<bool> async_ping();

        void close();

    private:
        RpcClientConfig config_;

        std::shared_ptr<IRpcStream> stream_;

        std::atomic<uint32_t> next_stream_id_{1};
        std::atomic<bool> running_{false};

        usub::uvent::sync::AsyncMutex write_mutex_;
        usub::uvent::sync::AsyncMutex connect_mutex_;
        usub::uvent::sync::AsyncMutex pending_mutex_;
        usub::uvent::sync::AsyncMutex ping_mutex_;

        std::unordered_map<uint32_t, std::shared_ptr<PendingCall>> pending_calls_;
        std::unordered_map<uint32_t,
                           std::shared_ptr<usub::uvent::sync::AsyncEvent>> ping_waiters_;

        usub::uvent::task::Awaitable<bool> ensure_connected();
        usub::uvent::task::Awaitable<void> reader_loop();

        static usub::uvent::task::Awaitable<void> run_reader_detached(
            std::shared_ptr<RpcClient> self);

        bool parse_error_payload(const usub::uvent::utils::DynamicBuffer& payload,
                                 uint32_t& out_code,
                                 std::string& out_msg) const;
    };
}

#endif // RPCCLIENT_H