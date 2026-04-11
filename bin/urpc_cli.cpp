#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"
#include "ulog/ulog.h"

#include <urpc/client/RPCClient.h>
#include <urpc/utils/Hash.h>
#include <urpc/config/Config.h>
#include <urpc/transport/TlsConfig.h>
#include <urpc/transport/TlsRpcStreamFactory.h>

using namespace usub;
using namespace usub::uvent;
using namespace std::chrono_literals;

// Exit codes (kept stable so scripts can rely on them).
//
//   0   ok
//   2   bad CLI args
//   3   ping failed (no socket timeout configured)
//   4   empty response (no per-call timeout configured)
//   110 connect/ping timeout (socket-level)
//   111 rpc call timeout (per-call deadline expired)
//
namespace exit_code {
    constexpr int ok = 0;
    constexpr int bad_args = 2;
    constexpr int ping_failed = 3;
    constexpr int empty_response = 4;
    constexpr int ping_timeout = 110;
    constexpr int call_timeout = 111;
}

static void print_hex(std::span<const uint8_t> data) {
    for (uint8_t b: data)
        std::cout << std::hex << static_cast<int>(b) << " ";
    std::cout << std::dec << "\n";
}

struct CliOptions {
    urpc::RpcClientConfig client_cfg;
    std::string method;
    std::string payload;
    uint32_t call_timeout_ms{0}; // 0 = unbounded (uses async_call)
};

task::Awaitable<void> cli_main(usub::Uvent *uvent,
                               int *exit_code_out,
                               CliOptions opts) {
    auto finish = [&](int code) {
        *exit_code_out = code;
        uvent->stop();
    };

    const int socket_timeout_ms = opts.client_cfg.socket_timeout_ms;
    const auto &host = opts.client_cfg.host;
    const auto port = opts.client_cfg.port;

    ulog::info("CLI: connecting to {}:{} (tls_factory={}, "
               "socket_timeout_ms={}, call_timeout_ms={})",
               host,
               port,
               opts.client_cfg.stream_factory ? "yes" : "no",
               socket_timeout_ms,
               opts.call_timeout_ms);

    auto client = std::make_shared<urpc::RpcClient>(
        std::move(opts.client_cfg));

    bool pong = co_await client->async_ping();
    if (!pong) {
        if (socket_timeout_ms > 0) {
            ulog::error("CLI: ping failed -- connection timeout ({} ms)",
                        socket_timeout_ms);
            client->close();
            finish(exit_code::ping_timeout);
            co_return;
        }

        ulog::error("CLI: ping failed (connection error)");
        client->close();
        finish(exit_code::ping_failed);
        co_return;
    }

    ulog::info("CLI: ping OK");

    const uint64_t id = urpc::method_id(opts.method);

    std::span<const uint8_t> req{
        reinterpret_cast<const uint8_t *>(opts.payload.data()),
        opts.payload.size()
    };

    ulog::info("CLI: calling method='{}' (id={}) payload_size={}",
               opts.method, id, opts.payload.size());

    const auto t_start = std::chrono::steady_clock::now();

    std::vector<uint8_t> resp;
    if (opts.call_timeout_ms > 0) {
        ulog::info("CLI: using async_call_with_timeout, deadline={} ms",
                   opts.call_timeout_ms);
        resp = co_await client->async_call_with_timeout(
            id, req, opts.call_timeout_ms);
    } else {
        ulog::info("CLI: using async_call (no per-call deadline)");
        resp = co_await client->async_call(id, req);
    }

    const auto t_end = std::chrono::steady_clock::now();
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t_end - t_start).count();

    if (resp.empty()) {
        if (opts.call_timeout_ms > 0
            && elapsed + 5 >= static_cast<long long>(opts.call_timeout_ms)) {
            ulog::error("CLI: rpc call TIMED OUT after {} ms "
                        "(deadline was {} ms) -- a Cancel frame was "
                        "sent to the server",
                        elapsed,
                        opts.call_timeout_ms);
            client->close();
            finish(exit_code::call_timeout);
            co_return;
        }

        if (socket_timeout_ms > 0) {
            ulog::error("CLI: empty response after {} ms "
                        "(socket timeout was {} ms)",
                        elapsed,
                        socket_timeout_ms);
            client->close();
            finish(exit_code::ping_timeout);
            co_return;
        }

        ulog::error("CLI: empty response after {} ms "
                    "(no data from server)",
                    elapsed);
        client->close();
        finish(exit_code::empty_response);
        co_return;
    }

    if (opts.call_timeout_ms > 0) {
        ulog::info("CLI: rpc call completed in {} ms (deadline was "
                   "{} ms, {} ms to spare)",
                   elapsed,
                   opts.call_timeout_ms,
                   static_cast<long long>(opts.call_timeout_ms) - elapsed);
    } else {
        ulog::info("CLI: rpc call completed in {} ms", elapsed);
    }

    std::string out(reinterpret_cast<const char *>(resp.data()),
                    resp.size());

    ulog::info("CLI: raw response size={}", resp.size());
    std::cout << "\n---- RESPONSE (utf8) ----\n" << out << "\n";
    std::cout << "\n---- RESPONSE (hex) ----\n";
    print_hex(resp);

    client->close();
    finish(exit_code::ok);
    co_return;
}

static void print_usage() {
    std::cout
            << "Usage:\n"
            << "  urpc_cli --host 127.0.0.1 --port 45900 "
            << "--method Example.Echo --data \"hello\" "
            << "[TLS options] [Timeout options] [AES options]\n\n"
            << "TLS options:\n"
            << "  --tls                       Enable TLS\n"
            << "  --tls-no-verify             Disable server cert verification\n"
            << "  --tls-ca <file>             CA certificate file\n"
            << "  --tls-cert <file>           Client certificate (for mTLS)\n"
            << "  --tls-key <file>            Client private key (for mTLS)\n"
            << "  --tls-server-name <n>       SNI / hostname for verification\n"
            << "                              (REQUIRED for proper hostname pinning)\n\n"
            << "Timeout options:\n"
            << "  --timeout-ms <n>            Socket inactivity / I/O timeout in ms\n"
            << "  --call-timeout-ms <n>       Per-RPC-call deadline in ms.\n"
            << "                              When set, uses async_call_with_timeout\n"
            << "                              and a Cancel frame is sent on expiry.\n\n"
            << "App-level AES options (over TLS):\n"
            << "  --aes                       Enable AES-256-GCM app-level encryption (default)\n"
            << "  --no-aes                    Disable AES-256-GCM app-level encryption\n"
            << "                              (only TLS transport encryption remains)\n\n"
            << "Exit codes:\n"
            << "  0   ok\n"
            << "  2   bad CLI args\n"
            << "  3   ping failed (no socket timeout set)\n"
            << "  4   empty response (no per-call timeout set)\n"
            << "  110 connect/ping timeout (socket-level)\n"
            << "  111 rpc call timeout (per-call deadline expired)\n";
}

int main(int argc, char **argv) {
    usub::ulog::ULogInit cfg_log{
        .trace_path = nullptr,
        .debug_path = nullptr,
        .info_path = nullptr,
        .warn_path = nullptr,
        .error_path = nullptr,
        .flush_interval_ns = 0,
        .queue_capacity = 4096,
        .batch_size = 256,
        .enable_color_stdout = true,
        .max_file_size_bytes = 0,
        .max_files = 0,
        .json_mode = false,
        .track_metrics = false
    };
    usub::ulog::init(cfg_log);

    if (argc < 4) {
        print_usage();
        usub::ulog::shutdown();
        return 1;
    }

    std::string host;
    uint16_t port = 0;
    std::string method;
    std::string data;

    bool tls_enabled = false;
    bool tls_verify_peer = true;
    std::string tls_ca;
    std::string tls_cert;
    std::string tls_key;
    std::string tls_server_name;

    int socket_timeout_ms = -1;
    uint32_t call_timeout_ms = 0;

    bool app_aes_enabled = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--method" && i + 1 < argc) method = argv[++i];
        else if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--tls") tls_enabled = true;
        else if (a == "--tls-no-verify") {
            tls_enabled = true;
            tls_verify_peer = false;
        } else if (a == "--tls-ca" && i + 1 < argc) {
            tls_enabled = true;
            tls_ca = argv[++i];
        } else if (a == "--tls-cert" && i + 1 < argc) {
            tls_enabled = true;
            tls_cert = argv[++i];
        } else if (a == "--tls-key" && i + 1 < argc) {
            tls_enabled = true;
            tls_key = argv[++i];
        } else if (a == "--tls-server-name" && i + 1 < argc) {
            tls_enabled = true;
            tls_server_name = argv[++i];
        } else if (a == "--timeout-ms" && i + 1 < argc) socket_timeout_ms = std::stoi(argv[++i]);
        else if (a == "--call-timeout-ms" && i + 1 < argc)
            call_timeout_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--aes") app_aes_enabled = true;
        else if (a == "--no-aes") app_aes_enabled = false;
        else if (a == "--help" || a == "-h") {
            print_usage();
            usub::ulog::shutdown();
            return 0;
        }
    }

    if (host.empty() || port == 0 || method.empty()) {
        ulog::error("CLI: bad args (host/port/method required)");
        usub::ulog::shutdown();
        return exit_code::bad_args;
    }

    CliOptions opts;
    opts.method = method;
    opts.payload = data;
    opts.call_timeout_ms = call_timeout_ms;

    opts.client_cfg.host = host;
    opts.client_cfg.port = port;
    opts.client_cfg.stream_factory = nullptr;
    opts.client_cfg.socket_timeout_ms = socket_timeout_ms;

    if (tls_enabled) {
        urpc::TlsClientConfig tls_cfg{};
        tls_cfg.enabled = true;
        tls_cfg.verify_peer = tls_verify_peer;
        tls_cfg.ca_cert_file = tls_ca;
        tls_cfg.client_cert_file = tls_cert;
        tls_cfg.client_key_file = tls_key;
        tls_cfg.server_name = !tls_server_name.empty() ? tls_server_name : host;
        tls_cfg.socket_timeout_ms = socket_timeout_ms;
        tls_cfg.app_encryption = app_aes_enabled;

        auto factory =
                std::make_shared<urpc::TlsRpcStreamFactory>(std::move(tls_cfg));
        opts.client_cfg.stream_factory = factory;

        ulog::info(
            "CLI: TLS enabled (verify_peer={}, ca='{}', cert='{}', key='{}', "
            "sni='{}', socket_timeout_ms={})",
            tls_verify_peer, tls_ca, tls_cert, tls_key,
            tls_server_name.empty() ? host : tls_server_name,
            socket_timeout_ms);

        ulog::info("CLI: app-level AES-256-GCM {}",
                   app_aes_enabled ? "ENABLED" : "DISABLED");

        if (tls_verify_peer && tls_server_name.empty()) {
            ulog::warn("CLI: --tls-server-name is empty; using --host as SNI. "
                "Set --tls-server-name explicitly for clarity.");
        }
    } else {
        if (!app_aes_enabled)
            ulog::info("CLI: AES flag ignored because TLS is disabled");

        ulog::info("CLI: TLS disabled, using plain TCP "
                   "(socket_timeout_ms={}), no app-level encryption",
                   socket_timeout_ms);
    }

    int exit_code_holder = exit_code::ok;
    usub::Uvent uvent(1);
    system::co_spawn(cli_main(&uvent, &exit_code_holder, std::move(opts)));
    uvent.run();

    usub::ulog::shutdown();
    return exit_code_holder;
}
