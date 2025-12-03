//
// Created by root on 12/1/25.
//

#ifndef TLS_H
#define TLS_H

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <ulog/ulog.h>

namespace urpc
{
    static bool verify_peer_hostname(SSL* ssl, const std::string& expected_host)
    {
        if (expected_host.empty())
            return true;

        X509* cert = SSL_get_peer_certificate(ssl);
        if (!cert)
        {
#if URPC_LOGS
            usub::ulog::error("TLS: no peer certificate");
#endif
            return false;
        }

        const int rc = X509_check_host(
            cert,
            expected_host.c_str(),
            0,
            0,
            nullptr
        );

        X509_free(cert);

        if (rc == 1)
        {
#if URPC_LOGS
            usub::ulog::info("TLS: peer hostname '{}' verified", expected_host);
#endif
            return true;
        }

#if URPC_LOGS
        usub::ulog::warn("TLS: hostname verification failed for '{}'", expected_host);
#endif
        return false;
    }
}

#endif //TLS_H