#ifndef URPC_TRANSPORT_H
#define URPC_TRANSPORT_H

#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <concepts>
#include <iostream>
#include <chrono>

#include "Wire.h"
#include "uvent/Uvent.h"

namespace urpc
{
    inline std::span<const std::byte> as_bytes(std::string_view s)
    {
        return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    }

    inline std::span<std::byte> as_wbytes(char* p, size_t n)
    {
        return {reinterpret_cast<std::byte*>(p), n};
    }

    struct Framer
    {
        std::string rx;
        uint8_t transport_bits{F_TP_RAW};
    };

    template <class RW>
    concept RWLike = requires(RW& rw, std::span<const std::byte> cbuf, std::span<std::byte> wbuf)
    {
        { rw.write_all(cbuf) } -> std::same_as<usub::uvent::task::Awaitable<void>>;
        { rw.read_some(wbuf) } -> std::same_as<usub::uvent::task::Awaitable<size_t>>;
        { rw.native_handle() } -> std::convertible_to<int>;
    };

    struct CoalesceConfig
    {
        size_t flush_threshold_bytes{32 * 1024};
        uint32_t flush_interval_ms{1};
    };

    class ITransport
    {
    public:
        virtual ~ITransport() = default;

        [[nodiscard]] virtual uint8_t transport_bits() const noexcept = 0;

        virtual usub::uvent::task::Awaitable<bool> send_frame(std::string frame) = 0;
        virtual usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) = 0;

        virtual usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() = 0;

        [[nodiscard]] virtual bool alive() const noexcept = 0;
        [[nodiscard]] virtual int native_handle() const noexcept = 0;

        virtual void close() noexcept = 0;

        // coalescing API
        virtual void set_coalescing(CoalesceConfig cfg) { (void)cfg; }
        virtual usub::uvent::task::Awaitable<void> flush() { co_return; }
    };

    template <class T>
    concept TransportLike =
        std::derived_from<T, ITransport> &&
        requires(T& t, std::string s)
        {
            { t.transport_bits() } -> std::convertible_to<uint8_t>;
            { t.send_frame(std::move(s)) };
            { t.send_settings(std::move(s)) };
            { t.recv_frame() };
            { t.alive() } -> std::convertible_to<bool>;
            { t.native_handle() } -> std::convertible_to<int>;
            t.close();
        };

    template <RWLike RW>
    usub::uvent::task::Awaitable<bool> read_exact_into(RW& rw, std::string& dst, size_t need)
    {
        const size_t start = dst.size();
        dst.resize(start + need);
        size_t off = 0;
        while (off < need)
        {
            auto got = co_await rw.read_some(as_wbytes(dst.data() + start + off, need - off));
            if (got == 0)
            {
                dst.resize(start + off);
                co_return false;
            }
            off += got;
#ifdef UVENT_DEBUG
        std::cout << "[transport] read_exact_into: got=" << got << " off=" << off << "/" << need << std::endl;
#endif
        }
        co_return true;
    }

    template <RWLike RW>
    usub::uvent::task::Awaitable<std::optional<UrpcHdr>>
    recv_header(RW& rw, Framer& fr)
    {
        while (fr.rx.size() < HDR_SIZE)
        {
            const auto need = HDR_SIZE - fr.rx.size();
#ifdef UVENT_DEBUG
        std::cout << "[transport] recv_header: need " << need << " more bytes" << std::endl;
#endif
            if (!(co_await read_exact_into(rw, fr.rx, need))) co_return std::nullopt;
        }
        auto hopt = hdr_decode(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(fr.rx.data()), fr.rx.size()));
        if (hopt) co_return hopt;

        const uint32_t len_field = get_le32(fr.rx.data());
        if (len_field > MAX_FRAME_NO_LEN + HDR_NO_LEN) co_return std::nullopt;
        const size_t total_need = 4 + (size_t)len_field;
        const size_t now = fr.rx.size();
        if (now < total_need)
        {
#ifdef UVENT_DEBUG
        std::cout << "[transport] recv_header: payload need " << (total_need-now) << " bytes" << std::endl;
#endif
            if (!(co_await read_exact_into(rw, fr.rx, total_need - now))) co_return std::nullopt;
        }
        co_return hdr_decode(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(fr.rx.data()), fr.rx.size()));
    }

    template <RWLike RW>
    usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
    recv_one(RW& rw, Framer& fr)
    {
        auto hopt = co_await recv_header(rw, fr);
        if (!hopt) co_return std::nullopt;

        const UrpcHdr h = *hopt;
        const size_t total = 4 + (size_t)h.len;

        if (fr.rx.size() < total)
        {
            if (!(co_await read_exact_into(rw, fr.rx, total - fr.rx.size()))) co_return std::nullopt;
        }

        ParsedFrame pf{};
        if (!parse_frame(fr.rx.data(), total, pf)) co_return std::nullopt;

#ifdef UVENT_DEBUG
    std::cout << "[transport] recv_one: OK type=" << int(pf.h.type)
              << " stream=" << pf.h.stream
              << " method=" << pf.h.method
              << " meta=" << pf.h.meta_len
              << " body=" << pf.h.body_len << std::endl;
#endif

        if (fr.rx.size() > total) fr.rx.erase(0, total);
        else fr.rx.clear();
        co_return pf;
    }

    // ===== RAW transport =====
    template <RWLike RW>
    class RawTransport final : public ITransport
    {
    public:
        explicit RawTransport(RW rw, int native = -1) : rw_(std::move(rw)), native_(native)
        {
            fr_.transport_bits = derive_transport_flags(TransportMode::RAW);
        }

        uint8_t transport_bits() const noexcept override { return fr_.transport_bits; }
        bool alive() const noexcept override { return alive_; }
        int native_handle() const noexcept override { return native_; }
        void close() noexcept override { alive_ = false; }

        void set_coalescing(CoalesceConfig cfg) override { coal_ = cfg; }

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
            co_await rw_.write_all(as_bytes(tx_buf_));
            tx_buf_.clear();
            co_return;
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr(TransportMode::RAW);
            h.flags = fr_.transport_bits;
            if (!meta_bin.empty()) h.flags |= F_CB_PRESENT;
            std::string frame = make_frame(h, std::move(meta_bin), {});
            co_return co_await send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!alive_) co_return std::nullopt;
            co_return co_await recv_one(rw_, fr_);
        }

    private:
        RW rw_;
        int native_{-1};
        bool alive_{true};
        Framer fr_{};
        CoalesceConfig coal_{};
        std::string tx_buf_;
        bool flush_armed_{false};
    };

    // ===== TLS placeholder transport (same write path; TLS handled externally) =====
    template <RWLike RW>
    class TlsTransport final : public ITransport
    {
    public:
        explicit TlsTransport(RW rw, TransportMode mode = TransportMode::TLS, int native = -1)
            : rw_(std::move(rw)), native_(native) { fr_.transport_bits = derive_transport_flags(mode); }

        uint8_t transport_bits() const noexcept override { return fr_.transport_bits; }
        bool alive() const noexcept override { return alive_; }
        int native_handle() const noexcept override { return native_; }
        void close() noexcept override { alive_ = false; }

        void set_coalescing(CoalesceConfig cfg) override { coal_ = cfg; }

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
            co_await rw_.write_all(as_bytes(tx_buf_));
            tx_buf_.clear();
            co_return;
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr(TransportMode::TLS);
            h.flags = fr_.transport_bits;
            if (!meta_bin.empty()) h.flags |= F_CB_PRESENT;
            std::string frame = make_frame(h, std::move(meta_bin), {});
            co_return co_await send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!alive_) co_return std::nullopt;
            co_return co_await recv_one(rw_, fr_);
        }

    private:
        RW rw_;
        int native_{-1};
        bool alive_{true};
        Framer fr_{};
        CoalesceConfig coal_{};
        std::string tx_buf_;
        bool flush_armed_{false};
    };

    template <RWLike RW>
    using MtlsTransport = TlsTransport<RW>;
} // namespace urpc

#endif // URPC_TRANSPORT_H
