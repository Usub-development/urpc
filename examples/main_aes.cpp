#include "uvent/Uvent.h"

#include "ulog/ulog.h"

#include <urpc/server/RPCServer.h>
#include <urpc/utils/Hash.h>
#include <urpc/transport/TlsConfig.h>
#include <urpc/transport/TlsRpcStreamFactory.h>

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

    // TLS + app-level AES
    urpc::TlsServerConfig tls_srv_cfg{};
    tls_srv_cfg.enabled = true;
    tls_srv_cfg.require_client_cert = false;
    tls_srv_cfg.ca_cert_file = "../certs/ca.crt";
    tls_srv_cfg.server_cert_file = "../certs/server.crt";
    tls_srv_cfg.server_key_file = "../certs/server.key";
    tls_srv_cfg.socket_timeout_ms = -1;
    tls_srv_cfg.app_encryption = true;

    auto tls_factory =
        std::make_shared<urpc::TlsRpcStreamFactory>(urpc::TlsClientConfig{});
    tls_factory->set_server_cfg(tls_srv_cfg);

    urpc::RpcServerConfig config{
        .host = "0.0.0.0",
        .port = 45900,
        .threads = 1,
        .stream_factory = tls_factory
    };

    ulog::info(
        "SERVER: RpcServerConfig created: host={}, port={}, TLS enabled={}, app_encryption={}",
        config.host,
        config.port,
        tls_srv_cfg.enabled,
        tls_srv_cfg.app_encryption);

    urpc::RpcServer server{config};
    ulog::info("SERVER: RpcServer created");

    server.register_method_ct<urpc::method_id("Example.Echo")>(
        [](urpc::RpcContext& ctx,
           std::span<const uint8_t> body)
        -> task::Awaitable<std::vector<uint8_t>>
        {
            if (ctx.peer)
            {
                ulog::info(
                    "SERVER: Example.Echo peer: authenticated={}, cn='{}', subject='{}'",
                    ctx.peer->authenticated,
                    ctx.peer->common_name,
                    ctx.peer->subject);
            }
            else
            {
                ulog::info("SERVER: Example.Echo: no peer identity");
            }

            ulog::info("SERVER: Example.Echo called, body_size={}", body.size());

            std::vector<uint8_t> out(body.begin(), body.end());
            co_return out;
        });

    ulog::info("SERVER: calling server.run()");
    server.run();
    ulog::warn("SERVER: server.run() returned, shutting down logger");

    usub::ulog::shutdown();
    return 0;
}
