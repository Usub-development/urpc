//
// main_client_leak_check_baseline.cpp
//
// BASELINE harness for the leak check. Uses ONLY the legacy
// async_call API -- no try_call, no async_call_with_timeout, no
// timeout watchdog, no cancel_callback. This is a deliberate
// restriction so that the SAME source file can be compiled against
// both:
//
//   1. the ORIGINAL urpc-main tree (before any of the security or
//      timeout fixes), and
//
//   2. the PATCHED urpc-fixed tree (after).
//
// The point is to give an apples-to-apples leak comparison between
// the two trees. If both produce the same set of LeakSanitizer
// reports, then the fixes are leak-neutral and any leak you see
// already existed in uvent / urpc.
//
// Usage:
//
//   # Build the original (no fixes) under ASan and run this harness
//   cd /path/to/urpc-main
//   cp /path/to/this/file examples/main_client_leak_check_baseline.cpp
//   # add it to CMakeLists.txt as a target if not already there
//   cmake -B build-asan-original -DCMAKE_BUILD_TYPE=Debug \
//         -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
//         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
//   cmake --build build-asan-original -j --target urpc_example_client_leak_check_baseline
//   ASAN_OPTIONS=detect_leaks=1 \
//     ./build-asan-original/urpc_example_client_leak_check_baseline 2>baseline.log
//
//   # Repeat for urpc-fixed and diff the leak reports.
//   diff baseline.log fixed.log
//
// Server requirement: needs Example.Echo (your existing main_server
// already provides it). Sleep2s is NOT used by this baseline so the
// server doesn't even need it.
//

#include <atomic>
#include <chrono>
#include <span>
#include <string>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>
#include <urpc/transport/TCPStream.h>  // for urpc::diag counters

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

#ifndef LEAK_BASELINE_ITERS
#  define LEAK_BASELINE_ITERS 800
#endif

static std::atomic<uint64_t> g_ok{0};
static std::atomic<uint64_t> g_err{0};

static task::Awaitable<void> harness_main(usub::Uvent* uvent)
{
    ulog::info("LEAK-baseline: harness sleeping 500ms before connect");
    co_await system::this_coroutine::sleep_for(500ms);

    auto client = std::make_shared<urpc::RpcClient>(
        urpc::RpcClientConfig{
            .host              = "localhost",
            .port              = 45900,
            .stream_factory    = nullptr,
            .ping_interval_ms  = 0,
            .socket_timeout_ms = 5000,
        });

    bool pong = co_await client->async_ping();
    if (!pong)
    {
        ulog::error("LEAK-baseline: server unreachable on localhost:45900");
        client->close();
        uvent->stop();
        co_return;
    }
    ulog::info("LEAK-baseline: ping OK, hammering Example.Echo");

    std::string body_str = "leak-baseline echo body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t*>(body_str.data()),
        body_str.size()};

    for (int i = 0; i < LEAK_BASELINE_ITERS; ++i)
    {
        auto resp = co_await client->async_call("Example.Echo", body);
        if (!resp.empty()) g_ok.fetch_add(1);
        else               g_err.fetch_add(1);

        if ((i + 1) % 100 == 0)
            ulog::info("LEAK-baseline: {}/{}", i + 1, LEAK_BASELINE_ITERS);
    }

    ulog::info("LEAK-baseline: SUMMARY ok={} err={}",
               g_ok.load(), g_err.load());
    ulog::info("LEAK-baseline: TcpRpcStream ctor={} dtor={} alive={}",
               urpc::diag::tcp_stream_ctor_count.load(),
               urpc::diag::tcp_stream_dtor_count.load(),
               urpc::diag::tcp_stream_ctor_count.load() -
                   urpc::diag::tcp_stream_dtor_count.load());

    client->close();
    co_await system::this_coroutine::sleep_for(500ms);

    ulog::info("LEAK-baseline: after client->close() ctor={} dtor={} alive={}",
               urpc::diag::tcp_stream_ctor_count.load(),
               urpc::diag::tcp_stream_dtor_count.load(),
               urpc::diag::tcp_stream_ctor_count.load() -
                   urpc::diag::tcp_stream_dtor_count.load());

    ulog::info("LEAK-baseline: stopping event loop");
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
        .queue_capacity = 16384,
        .batch_size = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };
    usub::ulog::init(cfg);
    ulog::info("LEAK-baseline: logger initialized");

    usub::Uvent uvent(1);
    system::co_spawn(harness_main(&uvent));

    ulog::info("LEAK-baseline: starting event loop");
    uvent.run();
    ulog::warn("LEAK-baseline: event loop finished");

    usub::ulog::shutdown();
    return 0;
}
