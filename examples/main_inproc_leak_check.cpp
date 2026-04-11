//
// main_inproc_leak_check.cpp
//
// Usage:
//
//   cmake --build build-asan -j --target urpc_inproc_leak_check
//   ASAN_OPTIONS=detect_leaks=1 \
//       ./build-asan/urpc_inproc_leak_check 2>&1 | tee inproc-leak.log
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

#include <urpc/server/RPCServer.h>
#include <urpc/client/RPCClient.h>
#include <urpc/transport/TCPStream.h>  // for urpc::diag counters
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

#ifndef INPROC_ECHO_ITERS
#  define INPROC_ECHO_ITERS 800
#endif

#ifndef INPROC_PORT
#  define INPROC_PORT 45901
#endif

#ifndef INPROC_THREADS
#  define INPROC_THREADS 2
#endif

static std::atomic<uint64_t> g_server_echo_count{0};
static std::atomic<uint64_t> g_client_ok{0};
static std::atomic<uint64_t> g_client_err{0};

static task::Awaitable<void> client_driver(usub::Uvent* uvent)
{
    ulog::info("CLIENT-DRV: sleeping 500ms before connect");
    co_await system::this_coroutine::sleep_for(500ms);

    auto client = std::make_shared<urpc::RpcClient>(
        urpc::RpcClientConfig{
            .host              = "127.0.0.1",
            .port              = INPROC_PORT,
            .stream_factory    = nullptr,
            .ping_interval_ms  = 0,
            .socket_timeout_ms = 5000,
        });

    bool pong = co_await client->async_ping();
    if (!pong)
    {
        ulog::error("CLIENT-DRV: in-process server unreachable on 127.0.0.1:{}",
                    static_cast<int>(INPROC_PORT));
        client->close();
        uvent->stop();
        co_return;
    }
    ulog::info("CLIENT-DRV: ping OK, hammering Example.Echo x{}",
               static_cast<int>(INPROC_ECHO_ITERS));

    std::string body_str = "inproc echo body";
    std::span<const uint8_t> body{
        reinterpret_cast<const uint8_t*>(body_str.data()),
        body_str.size()};

    for (int i = 0; i < INPROC_ECHO_ITERS; ++i)
    {
        auto resp = co_await client->async_call("Example.Echo", body);
        if (!resp.empty()) g_client_ok.fetch_add(1);
        else               g_client_err.fetch_add(1);

        if ((i + 1) % 100 == 0)
            ulog::info("CLIENT-DRV: {}/{}",
                       i + 1, static_cast<int>(INPROC_ECHO_ITERS));
    }

    ulog::info(
        "CLIENT-DRV: SUMMARY client_ok={} client_err={} server_echo={}",
        g_client_ok.load(),
        g_client_err.load(),
        g_server_echo_count.load());
    ulog::info("CLIENT-DRV: TcpRpcStream ctor={} dtor={} alive={}",
               urpc::diag::tcp_stream_ctor_count.load(),
               urpc::diag::tcp_stream_dtor_count.load(),
               urpc::diag::tcp_stream_ctor_count.load() -
                   urpc::diag::tcp_stream_dtor_count.load());

    client->close();

    co_await system::this_coroutine::sleep_for(500ms);

    ulog::info("CLIENT-DRV: after close ctor={} dtor={} alive={}",
               urpc::diag::tcp_stream_ctor_count.load(),
               urpc::diag::tcp_stream_dtor_count.load(),
               urpc::diag::tcp_stream_ctor_count.load() -
                   urpc::diag::tcp_stream_dtor_count.load());

    ulog::info("CLIENT-DRV: stopping event loop");
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
    ulog::info("INPROC: logger initialized [INPROC_LEAK_CHECK]");

    usub::Uvent uvent(INPROC_THREADS);
    ulog::info("INPROC: Uvent created with {} threads",
               static_cast<int>(INPROC_THREADS));

    urpc::RpcServerConfig srv_cfg{
        .host       = "127.0.0.1",
        .port       = INPROC_PORT,
        .threads    = INPROC_THREADS,   // cosmetic; we drive uvent ourselves
        .timeout_ms = 5000,
    };
    urpc::RpcServer server{srv_cfg};
    ulog::info("INPROC: RpcServer created on port {}",
               static_cast<int>(INPROC_PORT));

    server.register_method_ct<urpc::method_id("Example.Echo")>(
        [](urpc::RpcContext&,
           std::span<const uint8_t> body)
    -> task::Awaitable<std::vector<uint8_t>> {
            g_server_echo_count.fetch_add(1, std::memory_order_relaxed);
            std::vector<uint8_t> out(body.begin(), body.end());
            co_return out;
        });

    ulog::info("INPROC: Example.Echo registered");

    uvent.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage*)
    {
        system::co_spawn_static(server.run_async(), threadIndex);
    });
    ulog::info("INPROC: server.run_async() spawned on {} threads",
               static_cast<int>(INPROC_THREADS));

    system::co_spawn(client_driver(&uvent));
    ulog::info("INPROC: client_driver spawned");

    ulog::info("INPROC: starting event loop");
    uvent.run();
    ulog::warn("INPROC: event loop finished");

    ulog::info(
        "INPROC: final stats client_ok={} client_err={} server_echo={}",
        g_client_ok.load(),
        g_client_err.load(),
        g_server_echo_count.load());

    usub::ulog::shutdown();

    return 0;
}
