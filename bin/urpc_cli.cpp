#include <iostream>
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

static void print_hex(std::span<const uint8_t> data)
{
    for (uint8_t b : data)
        std::cout << std::hex << static_cast<int>(b) << " ";
    std::cout << std::dec << "\n";
}

task::Awaitable<void> cli_main(urpc::RpcClientConfig cfg,
                               std::string method,
                               std::string payload)
{
    const int timeout_ms = cfg.socket_timeout_ms;

    ulog::info("CLI: connecting to {}:{} (tls_factory={}, timeout_ms={})",
               cfg.host,
               cfg.port,
               cfg.stream_factory ? "yes" : "no",
               timeout_ms);

    auto client = std::make_shared<urpc::RpcClient>(std::move(cfg));

    bool pong = co_await client->async_ping();
    if (!pong)
    {
        if (timeout_ms > 0)
        {
            ulog::error(
                "CLI: ping failed – connection timeout ({} ms)",
                timeout_ms);
            std::_Exit(110);
        }

        ulog::error("CLI: ping failed (connection error)");
        std::_Exit(3);
    }

    uint64_t id = urpc::method_id(method);

    std::span<const uint8_t> req{
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };

    ulog::info("CLI: calling method={} (id={}) payload_size={}",
               method, id, payload.size());

    auto resp = co_await client->async_call(id, req);

    if (resp.empty())
    {
        if (timeout_ms > 0)
        {
            ulog::error(
                "CLI: request timed out after {} ms "
                "(no response from server)",
                timeout_ms);
            std::_Exit(111);
        }

        ulog::error("CLI: empty response (no data from server)");
        std::_Exit(4);
    }

    std::string out(reinterpret_cast<const char*>(resp.data()), resp.size());

    ulog::info("CLI: raw response size={}", resp.size());
    std::cout << "\n---- RESPONSE (utf8) ----\n"
        << out << "\n";

    std::cout << "\n---- RESPONSE (hex) ----\n";
    print_hex(resp);

    client->close();
    co_return;
}

int main(int argc, char** argv)
{
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

    if (argc < 4)
    {
        std::cout << "Usage:\n"
            << "  urpc_cli --host 127.0.0.1 --port 45900 "
            << "--method Example.Echo --data \"hello\" [TLS options] [Timeout] [AES]\n\n"
            << "TLS options:\n"
            << "  --tls                       Enable TLS\n"
            << "  --tls-no-verify             Disable server cert verification\n"
            << "  --tls-ca <file>             CA certificate file\n"
            << "  --tls-cert <file>           Client certificate (for mTLS)\n"
            << "  --tls-key <file>            Client private key (for mTLS)\n"
            << "  --tls-server-name <name>    SNI / hostname for verification\n\n"
            << "Timeout options:\n"
            << "  --timeout-ms <n>            Socket inactivity/IO timeout in ms\n\n"
            << "App-level AES options (over TLS):\n"
            << "  --aes                       Enable AES-256-GCM app-level encryption (default)\n"
            << "  --no-aes                    Disable AES-256-GCM app-level encryption\n"
            << "                              (only TLS transport encryption remains)\n\n"
            << "Exit codes:\n"
            << "  3   ping failed (no timeout set)\n"
            << "  4   empty response (no timeout set)\n"
            << "  110 connect/ping timeout\n"
            << "  111 rpc call timeout\n";
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

    int timeout_ms = -1;

    bool app_aes_enabled = true;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--host")
        {
            host = argv[++i];
        }
        else if (a == "--port")
        {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (a == "--method")
        {
            method = argv[++i];
        }
        else if (a == "--data")
        {
            data = argv[++i];
        }
        else if (a == "--tls")
        {
            tls_enabled = true;
        }
        else if (a == "--tls-no-verify")
        {
            tls_enabled = true;
            tls_verify_peer = false;
        }
        else if (a == "--tls-ca" && i + 1 < argc)
        {
            tls_enabled = true;
            tls_ca = argv[++i];
        }
        else if (a == "--tls-cert" && i + 1 < argc)
        {
            tls_enabled = true;
            tls_cert = argv[++i];
        }
        else if (a == "--tls-key" && i + 1 < argc)
        {
            tls_enabled = true;
            tls_key = argv[++i];
        }
        else if (a == "--tls-server-name" && i + 1 < argc)
        {
            tls_enabled = true;
            tls_server_name = argv[++i];
        }
        else if (a == "--timeout-ms" && i + 1 < argc)
        {
            timeout_ms = std::stoi(argv[++i]);
        }
        else if (a == "--aes")
        {
            app_aes_enabled = true;
        }
        else if (a == "--no-aes")
        {
            app_aes_enabled = false;
        }
    }

    if (host.empty() || port == 0 || method.empty())
    {
        ulog::error("CLI: bad args (host/port/method required)");
        return 2;
    }

    urpc::RpcClientConfig client_cfg;
    client_cfg.host = host;
    client_cfg.port = port;
    client_cfg.stream_factory = nullptr;
    client_cfg.socket_timeout_ms = timeout_ms;

    if (tls_enabled)
    {
        urpc::TlsClientConfig tls_cfg{};
        tls_cfg.enabled = true;
        tls_cfg.verify_peer = tls_verify_peer;
        tls_cfg.ca_cert_file = tls_ca;
        tls_cfg.client_cert_file = tls_cert;
        tls_cfg.client_key_file = tls_key;
        tls_cfg.server_name = !tls_server_name.empty() ? tls_server_name : host;
        tls_cfg.socket_timeout_ms = timeout_ms;
        tls_cfg.app_encryption = app_aes_enabled;

        auto factory =
            std::make_shared<urpc::TlsRpcStreamFactory>(std::move(tls_cfg));

        client_cfg.stream_factory = factory;

        ulog::info(
            "CLI: TLS enabled (verify_peer={}, ca='{}', cert='{}', key='{}', "
            "sni='{}', timeout_ms={})",
            tls_verify_peer,
            tls_ca,
            tls_cert,
            tls_key,
            tls_server_name.empty() ? host : tls_server_name,
            timeout_ms);

        ulog::info(
            "CLI: app-level AES-256-GCM {}",
            app_aes_enabled ? "ENABLED" : "DISABLED");

        if (!app_aes_enabled)
        {
            ulog::info(
                "CLI: only TLS transport encryption will be used (no body AES)");
        }
    }
    else
    {
        if (!app_aes_enabled)
        {
            ulog::info(
                "CLI: AES flag ignored because TLS is disabled");
        }

        ulog::info(
            "CLI: TLS disabled, using plain TCP (timeout_ms={}), "
            "no app-level encryption",
            timeout_ms);
    }

    Uvent uvent(1);
    system::co_spawn(cli_main(std::move(client_cfg), method, data));
    uvent.run();

    return 0;
}