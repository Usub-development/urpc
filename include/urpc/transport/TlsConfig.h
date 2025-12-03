// TlsConfig.h

#ifndef TLSCONFIG_H
#define TLSCONFIG_H

#include <string>

namespace urpc
{
    struct TlsClientConfig
    {
        bool enabled{false};
        bool verify_peer{true};

        bool app_encryption{true};

        std::string ca_cert_file;
        std::string client_cert_file;
        std::string client_key_file;

        std::string server_name;
        int socket_timeout_ms{-1};
    };

    struct TlsServerConfig
    {
        bool enabled{false};
        bool require_client_cert{false};

        bool app_encryption{true};

        std::string ca_cert_file;
        std::string server_cert_file;
        std::string server_key_file;
        int socket_timeout_ms{-1};
    };
}

#endif //TLSCONFIG_H