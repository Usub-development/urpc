#ifndef URPCSETTINGSBUILDER_H
#define URPCSETTINGSBUILDER_H

#include <string>
#include <optional>
#include <vector>
#include "Transport.h"
#include "UrpcSettings.h"
#include "Codec.h"
#include "Wire.h"

namespace urpc
{
    struct Endpoint
    {
        std::string host;
        uint16_t port{0};
        std::string scheme;
    };

    class UrpcSettingsBuilder
    {
    public:
        UrpcSettingsBuilder& endpoint(std::string h, uint16_t p, std::string s)
        {
            this->ep_.host = std::move(h);
            this->ep_.port = p;
            this->ep_.scheme = std::move(s);
            return *this;
        }

        UrpcSettingsBuilder& alpn(std::string a)
        {
            this->meta_.alpn = std::move(a);
            return *this;
        }

        UrpcSettingsBuilder& sni(std::string s)
        {
            this->meta_.sni = std::move(s);
            return *this;
        }

        UrpcSettingsBuilder& tls_version(std::string v)
        {
            this->meta_.tls_version = std::move(v);
            return *this;
        }

        UrpcSettingsBuilder& cipher(std::string c)
        {
            this->meta_.cipher = std::move(c);
            return *this;
        }

        UrpcSettingsBuilder& mtls_subject(std::string s)
        {
            this->meta_.mtls_subject = std::move(s);
            return *this;
        }

        UrpcSettingsBuilder& mtls_fingerprint(std::vector<uint8_t> f)
        {
            this->meta_.mtls_fingerprint = std::move(f);
            return *this;
        }

        UrpcSettingsBuilder& cb(ChannelBindingMeta cbm)
        {
            this->meta_.cb = std::move(cbm);
            return *this;
        }

        UrpcSettingsBuilder& node_id(std::string n)
        {
            this->meta_.node_id = std::move(n);
            return *this;
        }

        UrpcSettingsBuilder& caps(std::string c)
        {
            this->meta_.caps = std::move(c);
            return *this;
        }

        UrpcSettingsBuilder& mode(TransportMode m)
        {
            this->mode_ = m;
            return *this;
        }

        template <TransportLike T>
        usub::uvent::task::Awaitable<bool> send(T& tr) const
        {
            std::string out;
            encode(out, this->meta_);
            co_return co_await tr.send_settings(std::move(out));
        }

        [[nodiscard]] const Endpoint& endpoint() const noexcept { return *&this->ep_; }
        [[nodiscard]] TransportMode mode() const noexcept { return this->mode_; }
        [[nodiscard]] const UrpcSettingsMeta& meta() const noexcept { return this->meta_; }

    private:
        Endpoint ep_{};
        UrpcSettingsMeta meta_{};
        TransportMode mode_{TransportMode::RAW};
    };
}

#endif // URPCSETTINGSBUILDER_H