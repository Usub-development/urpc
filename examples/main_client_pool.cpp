// examples/main_client_pool.cpp

#include <chrono>
#include <span>
#include <string>
#include <vector>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/client/RPCClientPool.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

static task::Awaitable<void> client_worker(
    urpc::RpcClientPool& pool,
    std::size_t worker_id)
{
    auto lease = pool.try_acquire();
    urpc::RpcClient& client = lease.client;

    ulog::info("WORKER[{}]: acquired client idx={}", worker_id, lease.index);

    std::string payload_str =
        "hello from worker " + std::to_string(worker_id);
    std::span<const uint8_t> req{
        reinterpret_cast<const uint8_t*>(payload_str.data()),
        payload_str.size()
    };

    ulog::info("WORKER[{}]: sending request, body_size={}", worker_id, req.size());

    auto resp = co_await client.async_call("Example.Echo", req);

    ulog::info("WORKER[{}]: resp.size={}", worker_id, resp.size());
    if (!resp.empty())
    {
        std::string resp_str(
            reinterpret_cast<const char*>(resp.data()),
            resp.size());
        ulog::info("WORKER[{}]: resp='{}'", worker_id, resp_str);
    }

    co_return;
}

int main()
{
    usub::ulog::ULogInit cfg_log{
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
    usub::ulog::init(cfg_log);
    ulog::info("CLIENT_POOL: logger initialized");

    urpc::RpcClientPoolConfig pool_cfg{
        .host = "127.0.0.1",
        .port = 45900,
        .stream_factory = nullptr,
        .socket_timeout_ms = -1,
        .ping_interval_ms = 0,
        .max_clients = 4
    };

    urpc::RpcClientPool pool{pool_cfg};

    Uvent uvent(1);

    const std::size_t workers = 16;
    for (std::size_t i = 0; i < workers; ++i)
    {
        system::co_spawn(client_worker(pool, i));
    }

    ulog::info("CLIENT_POOL: starting event loop");
    uvent.run();
    ulog::warn("CLIENT_POOL: event loop finished, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}
