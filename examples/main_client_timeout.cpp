//
// main_client_timeout.cpp
//
// Demonstrates async_call_with_timeout(method_id, body, timeout_ms).
//
// This example makes TWO calls against the same server:
//
//   1. A "fast" method that should complete well inside the deadline.
//      Expected log line:
//          "completed in N ms (deadline was 500 ms, ... ms to spare)"
//
//   2. A "slow" method that the server takes longer to handle than
//      the deadline. Expected log line:
//          "rpc call TIMED OUT after ~500 ms ..."
//
// On the server side you should register two methods, e.g.
// "Example.Echo" (fast) and "Example.Sleep2s" (sleeps 2 seconds before
// replying). Adjust the names to whatever your server provides.
//
// The example also shows the proper shutdown pattern: pass a pointer
// to the Uvent loop into the coroutine and call uvent->stop() when
// done, so uvent.run() returns and the process exits cleanly.
//

#include <chrono>
#include <span>
#include <string>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

static task::Awaitable<void> do_one_call(
    std::shared_ptr<urpc::RpcClient> client,
    const char* label,
    const char* method_name,
    std::string body_str,
    uint32_t timeout_ms)
{
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t*>(body_str.data()),
        body_str.size()
    };

    const uint64_t method_id_hash = urpc::method_id(
        std::string_view{method_name});

    ulog::info("[{}] calling method='{}' (id={}) with deadline={}ms",
               label, method_name, method_id_hash, timeout_ms);

    const auto t0 = std::chrono::steady_clock::now();

    urpc::RpcCallResult res = co_await client->try_call(
        method_id_hash, body, timeout_ms);

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

    if (res.timed_out)
    {
        ulog::error("[{}] TIMED OUT after {} ms (deadline was {} ms). "
                    "A Cancel frame has been sent to the server.",
                    label, elapsed, timeout_ms);
        co_return;
    }

    if (!res.ok)
    {
        ulog::error("[{}] server-side / transport error after {} ms: "
                    "code={} msg='{}'",
                    label, elapsed, res.error_code, res.error_message);
        co_return;
    }

    const long long spare =
        static_cast<long long>(timeout_ms) - elapsed;

    std::string body_view(
        reinterpret_cast<const char*>(res.response.data()),
        res.response.size());

    if (timeout_ms > 0)
    {
        ulog::info("[{}] OK in {} ms (deadline was {} ms, {} ms to "
                   "spare), resp_size={} resp='{}'",
                   label, elapsed, timeout_ms, spare,
                   res.response.size(), body_view);
    }
    else
    {
        ulog::info("[{}] OK in {} ms, resp_size={} resp='{}'",
                   label, elapsed, res.response.size(), body_view);
    }
    co_return;
}

static task::Awaitable<void> client_coro(usub::Uvent* uvent)
{
    ulog::info("CLIENT: client_coro started, sleeping 1s before connect");
    co_await system::this_coroutine::sleep_for(1000ms);

    auto client = std::make_shared<urpc::RpcClient>(
        urpc::RpcClientConfig{
            .host              = "localhost",
            .port              = 45900,
            .stream_factory    = nullptr,
            .ping_interval_ms  = 0,
            .socket_timeout_ms = 5000,
        });

    bool pong = co_await client->async_ping();
    ulog::info("CLIENT: ping result={}", pong);
    if (!pong)
    {
        ulog::error("CLIENT: server unreachable, exiting");
        client->close();
        uvent->stop();
        co_return;
    }

    co_await do_one_call(
        client,
        /*label=*/      "fast",
        /*method=*/     "Example.Echo",
        /*body=*/       "hello timeout demo",
        /*timeout_ms=*/ 500);

    co_await do_one_call(
        client,
        /*label=*/      "slow",
        /*method=*/     "Example.Sleep2s",
        /*body=*/       "you will not see me",
        /*timeout_ms=*/ 500);

    co_await do_one_call(
        client,
        /*label=*/      "slow_ok",
        /*method=*/     "Example.Sleep2s",
        /*body=*/       "this one should make it",
        /*timeout_ms=*/ 5000);

    ulog::info("CLIENT: client_coro finished, stopping event loop");
    client->close();
    uvent->stop();
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

    usub::Uvent uvent(1);
    system::co_spawn(client_coro(&uvent));

    ulog::info("CLIENT: starting event loop");
    uvent.run();
    ulog::warn("CLIENT: event loop finished, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}
