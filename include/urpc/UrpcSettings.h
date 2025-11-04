#ifndef URPCSETTINGS_H
#define URPCSETTINGS_H

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "ureflect/ureflect_auto.h"

namespace urpc
{
    enum class CbType : uint8_t { None = 0, TlsExporter = 1 };

    struct ChannelBindingMeta
    {
        CbType type{CbType::None};
        std::vector<uint8_t> data;
    };

    struct UrpcSettingsMeta
    {
        std::string alpn{"urpcv1"};
        std::optional<std::string> node_id;
        std::optional<std::string> caps;
        std::optional<std::string> sni;
        std::optional<std::string> tls_version;
        std::optional<std::string> cipher;
        std::optional<std::string> mtls_subject;
        std::optional<std::vector<uint8_t>> mtls_fingerprint;
        std::optional<ChannelBindingMeta> cb;
    };
} // namespace urpc

#endif // URPCSETTINGS_H