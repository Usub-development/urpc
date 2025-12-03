//
// Created by root on 11/29/25.
//

#ifndef IRPCSTREAM_H
#define IRPCSTREAM_H

#include <array>

#include <uvent/utils/buffer/DynamicBuffer.h>
#include <uvent/tasks/Awaitable.h>

#include <urpc/transport/TlsPeer.h>

namespace urpc
{
    struct IRpcStream
    {
        virtual usub::uvent::task::Awaitable<ssize_t> async_read(
            usub::uvent::utils::DynamicBuffer& buf, size_t max_read) = 0;

        virtual usub::uvent::task::Awaitable<ssize_t> async_write(
            uint8_t* data, size_t len) = 0;

        [[nodiscard]] virtual const RpcPeerIdentity* peer_identity() const noexcept = 0;

        [[nodiscard]] virtual bool get_app_secret_key(
            std::array<uint8_t, 32>& out_key) const noexcept = 0;

        virtual void shutdown() = 0;
        virtual ~IRpcStream() = default;
    };
}

#endif // IRPCSTREAM_H