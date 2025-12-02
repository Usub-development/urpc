//
// Created by root on 12/1/25.
//

#ifndef TLSCONFIG_H
#define TLSCONFIG_H

#include <string>

namespace urpc
{
    struct TlsClientConfig
    {
        bool enabled{false};
        bool verify_peer{true};

        std::string ca_cert_file;
        std::string client_cert_file;
        std::string client_key_file;

        std::string server_name;
    };

    struct TlsServerConfig
    {
        bool enabled{false};
        bool require_client_cert{false};

        std::string ca_cert_file;
        std::string server_cert_file;
        std::string server_key_file;
    };
}

#endif //TLSCONFIG_H
