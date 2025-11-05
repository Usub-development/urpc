//
// Created by root on 11/5/25.
//

#ifndef URPCOPTIONS_H
#define URPCOPTIONS_H

#include <cstdint>
#include <optional>
#include "Wire.h"

namespace urpc {
    struct MetaOpts {
        uint32_t        timeout_ms{0};
        uint64_t        cancel_id{0};
        CodecKind       codec{CodecKind::RAW};
        CompressionKind comp{CompressionKind::NONE};
        uint8_t         priority{0};
    };
}

#endif //URPCOPTIONS_H
