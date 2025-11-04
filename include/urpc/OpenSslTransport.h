#ifndef OPENSSLTRANSPORT_H
#define OPENSSLTRANSPORT_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <optional>
#include <span>
#include <cstdint>
#include <cstring>
#include "Wire.h"
#include "Transport.h"

namespace urpc
{
    usub::uvent::task::Awaitable<void> wait_readable(int fd);
    usub::uvent::task::Awaitable<void> wait_writable(int fd);

    class OpenSslTransport final : public ITransport
    {
    public:
        enum class Mode { TLS, MTLS };

        OpenSslTransport(int fd, SSL_CTX* ctx, Mode mode, bool server_side)
            : fd_(fd), ctx_(ctx), mode_(mode), server_side_(server_side)
        {
            this->ssl_ = SSL_new(this->ctx_);
            SSL_set_fd(this->ssl_, this->fd_);
            if (this->server_side_) SSL_set_accept_state(this->ssl_);
            else SSL_set_connect_state(this->ssl_);
            this->fr_.transport_bits = flags_set_transport(0, (this->mode_ == Mode::MTLS) ? F_TP_MTLS : F_TP_TLS);
        }

        ~OpenSslTransport() override
        {
            this->close();
            if (this->ssl_) SSL_free(this->ssl_);
        }

        [[nodiscard]] uint8_t transport_bits() const noexcept override { return this->fr_.transport_bits; }
        [[nodiscard]] bool alive() const noexcept override { return this->alive_; }
        [[nodiscard]] int native_handle() const noexcept override { return this->fd_; }

        void close() noexcept override
        {
            if (this->alive_)
            {
                this->alive_ = false;
                SSL_shutdown(this->ssl_);
            }
        }

        usub::uvent::task::Awaitable<bool> handshake()
        {
            for (;;)
            {
                const int r = SSL_do_handshake(this->ssl_);
                if (r == 1) co_return true;
                const int e = SSL_get_error(this->ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(this->fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(this->fd_);
                    continue;
                }
                co_return false;
            }
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr((this->mode_ == Mode::MTLS) ? TransportMode::MTLS : TransportMode::TLS);
            h.flags = this->fr_.transport_bits;
            std::string frame = make_frame(h, std::move(meta_bin), {});
            co_return co_await this->send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<bool> send_frame(std::string frame) override
        {
            if (!this->alive_) co_return false;
            const char* p = frame.data();
            size_t n = frame.size();
            while (n)
            {
                const int r = SSL_write(this->ssl_, p, static_cast<int>(n));
                if (r > 0)
                {
                    p += r;
                    n -= static_cast<size_t>(r);
                    continue;
                }
                const int e = SSL_get_error(this->ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(this->fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(this->fd_);
                    continue;
                }
                co_return false;
            }
            co_return true;
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!(co_await this->read_exact_ssl(HDR_SIZE))) co_return std::nullopt;
            const uint32_t len_field = get_le32(this->rx_.data());
            const size_t total = HDR_SIZE + static_cast<size_t>(len_field);
            if (this->rx_.size() < total)
            {
                if (!(co_await this->read_exact_ssl(total - this->rx_.size()))) co_return std::nullopt;
            }
            ParsedFrame pf{};
            if (!parse_frame(this->rx_.data(), total, pf)) co_return std::nullopt;
            if (this->rx_.size() > total) this->rx_.erase(0, total);
            else this->rx_.clear();
            co_return pf;
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> rpc_call(
            uint64_t method, std::string meta, std::string body) override
        {
            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::REQUEST);
            h.method = method;
            h.flags = this->fr_.transport_bits;
            std::string frame = make_frame(h, std::move(meta), std::move(body));
            if (!(co_await this->send_frame(std::move(frame)))) co_return std::nullopt;
            co_return co_await this->recv_frame();
        }

        static void enable_server_verify(SSL_CTX* ctx)
        {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }

        static void enable_client_verify(SSL_CTX* ctx) { SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr); }

        std::optional<std::string> exporter_tls_unique(size_t len = 32)
        {
            std::string out(len, '\0');
            if (SSL_export_keying_material(this->ssl_, out.data(), static_cast<size_t>(out.size()),
                                           "EXPORTER-urpc", strlen("EXPORTER-urpc"), nullptr, 0, 0) == 1)
                return out;
            return std::nullopt;
        }

    private:
        usub::uvent::task::Awaitable<bool> read_exact_ssl(size_t need)
        {
            const size_t start = this->rx_.size();
            this->rx_.resize(start + need);
            size_t off = 0;
            while (off < need)
            {
                const int r = SSL_read(this->ssl_, this->rx_.data() + start + off, static_cast<int>(need - off));
                if (r > 0)
                {
                    off += static_cast<size_t>(r);
                    continue;
                }
                const int e = SSL_get_error(this->ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(this->fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(this->fd_);
                    continue;
                }
                co_return false;
            }
            co_return true;
        }

    private:
        int fd_{-1};
        SSL_CTX* ctx_{nullptr};
        SSL* ssl_{nullptr};
        Mode mode_{Mode::TLS};
        bool server_side_{false};
        bool alive_{true};
        Framer fr_{};
        std::string rx_;
    };
} // namespace urpc

#endif // OPENSSLTRANSPORT_H
