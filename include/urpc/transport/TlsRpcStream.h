//
// Created by root on 01.12.2025.
//

#ifndef URPC_TLSRPCSTREAM_H
#define URPC_TLSRPCSTREAM_H

#include <memory>
#include <array>

#include <openssl/ssl.h>

#include <uvent/net/Socket.h>
#include <uvent/tasks/Awaitable.h>
#include <uvent/utils/buffer/DynamicBuffer.h>

#include <urpc/crypto/AppCrypto.h>
#include <urpc/transport/IRPCStream.h>
#include <urpc/transport/TlsConfig.h>
#include <urpc/transport/TlsPeer.h>
#include <ulog/ulog.h>

namespace urpc
{
    class TlsRpcStream : public IRpcStream
    {
    public:
        enum class Mode
        {
            Client,
            Server
        };

        using SocketType = usub::uvent::net::TCPClientSocket;

        static std::shared_ptr<TlsRpcStream> create(
            SocketType&& sock,
            std::shared_ptr<SSL_CTX> ctx,
            Mode mode,
            TlsClientConfig client_cfg,
            TlsServerConfig server_cfg);

        static usub::uvent::task::Awaitable<std::shared_ptr<TlsRpcStream>>
        connect(std::string host,
                uint16_t port,
                const TlsClientConfig& cfg);

        static usub::uvent::task::Awaitable<std::shared_ptr<TlsRpcStream>>
        from_accepted_socket(SocketType&& socket,
                             const TlsServerConfig& cfg);

        usub::uvent::task::Awaitable<ssize_t> async_read(
            usub::uvent::utils::DynamicBuffer& buf,
            size_t max_read) override;

        usub::uvent::task::Awaitable<ssize_t> async_write(
            uint8_t* data,
            size_t len) override;

        [[nodiscard]] const RpcPeerIdentity* peer_identity() const noexcept override
        {
            return this->peer_.authenticated ? &this->peer_ : nullptr;
        }

        [[nodiscard]] bool get_app_secret_key(
            std::array<uint8_t, 32>& out_key) const noexcept override
        {
            if (!this->has_app_key_)
                return false;
            out_key = this->app_key_;
            return true;
        }

        [[nodiscard]] const AppCipherContext* app_cipher() const noexcept
        {
            return this->app_cipher_.valid ? &this->app_cipher_ : nullptr;
        }

        void shutdown() override;

        ~TlsRpcStream() override;

    private:
        TlsRpcStream(SocketType&& sock,
                     std::shared_ptr<SSL_CTX> ctx,
                     Mode mode,
                     TlsClientConfig client_cfg,
                     TlsServerConfig server_cfg);

        usub::uvent::task::Awaitable<bool> do_handshake();

        usub::uvent::task::Awaitable<bool> flush_wbio();
        usub::uvent::task::Awaitable<bool> read_into_rbio(std::size_t max_chunk);

        void fill_peer_identity();
        bool derive_app_key();

        SocketType socket_;
        std::shared_ptr<SSL_CTX> ctx_;
        SSL* ssl_;
        Mode mode_;
        RpcPeerIdentity peer_;
        TlsClientConfig client_cfg_;
        TlsServerConfig server_cfg_;
        bool shutdown_called_{false};

        std::array<uint8_t, 32> app_key_{};
        bool has_app_key_{false};

        AppCipherContext app_cipher_;
    };
}

#endif // URPC_TLSRPCSTREAM_H