//
// Created by Kirill Zhukov on 30.11.2025.
//

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"
#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

static std::atomic<uint64_t> g_ok_calls{0};
static std::atomic<uint64_t> g_failed_calls{0};
static std::atomic<uint64_t> g_total_latency_ns{0};

struct MethodCase
{
    const char* name;
    uint64_t id;
};

static constexpr MethodCase kMethods[] = {
    {"Example.Echo", urpc::method_id("Example.Echo")},
    {"Example.Upper", urpc::method_id("Example.Upper")},
    {"Example.Reverse", urpc::method_id("Example.Reverse")}
};

static std::string to_upper(std::string_view s)
{
    std::string out{s};
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

static std::string reverse_str(std::string_view s)
{
    return std::string{s.rbegin(), s.rend()};
}

usub::uvent::task::Awaitable<void> run_single_client()
{
    constexpr int kRequests = 100;

    ulog::info("TEST1: single client started");

    auto client = std::make_shared<urpc::RpcClient>("localhost", 45900);

    bool pong = co_await client->async_ping();
    ulog::info("TEST1: ping={}", pong);

    for (int i = 0; i < kRequests; ++i)
    {
        const MethodCase& m = kMethods[i % 3];

        std::string payload_str =
            "test1;req=" + std::to_string(i) +
            ";method=" + std::string{m.name} +
            ";payload=abcdefghijklmnopqrstuvwxyz0123456789";

        std::span<const uint8_t> req{
            reinterpret_cast<const uint8_t*>(payload_str.data()),
            payload_str.size()
        };

        ulog::info("TEST1: before call #{} method={}", i, m.name);

        auto started = std::chrono::steady_clock::now();
        auto resp = co_await client->async_call(m.id, req);
        auto ended = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count();
        g_total_latency_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);

        if (resp.empty())
        {
            g_failed_calls.fetch_add(1, std::memory_order_relaxed);
            ulog::error("TEST1: empty response, req={} method={}", i, m.name);
            continue;
        }

        std::string resp_str(reinterpret_cast<const char*>(resp.data()), resp.size());
        std::string expected;

        if (std::string_view{m.name} == "Example.Echo")
        {
            expected = payload_str;
        }
        else if (std::string_view{m.name} == "Example.Upper")
        {
            expected = to_upper(payload_str);
        }
        else if (std::string_view{m.name} == "Example.Reverse")
        {
            expected = reverse_str(payload_str);
        }

        if (resp_str != expected)
        {
            g_failed_calls.fetch_add(1, std::memory_order_relaxed);
            ulog::error("TEST1: mismatch req={} method={} expected='{}' got='{}'",
                        i, m.name, expected, resp_str);
        }
        else
        {
            g_ok_calls.fetch_add(1, std::memory_order_relaxed);
            ulog::info("TEST1: ok req={} method={}", i, m.name);
        }
    }

    client->close();
    co_await system::this_coroutine::sleep_for(5ms);

    ulog::info("TEST1: single client finished");
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
    ulog::info("TEST1: logger initialized");

    Uvent uvent(2);

    system::co_spawn(run_single_client());

    ulog::info("TEST1: starting event loop");
    uvent.run();
    ulog::warn("TEST1: event loop finished");

    uint64_t ok = g_ok_calls.load(std::memory_order_relaxed);
    uint64_t failed = g_failed_calls.load(std::memory_order_relaxed);
    uint64_t total = ok + failed;

    if (total > 0)
    {
        uint64_t total_ns = g_total_latency_ns.load(std::memory_order_relaxed);
        double avg_us = static_cast<double>(total_ns) / static_cast<double>(total) / 1000.0;

        ulog::info("TEST1: total={} ok={} failed={} avg_latency_us={:.2f}",
                   total, ok, failed, avg_us);
    }
    else
    {
        ulog::info("TEST1: no calls were completed");
    }

    ulog::warn("TEST1: shutting down logger");
    usub::ulog::shutdown();
    return 0;
}