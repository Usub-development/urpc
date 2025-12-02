//
// Created by root on 01.12.2025.
//

#include <urpc/transport/TlsRpcStream.h>

#include <openssl/x509v3.h>
#include <openssl/err.h>

namespace urpc
{
    using namespace usub::uvent;

    static void log_last_ssl_error(const char* where)
    {
        unsigned long err = ERR_get_error();
        if (err != 0)
        {
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            usub::ulog::error("TlsRpcStream: {}: {}", where, buf);
        }
        else
        {
            usub::ulog::error("TlsRpcStream: {}: unknown SSL error", where);
        }
    }

    static std::string x509_name_to_string(X509_NAME* name)
    {
        if (!name) return {};
        char* cstr = X509_NAME_oneline(name, nullptr, 0);
        if (!cstr) return {};
        std::string out{cstr};
        OPENSSL_free(cstr);
        return out;
    }

    static std::string get_common_name(X509* cert)
    {
        if (!cert) return {};

        X509_NAME* subj = X509_get_subject_name(cert);
        if (!subj) return {};

        int idx = X509_NAME_get_index_by_NID(subj, NID_commonName, -1);
        if (idx < 0) return {};

        X509_NAME_ENTRY* e = X509_NAME_get_entry(subj, idx);
        if (!e) return {};

        ASN1_STRING* data = X509_NAME_ENTRY_get_data(e);
        if (!data) return {};

        unsigned char* utf8 = nullptr;
        int len = ASN1_STRING_to_UTF8(&utf8, data);
        if (len < 0 || !utf8) return {};

        std::string out(reinterpret_cast<char*>(utf8),
                        static_cast<std::size_t>(len));
        OPENSSL_free(utf8);
        return out;
    }

    static std::vector<std::string> get_dns_sans(X509* cert)
    {
        std::vector<std::string> out;
        if (!cert) return out;

        STACK_OF(GENERAL_NAME)* san_names = static_cast<STACK_OF(GENERAL_NAME)*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));

        if (!san_names) return out;

        int count = sk_GENERAL_NAME_num(san_names);
        for (int i = 0; i < count; ++i)
        {
            const GENERAL_NAME* gen = sk_GENERAL_NAME_value(san_names, i);
            if (!gen || gen->type != GEN_DNS) continue;

            ASN1_IA5STRING* dns = gen->d.dNSName;
            if (!dns) continue;

            const unsigned char* data = ASN1_STRING_get0_data(dns);
            int len = ASN1_STRING_length(dns);
            if (!data || len <= 0) continue;

            out.emplace_back(
                reinterpret_cast<const char*>(data),
                static_cast<std::size_t>(len));
        }

        sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
        return out;
    }

    static std::string cert_to_pem(X509* cert)
    {
        if (!cert) return {};
        BIO* mem = BIO_new(BIO_s_mem());
        if (!mem) return {};
        if (!PEM_write_bio_X509(mem, cert))
        {
            BIO_free(mem);
            return {};
        }
        char* data = nullptr;
        long len = BIO_get_mem_data(mem, &data);
        std::string pem;
        if (len > 0 && data)
            pem.assign(data, static_cast<std::size_t>(len));
        BIO_free(mem);
        return pem;
    }

    // ===== SSL_CTX фабрики =====

    static std::shared_ptr<SSL_CTX> make_client_ctx(const TlsClientConfig& cfg)
    {
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* raw = SSL_CTX_new(method);
        if (!raw)
        {
            log_last_ssl_error("SSL_CTX_new(client)");
            return {};
        }

        std::shared_ptr<SSL_CTX> ctx(raw, [](SSL_CTX* p) { SSL_CTX_free(p); });

        if (!cfg.ca_cert_file.empty())
        {
            if (!SSL_CTX_load_verify_locations(
                ctx.get(), cfg.ca_cert_file.c_str(), nullptr))
            {
                log_last_ssl_error("SSL_CTX_load_verify_locations(client)");
                return {};
            }
        }
        else
        {
            if (!SSL_CTX_set_default_verify_paths(ctx.get()))
            {
                log_last_ssl_error("SSL_CTX_set_default_verify_paths(client)");
            }
        }

        if (!cfg.client_cert_file.empty() && !cfg.client_key_file.empty())
        {
            if (!SSL_CTX_use_certificate_file(
                ctx.get(), cfg.client_cert_file.c_str(),
                SSL_FILETYPE_PEM))
            {
                log_last_ssl_error("SSL_CTX_use_certificate_file(client)");
                return {};
            }

            if (!SSL_CTX_use_PrivateKey_file(
                ctx.get(), cfg.client_key_file.c_str(),
                SSL_FILETYPE_PEM))
            {
                log_last_ssl_error("SSL_CTX_use_PrivateKey_file(client)");
                return {};
            }

            if (!SSL_CTX_check_private_key(ctx.get()))
            {
                usub::ulog::error(
                    "TlsRpcStream: client key does not match certificate");
                return {};
            }
        }

        if (cfg.verify_peer)
            SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
        else
            SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

        return ctx;
    }

    static std::shared_ptr<SSL_CTX> make_server_ctx(const TlsServerConfig& cfg)
    {
        const SSL_METHOD* method = TLS_server_method();
        SSL_CTX* raw = SSL_CTX_new(method);
        if (!raw)
        {
            log_last_ssl_error("SSL_CTX_new(server)");
            return {};
        }

        std::shared_ptr<SSL_CTX> ctx(raw, [](SSL_CTX* p) { SSL_CTX_free(p); });

        if (!SSL_CTX_use_certificate_file(
            ctx.get(), cfg.server_cert_file.c_str(),
            SSL_FILETYPE_PEM))
        {
            log_last_ssl_error("SSL_CTX_use_certificate_file(server)");
            return {};
        }

        if (!SSL_CTX_use_PrivateKey_file(
            ctx.get(), cfg.server_key_file.c_str(),
            SSL_FILETYPE_PEM))
        {
            log_last_ssl_error("SSL_CTX_use_PrivateKey_file(server)");
            return {};
        }

        if (!SSL_CTX_check_private_key(ctx.get()))
        {
            usub::ulog::error(
                "TlsRpcStream: server key does not match certificate");
            return {};
        }

        if (!cfg.ca_cert_file.empty())
        {
            if (!SSL_CTX_load_verify_locations(
                ctx.get(), cfg.ca_cert_file.c_str(), nullptr))
            {
                log_last_ssl_error("SSL_CTX_load_verify_locations(server)");
                return {};
            }
        }

        if (cfg.require_client_cert)
        {
            SSL_CTX_set_verify(
                ctx.get(),
                SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                nullptr);
        }
        else
        {
            SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
        }

        return ctx;
    }

    // ===== TlsRpcStream =====

    TlsRpcStream::TlsRpcStream(SocketType&& sock,
                               std::shared_ptr<SSL_CTX> ctx,
                               Mode mode,
                               TlsClientConfig client_cfg,
                               TlsServerConfig server_cfg)
        : socket_(std::move(sock))
          , ctx_(std::move(ctx))
          , ssl_(nullptr)
          , mode_(mode)
          , client_cfg_(std::move(client_cfg))
          , server_cfg_(std::move(server_cfg))
    {
#if URPC_LOGS
        usub::ulog::info(
            "TlsRpcStream ctor: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
#endif

        this->ssl_ = SSL_new(this->ctx_.get());
        if (!this->ssl_)
        {
            log_last_ssl_error("SSL_new");
            return;
        }

        // используем memory BIO, SSL сам сокет не трогает
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(this->ssl_, rbio, wbio);

        if (this->mode_ == Mode::Client)
        {
            SSL_set_connect_state(this->ssl_);
            if (!this->client_cfg_.server_name.empty())
            {
                SSL_set_tlsext_host_name(
                    this->ssl_,
                    this->client_cfg_.server_name.c_str());
            }
        }
        else
        {
            SSL_set_accept_state(this->ssl_);
        }
    }

    TlsRpcStream::~TlsRpcStream()
    {
        this->shutdown();
    }

    void TlsRpcStream::shutdown()
    {
        if (this->shutdown_called_)
            return;
        this->shutdown_called_ = true;

#if URPC_LOGS
        usub::ulog::info(
            "TlsRpcStream::shutdown: this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
#endif

        if (this->ssl_)
        {
            // Пытаемся корректно отправить close_notify
            SSL_shutdown(this->ssl_);
            SSL_free(this->ssl_);
            this->ssl_ = nullptr;
        }

        this->socket_.shutdown();
    }

    void TlsRpcStream::fill_peer_identity()
    {
        this->peer_ = RpcPeerIdentity{};
        X509* cert = nullptr;

        if (this->mode_ == Mode::Client)
        {
            if (this->client_cfg_.verify_peer)
            {
                long res = SSL_get_verify_result(this->ssl_);
                if (res == X509_V_OK)
                    this->peer_.authenticated = true;
            }
            cert = SSL_get_peer_certificate(this->ssl_);
        }
        else
        {
            cert = SSL_get_peer_certificate(this->ssl_);
            if (cert && this->server_cfg_.require_client_cert)
            {
                long res = SSL_get_verify_result(this->ssl_);
                if (res == X509_V_OK)
                    this->peer_.authenticated = true;
            }
        }

        if (!cert)
            return;

        this->peer_.subject = x509_name_to_string(
            X509_get_subject_name(cert));
        this->peer_.issuer = x509_name_to_string(
            X509_get_issuer_name(cert));
        this->peer_.common_name = get_common_name(cert);
        this->peer_.dns_sans = get_dns_sans(cert);
        this->peer_.pem = cert_to_pem(cert);

        X509_free(cert);
    }

    usub::uvent::task::Awaitable<bool> TlsRpcStream::flush_wbio()
    {
        BIO* wbio = SSL_get_wbio(this->ssl_);
        if (!wbio) co_return true;

        static constexpr std::size_t kChunk = 16 * 1024;
        std::vector<uint8_t> tmp;
        tmp.resize(kChunk);

        while (true)
        {
            int pending = BIO_ctrl_pending(wbio);
            if (pending <= 0)
                break;

            int to_read = pending < static_cast<int>(kChunk)
                              ? pending
                              : static_cast<int>(kChunk);

            int n = BIO_read(wbio, tmp.data(), to_read);
            if (n <= 0)
                break;

            ssize_t wr = co_await this->socket_.async_write(
                tmp.data(),
                static_cast<std::size_t>(n));
            if (wr <= 0)
            {
#if URPC_LOGS
                usub::ulog::warn(
                    "TlsRpcStream::flush_wbio: async_write failed wr={}", wr);
#endif
                co_return false;
            }
        }

        co_return true;
    }

    usub::uvent::task::Awaitable<bool> TlsRpcStream::read_into_rbio(
        std::size_t max_chunk)
    {
        utils::DynamicBuffer buf;
        buf.reserve(max_chunk);
        buf.clear();

        ssize_t rd = co_await this->socket_.async_read(buf, max_chunk);
        if (rd <= 0)
        {
#if URPC_LOGS
            usub::ulog::warn(
                "TlsRpcStream::read_into_rbio: async_read rd={}", rd);
#endif
            co_return false;
        }

        BIO* rbio = SSL_get_rbio(this->ssl_);
        if (!rbio)
            co_return false;

        int written = BIO_write(rbio, buf.data(),
                                static_cast<int>(buf.size()));
        if (written <= 0)
        {
            log_last_ssl_error("BIO_write(rbio)");
            co_return false;
        }

        co_return true;
    }

    usub::uvent::task::Awaitable<bool> TlsRpcStream::do_handshake()
    {
#if URPC_LOGS
        usub::ulog::info(
            "TlsRpcStream::do_handshake: start this={} fd={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd);
#endif

        static constexpr std::size_t kMaxChunk = 16 * 1024;

        while (true)
        {
            int rc = SSL_do_handshake(this->ssl_);

            bool flushed = co_await this->flush_wbio();
            if (!flushed) co_return false;

            if (rc == 1)
            {
#if URPC_LOGS
                usub::ulog::info(
                    "TlsRpcStream::do_handshake: success this={}",
                    static_cast<void*>(this));
#endif
                this->fill_peer_identity();
                co_return true;
            }

            int err = SSL_get_error(this->ssl_, rc);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            {
                bool ok = co_await this->read_into_rbio(kMaxChunk);
                if (!ok) co_return false;
                continue;
            }

            log_last_ssl_error("SSL_do_handshake");
            co_return false;
        }
    }

    usub::uvent::task::Awaitable<ssize_t> TlsRpcStream::async_read(
        usub::uvent::utils::DynamicBuffer& buf,
        size_t max_read)
    {
        buf.clear();

        if (!this->ssl_)
        {
#if URPC_LOGS
            usub::ulog::debug(
                "TlsRpcStream::async_read: this={} fd={} ssl_ is null (already shutdown), return 0",
                static_cast<void*>(this),
                this->socket_.get_raw_header()->fd);
#endif
            co_return 0;
        }

#if URPC_LOGS
        usub::ulog::debug(
            "TlsRpcStream::async_read: this={} fd={} max_read={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            max_read);
#endif

        if (max_read == 0)
            co_return 0;

        constexpr std::size_t kMaxChunk = 16 * 1024;
        const std::size_t want = std::min(max_read, kMaxChunk);

        std::vector<uint8_t> tmp(want);

        while (true)
        {
            int rc = SSL_read(this->ssl_, tmp.data(), static_cast<int>(want));

            if (rc > 0)
            {
                buf.append(tmp.data(), static_cast<std::size_t>(rc));
#if URPC_LOGS
                usub::ulog::debug(
                    "TlsRpcStream::async_read: rc={} buf.size()={}",
                    rc, buf.size());
#endif
                co_return static_cast<ssize_t>(rc);
            }

            int err = SSL_get_error(this->ssl_, rc);

            if (err == SSL_ERROR_WANT_READ)
            {
#if URPC_LOGS
                usub::ulog::debug(
                    "TlsRpcStream::async_read: SSL_ERROR_WANT_READ, feeding rbio");
#endif
                bool ok = co_await this->read_into_rbio(kMaxChunk);
                if (!ok)
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "TlsRpcStream::async_read: read_into_rbio failed");
#endif
                    buf.clear();
                    co_return -1;
                }
                continue;
            }

            if (err == SSL_ERROR_WANT_WRITE)
            {
#if URPC_LOGS
                usub::ulog::debug(
                    "TlsRpcStream::async_read: SSL_ERROR_WANT_WRITE, flushing wbio");
#endif
                bool ok = co_await this->flush_wbio();
                if (!ok)
                {
#if URPC_LOGS
                    usub::ulog::warn(
                        "TlsRpcStream::async_read: flush_wbio failed");
#endif
                    buf.clear();
                    co_return -1;
                }
                continue;
            }

            if (err == SSL_ERROR_ZERO_RETURN)
            {
#if URPC_LOGS
                usub::ulog::info(
                    "TlsRpcStream::async_read: SSL_ERROR_ZERO_RETURN (EOF)");
#endif
                buf.clear();
                co_return 0;
            }

            log_last_ssl_error("SSL_read");
            buf.clear();
            co_return -1;
        }
    }

    usub::uvent::task::Awaitable<ssize_t> TlsRpcStream::async_write(
        uint8_t* data,
        size_t len)
    {
#if URPC_LOGS
        usub::ulog::debug(
            "TlsRpcStream::async_write: this={} fd={} len={}",
            static_cast<void*>(this),
            this->socket_.get_raw_header()->fd,
            len);
#endif

        static constexpr std::size_t kMaxChunk = 16 * 1024;
        size_t written_app = 0;

        while (written_app < len)
        {
            int rc = SSL_write(
                this->ssl_,
                data + written_app,
                static_cast<int>(len - written_app));

            bool flushed = co_await this->flush_wbio();
            if (!flushed) co_return -1;

            if (rc > 0)
            {
                written_app += static_cast<std::size_t>(rc);
                continue;
            }

            int err = SSL_get_error(this->ssl_, rc);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            {
                bool ok = co_await this->read_into_rbio(kMaxChunk);
                if (!ok) co_return -1;
                continue;
            }
            if (err == SSL_ERROR_ZERO_RETURN)
            {
#if URPC_LOGS
                usub::ulog::info(
                    "TlsRpcStream::async_write: SSL_ERROR_ZERO_RETURN");
#endif
                co_return static_cast<ssize_t>(written_app);
            }

            log_last_ssl_error("SSL_write");
            co_return -1;
        }

        co_return static_cast<ssize_t>(written_app);
    }

    std::shared_ptr<TlsRpcStream> TlsRpcStream::create(
        SocketType&& sock,
        std::shared_ptr<SSL_CTX> ctx,
        Mode mode,
        TlsClientConfig client_cfg,
        TlsServerConfig server_cfg)
    {
        return std::shared_ptr<TlsRpcStream>(
            new TlsRpcStream(
                std::move(sock),
                std::move(ctx),
                mode,
                std::move(client_cfg),
                std::move(server_cfg)));
    }

    usub::uvent::task::Awaitable<std::shared_ptr<TlsRpcStream>>
    TlsRpcStream::connect(std::string host,
                          uint16_t port,
                          const TlsClientConfig& cfg)
    {
        net::TCPClientSocket sock;
        auto res = co_await sock.async_connect(
            host.c_str(),
            std::to_string(port).c_str());
        if (res.has_value())
        {
#if URPC_LOGS
            usub::ulog::error(
                "TlsRpcStream::connect: async_connect failed ec={}",
                res.value());
#endif
            co_return nullptr;
        }

        auto ctx = make_client_ctx(cfg);
        if (!ctx)
        {
            co_return nullptr;
        }

        auto stream = TlsRpcStream::create(
            std::move(sock),
            std::move(ctx),
            Mode::Client,
            cfg,
            TlsServerConfig{});

        bool ok = co_await stream->do_handshake();
        if (!ok)
        {
            stream->shutdown();
            co_return nullptr;
        }

        co_return stream;
    }

    usub::uvent::task::Awaitable<std::shared_ptr<TlsRpcStream>>
    TlsRpcStream::from_accepted_socket(SocketType&& socket,
                                       const TlsServerConfig& cfg)
    {
        auto ctx = make_server_ctx(cfg);
        if (!ctx)
        {
            co_return nullptr;
        }

        auto stream = TlsRpcStream::create(
            std::move(socket),
            std::move(ctx),
            Mode::Server,
            TlsClientConfig{},
            cfg);

        bool ok = co_await stream->do_handshake();
        if (!ok)
        {
            stream->shutdown();
            co_return nullptr;
        }

        co_return stream;
    }
}
