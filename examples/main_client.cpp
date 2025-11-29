//
// Created by Kirill Zhukov on 29.11.2025.
//

// main_client.cpp

#include <chrono>
#include <span>
#include <string>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

task::Awaitable<void> client_coro()
{
    ulog::info("CLIENT: client_coro started, sleeping 1s before connect");
    co_await system::this_coroutine::sleep_for(1000ms);

    urpc::RpcClient client{"localhost", 45900};

    bool pong = co_await client.async_ping();
    ulog::info("CLIENT: ping result={}", pong);

    std::string payload_str = "hello from client";
    std::span<const uint8_t> req{
        reinterpret_cast<const uint8_t*>(payload_str.data()),
        payload_str.size()
    };

    auto resp = co_await client.async_call("Example.Echo", req);

    ulog::info("CLIENT: resp.size={}", resp.size());
    if (!resp.empty())
    {
        std::string resp_str(reinterpret_cast<const char*>(resp.data()),
                             resp.size());
        ulog::info("CLIENT: resp='{}'", resp_str);
    }

    ulog::info("CLIENT: client_coro finished");
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
        .queue_capacity = 14,
        .batch_size = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);
    ulog::info("CLIENT: logger initialized");

    Uvent uvent(1);
    system::co_spawn(client_coro());

    ulog::info("CLIENT: starting event loop");
    uvent.run();
    ulog::warn("CLIENT: event loop finished, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}