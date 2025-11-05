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
#include "UrpcSettings.h"
#include "uvent/Uvent.h"

namespace urpc
{
    inline usub::uvent::task::Awaitable<void> wait_readable(int fd)
    {
        co_await usub::uvent::system::this_coroutine::wait_readable(fd);
        co_return;
    }

    inline usub::uvent::task::Awaitable<void> wait_writable(int fd)
    {
        co_await usub::uvent::system::this_coroutine::wait_writable(fd);
        co_return;
    }

    class OpenSslTransport final : public ITransport
    {
    public:
        enum class Mode { TLS, MTLS };

        OpenSslTransport(int fd, SSL_CTX* ctx, Mode mode, bool server_side)
            : fd_(fd), ctx_(ctx), mode_(mode), server_side_(server_side)
        {
            ssl_ = SSL_new(ctx_);
            SSL_set_fd(ssl_, fd_);
            if (server_side_) SSL_set_accept_state(ssl_);
            else SSL_set_connect_state(ssl_);
            fr_.transport_bits = flags_set_transport(0, (mode_ == Mode::MTLS) ? F_TP_MTLS : F_TP_TLS);
        }

        ~OpenSslTransport() override
        {
            close();
            if (ssl_) SSL_free(ssl_);
        }

        [[nodiscard]] uint8_t transport_bits() const noexcept override { return fr_.transport_bits; }
        [[nodiscard]] bool alive() const noexcept override { return alive_; }
        [[nodiscard]] int native_handle() const noexcept override { return fd_; }

        void close() noexcept override
        {
            if (alive_)
            {
                alive_ = false;
                SSL_shutdown(ssl_);
            }
        }

        void set_coalescing(CoalesceConfig cfg) override { coal_ = cfg; }

        usub::uvent::task::Awaitable<bool> handshake()
        {
            for (;;)
            {
                const int r = SSL_do_handshake(ssl_);
                if (r == 1) co_return true;
                const int e = SSL_get_error(ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(fd_);
                    continue;
                }
                co_return false;
            }
        }

        // Channel binding helper (EXPORTER)
        std::optional<ChannelBindingMeta> channel_binding_meta()
        {
            std::string key(32, '\0');
            if (SSL_export_keying_material(ssl_, key.data(), key.size(),
                                           "EXPORTER-urpc", strlen("EXPORTER-urpc"), nullptr, 0, 0) != 1)
                return std::nullopt;

            ChannelBindingMeta cb;
            cb.type = CbType::TlsExporter;
            cb.data.assign(key.begin(), key.end());
            return cb;
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr((mode_ == Mode::MTLS) ? TransportMode::MTLS : TransportMode::TLS);
            h.flags = fr_.transport_bits;
            if (!meta_bin.empty()) h.flags |= F_CB_PRESENT;
            std::string frame = make_frame(h, std::move(meta_bin), {});
            co_return co_await send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<bool> send_frame(std::string frame) override
        {
            if (!alive_) co_return false;
            tx_buf_.append(frame);
            if (tx_buf_.size() >= coal_.flush_threshold_bytes)
            {
                co_await flush();
            }
            else if (!flush_armed_)
            {
                flush_armed_ = true;
                co_await usub::uvent::system::this_coroutine::sleep_for(
                    std::chrono::milliseconds(coal_.flush_interval_ms));
                flush_armed_ = false;
                if (!tx_buf_.empty()) co_await flush();
            }
            co_return true;
        }

        usub::uvent::task::Awaitable<void> flush() override
        {
            if (tx_buf_.empty()) co_return;
            const char* p = tx_buf_.data();
            size_t n = tx_buf_.size();
            while (n)
            {
                const int r = SSL_write(ssl_, p, static_cast<int>(n));
                if (r > 0)
                {
                    p += r;
                    n -= size_t(r);
                    continue;
                }
                const int e = SSL_get_error(ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(fd_);
                    continue;
                }
                tx_buf_.erase(0, p - tx_buf_.data());
                co_return;
            }
            tx_buf_.clear();
            co_return;
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!(co_await read_exact_ssl(HDR_SIZE))) co_return std::nullopt;
            const uint32_t len_field = get_le32(rx_.data());
            if (len_field > MAX_FRAME_NO_LEN + HDR_NO_LEN) co_return std::nullopt;
            const size_t total = HDR_SIZE + size_t(len_field);
            if (rx_.size() < total)
            {
                if (!(co_await read_exact_ssl(total - rx_.size()))) co_return std::nullopt;
            }
            ParsedFrame pf{};
            if (!parse_frame(rx_.data(), total, pf)) co_return std::nullopt;
            if (rx_.size() > total) rx_.erase(0, total);
            else rx_.clear();
            co_return pf;
        }

        static void enable_server_verify(SSL_CTX* ctx)
        {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }

        static void enable_client_verify(SSL_CTX* ctx) { SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr); }

    private:
        usub::uvent::task::Awaitable<bool> read_exact_ssl(size_t need)
        {
            const size_t start = rx_.size();
            rx_.resize(start + need);
            size_t off = 0;
            while (off < need)
            {
                const int r = SSL_read(ssl_, rx_.data() + start + off, int(need - off));
                if (r > 0)
                {
                    off += size_t(r);
                    continue;
                }
                const int e = SSL_get_error(ssl_, r);
                if (e == SSL_ERROR_WANT_READ)
                {
                    co_await wait_readable(fd_);
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    co_await wait_writable(fd_);
                    continue;
                }
                rx_.resize(start + off);
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

        CoalesceConfig coal_{};
        std::string tx_buf_;
        bool flush_armed_{false};
    };
} // namespace urpc

#endif // OPENSSLTRANSPORT_H
