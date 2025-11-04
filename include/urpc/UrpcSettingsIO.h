#ifndef URPCSETTINGSIO_H
#define URPCSETTINGSIO_H

#include <optional>
#include <string>
#include "UrpcSettings.h"
#include "Codec.h"
#include "Wire.h"
#include "Transport.h"

namespace urpc
{
    template <TransportLike T>
    usub::uvent::task::Awaitable<bool> send_settings_bin(T& tr, const UrpcSettingsMeta& meta)
    {
        std::string buf;
        encode(buf, meta);
        co_return co_await tr.send_settings(std::move(buf));
    }

    inline std::optional<UrpcSettingsMeta> decode_settings_from(const ParsedFrame& pf)
    {
        if (pf.h.stream != SETTINGS_STREAM || pf.h.method != SETTINGS_METHOD) return std::nullopt;
        UrpcSettingsMeta meta{};
        Buf b{pf.meta};
        if (!decode(b, meta)) return std::nullopt;
        return meta;
    }
} // namespace urpc

#endif // URPCSETTINGSIO_H