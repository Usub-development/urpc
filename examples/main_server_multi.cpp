#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <urpc/server/RPCServer.h>
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;

static std::vector<uint8_t> to_upper(const std::span<const uint8_t>& body)
{
    std::vector<uint8_t> out(body.begin(), body.end());
    for (auto& ch : out)
        ch = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

static std::vector<uint8_t> reverse_bytes(const std::span<const uint8_t>& body)
{
    std::vector<uint8_t> out(body.begin(), body.end());
    std::reverse(out.begin(), out.end());
    return out;
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
    ulog::info("SERVER: logger initialized (multi-method)");

    urpc::RpcServer server{"0.0.0.0", 45900, 4};
    ulog::info("SERVER: RpcServer created on port 45900");

    server.register_method_ct<urpc::method_id("Example.Echo")>(
        [](urpc::RpcContext&,
           std::span<const uint8_t> body)
        -> task::Awaitable<std::vector<uint8_t>>
        {
            ulog::info("SERVER: Example.Echo called, body_size={}", body.size());
            std::vector<uint8_t> out(body.begin(), body.end());
            co_return out;
        });

    server.register_method_ct<urpc::method_id("Example.Upper")>(
        [](urpc::RpcContext&,
           std::span<const uint8_t> body)
        -> task::Awaitable<std::vector<uint8_t>>
        {
            ulog::info("SERVER: Example.Upper called, body_size={}", body.size());
            co_return to_upper(body);
        });

    server.register_method_ct<urpc::method_id("Example.Reverse")>(
        [](urpc::RpcContext&,
           std::span<const uint8_t> body)
        -> task::Awaitable<std::vector<uint8_t>>
        {
            ulog::info("SERVER: Example.Reverse called, body_size={}", body.size());
            co_return reverse_bytes(body);
        });

    ulog::info("SERVER: calling server.run()");
    server.run();
    ulog::warn("SERVER: server.run() returned, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}