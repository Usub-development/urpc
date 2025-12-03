//
// Created by root on 01.12.2025.
//

#ifndef IRPCSTREAMFACTORY_H
#define IRPCSTREAMFACTORY_H

#include <memory>
#include <string>

#include <uvent/tasks/Awaitable.h>
#include <uvent/net/Socket.h>

#include <urpc/transport/IRPCStream.h>

namespace urpc
{
    struct IRpcStreamFactory
    {
        virtual usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_client_stream(const std::string& host, uint16_t port) = 0;

        virtual usub::uvent::task::Awaitable<std::shared_ptr<IRpcStream>>
        create_server_stream(usub::uvent::net::TCPClientSocket&& socket) = 0;

        virtual ~IRpcStreamFactory() = default;
    };
}

#endif // IRPCSTREAMFACTORY_H