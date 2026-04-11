//
// main_client_leak_check.cpp
//
// Stress harness for leak/UB detection. Run under ASan + LSan:
//
//   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
//
//   cmake --build build-asan -j --target urpc_example_client_leak_check
//
//   ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1 ./build-asan/urpc_example_client_leak_check
//
// The harness exercises every new code path introduced by the
// security/timeout patch:
//
//   1. N successful try_call against a fast method.
//      -> Should not leak PendingCall, response vector, or anything.
//
//   2. N try_call against a slow method WITH a tight deadline.
//      -> Each call spawns a watchdog coroutine, the watchdog wins
//         the race, sends a Cancel frame, marks PendingCall timed_out,
//         the late response from the server arrives and is dropped.
//      -> Tests: watchdog cleanup, late-response handling, Cancel
//         frame send under write_mutex_, cancel callback firing.
//
//   3. N try_call against the slow method with a generous deadline.
//      -> Watchdog is spawned but loses the race; test that the
//         watchdog coroutine is freed cleanly when its sleep_for
//         expires AFTER the response was already delivered.
//
//   4. N async_call (legacy unbounded) interleaved.
//      -> Sanity that the legacy path still doesn't leak after my
//         changes touched the same reader_loop.
//
// Server requirements: needs Example.Echo and Example.Sleep2s
// (the same handlers used by main_client_timeout.cpp). Run your
// existing server before launching this.
//

#include <atomic>
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

#ifndef LEAK_FAST_ITERS
#  define LEAK_FAST_ITERS 500
#endif
#ifndef LEAK_TIMEOUT_ITERS
#  define LEAK_TIMEOUT_ITERS 200
#endif
#ifndef LEAK_SLOW_OK_ITERS
#  define LEAK_SLOW_OK_ITERS 20
#endif
#ifndef LEAK_LEGACY_ITERS
#  define LEAK_LEGACY_ITERS 100
#endif

static std::atomic<uint64_t> g_ok_calls{0};
static std::atomic<uint64_t> g_timeout_calls{0};
static std::atomic<uint64_t> g_error_calls{0};

static task::Awaitable<void> phase_fast(
    std::shared_ptr<urpc::RpcClient> client, int n) {
    const uint64_t mid = urpc::method_id(std::string_view{"Example.Echo"});

    std::string body_str = "leak-check echo body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t *>(body_str.data()),
        body_str.size()
    };

    ulog::info("LEAK[fast]: starting {} successful calls", n);
    for (int i = 0; i < n; ++i) {
        auto res = co_await client->try_call(mid, body, /*timeout_ms=*/2000);
        if (res.ok) g_ok_calls.fetch_add(1);
        else if (res.timed_out) g_timeout_calls.fetch_add(1);
        else g_error_calls.fetch_add(1);

        if ((i + 1) % 100 == 0)
            ulog::info("LEAK[fast]: {}/{}", i + 1, n);
    }
    ulog::info("LEAK[fast]: done");
    co_return;
}

static task::Awaitable<void> phase_timeouts(
    std::shared_ptr<urpc::RpcClient> client, int n) {
    const uint64_t mid = urpc::method_id(std::string_view{"Example.Sleep2s"});

    std::string body_str = "leak-check timeout body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t *>(body_str.data()),
        body_str.size()
    };

    ulog::info("LEAK[timeout]: starting {} forced timeouts", n);
    for (int i = 0; i < n; ++i) {
        auto res = co_await client->try_call(mid, body, /*timeout_ms=*/50);
        if (res.ok) g_ok_calls.fetch_add(1);
        else if (res.timed_out) g_timeout_calls.fetch_add(1);
        else g_error_calls.fetch_add(1);

        if ((i + 1) % 50 == 0)
            ulog::info("LEAK[timeout]: {}/{}", i + 1, n);
    }
    ulog::info("LEAK[timeout]: done");
    co_return;
}

static task::Awaitable<void> phase_slow_ok(
    std::shared_ptr<urpc::RpcClient> client, int n) {
    const uint64_t mid = urpc::method_id(std::string_view{"Example.Sleep2s"});

    std::string body_str = "leak-check slow-ok body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t *>(body_str.data()),
        body_str.size()
    };

    ulog::info("LEAK[slow_ok]: starting {} slow-but-successful calls", n);
    for (int i = 0; i < n; ++i) {
        auto res = co_await client->try_call(mid, body, /*timeout_ms=*/5000);
        if (res.ok) g_ok_calls.fetch_add(1);
        else if (res.timed_out) g_timeout_calls.fetch_add(1);
        else g_error_calls.fetch_add(1);

        ulog::info("LEAK[slow_ok]: {}/{} ok={}", i + 1, n, res.ok);
    }
    ulog::info("LEAK[slow_ok]: done");
    co_return;
}

static task::Awaitable<void> phase_legacy(
    std::shared_ptr<urpc::RpcClient> client, int n) {
    std::string body_str = "leak-check legacy body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t *>(body_str.data()),
        body_str.size()
    };

    ulog::info("LEAK[legacy]: starting {} legacy async_call", n);
    for (int i = 0; i < n; ++i) {
        auto resp = co_await client->async_call("Example.Echo", body);
        if (!resp.empty()) g_ok_calls.fetch_add(1);
        else g_error_calls.fetch_add(1);

        if ((i + 1) % 50 == 0)
            ulog::info("LEAK[legacy]: {}/{}", i + 1, n);
    }
    ulog::info("LEAK[legacy]: done");
    co_return;
}

static task::Awaitable<void> harness_main(usub::Uvent *uvent) {
    ulog::info("LEAK: harness sleeping 500ms before connect");
    co_await system::this_coroutine::sleep_for(500ms);

    auto client = std::make_shared<urpc::RpcClient>(
        urpc::RpcClientConfig{
            .host = "localhost",
            .port = 45900,
            .stream_factory = nullptr,
            .ping_interval_ms = 0,
            .socket_timeout_ms = 5000,
        });

    bool pong = co_await client->async_ping();
    if (!pong) {
        ulog::error("LEAK: server unreachable on localhost:45900, "
            "cannot run harness. Start your urpc server with "
            "Example.Echo and Example.Sleep2s and retry.");
        client->close();
        uvent->stop();
        co_return;
    }
    ulog::info("LEAK: ping OK, starting phases");

    co_await phase_fast(client, LEAK_FAST_ITERS);
    co_await phase_timeouts(client, LEAK_TIMEOUT_ITERS);
    co_await phase_slow_ok(client, LEAK_SLOW_OK_ITERS);
    co_await phase_legacy(client, LEAK_LEGACY_ITERS);

    ulog::info("LEAK: draining late responses for 3s");
    co_await system::this_coroutine::sleep_for(3000ms);

    ulog::info("LEAK: SUMMARY ok={} timeout={} error={}",
               g_ok_calls.load(),
               g_timeout_calls.load(),
               g_error_calls.load());

    client->close();

    co_await system::this_coroutine::sleep_for(500ms);

    ulog::info("LEAK: harness done, stopping event loop");
    uvent->stop();
    co_return;
}

int main() {
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr,
        .debug_path = nullptr,
        .info_path = nullptr,
        .warn_path = nullptr,
        .error_path = nullptr,
        .flush_interval_ns = 2'000'000ULL,
        .queue_capacity = 16384,
        .batch_size = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };
    usub::ulog::init(cfg);
    ulog::info("LEAK: logger initialized");

    usub::Uvent uvent(1);
    system::co_spawn(harness_main(&uvent));

    ulog::info("LEAK: starting event loop");
    uvent.run();
    ulog::warn("LEAK: event loop finished");

    usub::ulog::shutdown();
    return 0;
}
