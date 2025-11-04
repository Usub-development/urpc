#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <concepts>
#include <iostream>

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

    class ITransport
    {
    public:
        virtual ~ITransport() = default;

        [[nodiscard]] virtual uint8_t transport_bits() const noexcept = 0;

        virtual usub::uvent::task::Awaitable<bool> send_frame(std::string frame) = 0;
        virtual usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) = 0;

        virtual usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() = 0;

        virtual usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
        rpc_call(uint64_t method, std::string meta, std::string body) = 0;

        [[nodiscard]] virtual bool alive() const noexcept = 0;
        [[nodiscard]] virtual int native_handle() const noexcept = 0;

        virtual void close() noexcept = 0;
    };

    template <class T>
    concept TransportLike =
        std::derived_from<T, ITransport> &&
        requires(T& t, std::string s, uint64_t m)
        {
            { t.transport_bits() } -> std::convertible_to<uint8_t>;
            { t.send_frame(std::move(s)) } -> std::same_as<usub::uvent::task::Awaitable<bool>>;
            { t.send_settings(std::move(s)) } -> std::same_as<usub::uvent::task::Awaitable<bool>>;
            { t.recv_frame() } -> std::same_as<usub::uvent::task::Awaitable<std::optional<ParsedFrame>>>;
            {
                t.rpc_call(m, std::move(s), std::move(s))
            } -> std::same_as<usub::uvent::task::Awaitable<std::optional<ParsedFrame>>>;
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
            std::cout << "[transport] read_exact_into: got=" << got
                << " off=" << off << "/" << need << std::endl;
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
            std::cout << "[transport] recv_header: need " << need << " more bytes for header" << std::endl;
            if (!(co_await read_exact_into(rw, fr.rx, need))) co_return std::nullopt;
        }

        auto hopt = hdr_decode(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(fr.rx.data()), fr.rx.size()));
        if (hopt)
        {
            std::cout << "[transport] recv_header: full header+payload already in buffer (rx=" << fr.rx.size() << ")"
                << std::endl;
            co_return hopt;
        }

        const uint32_t len_field = get_le32(fr.rx.data());
        if (len_field > MAX_FRAME_NO_LEN + HDR_NO_LEN) co_return std::nullopt;
        const size_t total_need = 4 + static_cast<size_t>(len_field);
        const size_t now = fr.rx.size();
        if (now < total_need)
        {
            std::cout << "[transport] recv_header: payload need " << (total_need - now)
                << " bytes (total=" << total_need << ")" << std::endl;
            if (!(co_await read_exact_into(rw, fr.rx, total_need - now))) co_return std::nullopt;
        }

        co_return hdr_decode(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(fr.rx.data()), fr.rx.size()));
    }

    template <RWLike RW>
    usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
    recv_one(RW& rw, Framer& fr)
    {
        auto hopt = co_await recv_header(rw, fr);
        if (!hopt) co_return std::nullopt;

        const UrpcHdr h = *hopt;
        const size_t total = 4 + static_cast<size_t>(h.len);

        if (fr.rx.size() < total)
        {
            if (!(co_await read_exact_into(rw, fr.rx, total - fr.rx.size()))) co_return std::nullopt;
        }

        ParsedFrame pf{};
        if (!parse_frame(fr.rx.data(), total, pf)) co_return std::nullopt;

        std::cout << "[transport] recv_one: OK type=" << int(pf.h.type)
            << " stream=" << pf.h.stream
            << " method=" << pf.h.method
            << " meta=" << pf.h.meta_len
            << " body=" << pf.h.body_len << std::endl;

        if (fr.rx.size() > total) fr.rx.erase(0, total);
        else fr.rx.clear();

        co_return pf;
    }

    template <RWLike RW>
    class TlsTransport final : public ITransport
    {
    public:
        explicit TlsTransport(RW rw, TransportMode mode = TransportMode::TLS, int native = -1)
            : rw_(std::move(rw)), native_(native)
        {
            this->fr_.transport_bits = derive_transport_flags(mode);
        }

        [[nodiscard]] uint8_t transport_bits() const noexcept override { return this->fr_.transport_bits; }
        [[nodiscard]] bool alive() const noexcept override { return this->alive_; }
        [[nodiscard]] int native_handle() const noexcept override { return this->native_; }
        void close() noexcept override { this->alive_ = false; }

        usub::uvent::task::Awaitable<bool> send_frame(std::string frame) override
        {
            if (!this->alive_) co_return false;
            std::cout << "[transport] send_frame: " << frame.size() << " bytes" << std::endl;
            co_await this->rw_.write_all(as_bytes(frame));
            co_return true;
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr(TransportMode::TLS);
            h.flags = this->fr_.transport_bits;
            if (!meta_bin.empty()) h.flags |= F_CB_PRESENT;
            std::string frame = make_frame(h, std::move(meta_bin), {});
            std::cout << "[transport] send_settings(TLS): frame=" << frame.size() << "B" << std::endl;
            co_return co_await this->send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!this->alive_) co_return std::nullopt;
            co_return co_await recv_one(this->rw_, this->fr_);
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
        rpc_call(uint64_t method, std::string meta, std::string body) override
        {
            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::REQUEST);
            h.method = method;
            h.flags = this->fr_.transport_bits;

            std::string frame = make_frame(h, std::move(meta), std::move(body));
            if (!(co_await this->send_frame(std::move(frame)))) co_return std::nullopt;
            co_return co_await this->recv_frame();
        }

    private:
        RW rw_;
        int native_{-1};
        bool alive_{true};
        Framer fr_{};
    };

    template <RWLike RW>
    using MtlsTransport = TlsTransport<RW>;

    template <RWLike RW>
    class RawTransport final : public ITransport
    {
    public:
        explicit RawTransport(RW rw, int native = -1)
            : rw_(std::move(rw)), native_(native)
        {
            this->fr_.transport_bits = derive_transport_flags(TransportMode::RAW);
        }

        [[nodiscard]] uint8_t transport_bits() const noexcept override { return this->fr_.transport_bits; }
        [[nodiscard]] bool alive() const noexcept override { return this->alive_; }
        [[nodiscard]] int native_handle() const noexcept override { return this->native_; }
        void close() noexcept override { this->alive_ = false; }

        usub::uvent::task::Awaitable<bool> send_frame(std::string frame) override
        {
            if (!this->alive_) co_return false;
            std::cout << "[transport] send_frame: " << frame.size() << " bytes" << std::endl;
            co_await this->rw_.write_all(as_bytes(frame));
            co_return true;
        }

        usub::uvent::task::Awaitable<bool> send_settings(std::string meta_bin) override
        {
            auto h = make_settings_hdr(TransportMode::RAW);
            h.flags = this->fr_.transport_bits;
            if (!meta_bin.empty()) h.flags |= F_CB_PRESENT;

            std::string frame = make_frame(h, std::move(meta_bin), {});
            std::cout << "[transport] send_settings(RAW): frame=" << frame.size() << "B" << std::endl;
            co_return co_await this->send_frame(std::move(frame));
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>> recv_frame() override
        {
            if (!this->alive_) co_return std::nullopt;
            co_return co_await recv_one(this->rw_, this->fr_);
        }

        usub::uvent::task::Awaitable<std::optional<ParsedFrame>>
        rpc_call(uint64_t method, std::string meta, std::string body) override
        {
            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::REQUEST);
            h.method = method;
            h.flags = this->fr_.transport_bits;

            std::string frame = make_frame(h, std::move(meta), std::move(body));
            if (!(co_await this->send_frame(std::move(frame)))) co_return std::nullopt;
            co_return co_await this->recv_frame();
        }

    private:
        RW rw_;
        int native_{-1};
        bool alive_{true};
        Framer fr_{};
    };
} // namespace urpc

#endif // TRANSPORT_H
