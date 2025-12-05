// main_server.cpp

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/server/RPCServer.h>
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;

int main()
{
    // ulog -> stdout
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
    ulog::info("SERVER: logger initialized");

    urpc::RpcServerConfig config{
        .host = "0.0.0.0",
        .port = 45900
    };
    urpc::RpcServer server{config};
    ulog::info("SERVER: RpcServer created");

    server.register_method_ct<urpc::method_id("Example.Echo")>(
        [](urpc::RpcContext& ctx,
           std::span<const uint8_t> body)
        -> task::Awaitable<std::vector<uint8_t>>
        {
            ulog::info("SERVER: Example.Echo called, body_size={}", body.size());

            std::vector<uint8_t> out(body.begin(), body.end());

            co_return out;
        });

    server.register_method_ct<urpc::method_id("Example.String")>(
        [](urpc::RpcContext&,
           std::span<const uint8_t> body)
        -> usub::uvent::task::Awaitable<std::string>
        {
            std::string in(reinterpret_cast<const char*>(body.data()), body.size());
            co_return "echo: " + in;
        });


    ulog::info("SERVER: calling server.run()");
    server.run();
    ulog::warn("SERVER: server.run() returned, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}
