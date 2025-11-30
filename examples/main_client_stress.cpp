//
// Created by Kirill Zhukov on 30.11.2025.
//

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"
#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

static std::atomic<uint64_t> g_ok_calls{0};
static std::atomic<uint64_t> g_failed_calls{0};
static std::atomic<uint64_t> g_total_latency_ns{0};
static std::atomic<int> g_finished_clients{0};

constexpr int kClientsCount = 64;
constexpr int kRequestsPerClient = 2;

task::Awaitable<void> torture_client(int client_id)
{
    ulog::info("CLIENT[{}]: torture_client started", client_id);

    auto client = std::make_shared<urpc::RpcClient>("localhost", 45900);

    ulog::info("CLIENT[{}]: before ping", client_id);
    bool pong = co_await client->async_ping();
    ulog::info("CLIENT[{}]: ping={}", client_id, pong);

    for (int i = 0; i < kRequestsPerClient; ++i)
    {
        std::string payload_str =
            "client=" + std::to_string(client_id) +
            ";req=" + std::to_string(i) +
            ";payload=abcdefghijklmnopqrstuvwxyz0123456789";

        std::span<const uint8_t> req{
            reinterpret_cast<const uint8_t*>(payload_str.data()),
            payload_str.size()
        };

        ulog::info("CLIENT[{}]: before call #{}", client_id, i);

        auto started = std::chrono::steady_clock::now();
        auto resp = co_await client->async_call("Example.Echo", req);
        auto ended = std::chrono::steady_clock::now();

        ulog::info("CLIENT[{}]: after call #{}", client_id, i);

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count();
        g_total_latency_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);

        if (resp.empty())
        {
            g_failed_calls.fetch_add(1, std::memory_order_relaxed);
            ulog::error("CLIENT[{}]: empty response on req={}", client_id, i);
            continue;
        }

        std::string resp_str(reinterpret_cast<const char*>(resp.data()), resp.size());
        if (resp_str != payload_str)
        {
            g_failed_calls.fetch_add(1, std::memory_order_relaxed);
            ulog::error("CLIENT[{}]: mismatch on req={} expected='{}' got='{}'",
                        client_id, i, payload_str, resp_str);
        }
        else
        {
            g_ok_calls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    client->close();
    co_await system::this_coroutine::sleep_for(5ms);

    g_finished_clients.fetch_add(1, std::memory_order_relaxed);
    ulog::info("CLIENT[{}]: torture_client finished", client_id);
    co_return;
}

int main()
{
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr,
        .debug_path = nullptr,
        .info_path = nullptr,
        .warn_path = nullptr,
        .error_path = nullptr,
        .flush_interval_ns = 2'000'000ULL,
        .queue_capacity = 16384,
        .batch_size = 1024,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);
    ulog::info("STRESS CLIENT: logger initialized");

    Uvent uvent(4);

    system::co_spawn([]() -> task::Awaitable<void>
    {
        ulog::info("STRESS CLIENT: pre-sleep 1s before spawning clients");
        co_await system::this_coroutine::sleep_for(1000ms);

        for (int i = 0; i < kClientsCount; ++i)
            system::co_spawn(torture_client(i));

        while (true)
        {
            int finished = g_finished_clients.load(std::memory_order_relaxed);
            ulog::info("STRESS CLIENT: finished_clients={}/{}", finished, kClientsCount);
            if (finished >= kClientsCount)
                break;
            co_await system::this_coroutine::sleep_for(100ms);
        }

        ulog::info("STRESS CLIENT: all clients finished");
        co_return;
    }());

    ulog::info("STRESS CLIENT: starting event loop");
    uvent.run();
    ulog::warn("STRESS CLIENT: event loop finished");

    uint64_t ok = g_ok_calls.load(std::memory_order_relaxed);
    uint64_t failed = g_failed_calls.load(std::memory_order_relaxed);
    uint64_t total = ok + failed;

    if (total > 0)
    {
        uint64_t total_ns = g_total_latency_ns.load(std::memory_order_relaxed);
        double avg_us = static_cast<double>(total_ns) / static_cast<double>(total) / 1000.0;

        ulog::info("STRESS CLIENT: total={} ok={} failed={} avg_latency_us={:.2f}",
                   total, ok, failed, avg_us);
    }
    else
    {
        ulog::info("STRESS CLIENT: no calls were completed");
    }

    ulog::warn("STRESS CLIENT: shutting down logger");
    usub::ulog::shutdown();
    return 0;
}