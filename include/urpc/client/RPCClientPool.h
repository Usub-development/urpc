#ifndef URPC_RPCCLIENTPOOL_H
#define URPC_RPCCLIENTPOOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <limits>
#include <memory>

#include <uvent/utils/datastructures/array/ConcurrentVector.h>

#include <urpc/client/RPCClient.h>
#include <urpc/config/Config.h>
#include <urpc/transport/IRPCStream.h>

namespace urpc
{
    struct RpcClientPoolConfig
    {
        std::string host;
        uint16_t port{0};

        std::shared_ptr<IRpcStreamFactory> stream_factory{};

        int socket_timeout_ms{-1};
        int ping_interval_ms{0};

        std::size_t max_clients{std::numeric_limits<std::size_t>::max()};
    };

    struct RpcClientLease
    {
        RpcClient& client;
        std::size_t index;

        RpcClient& get() noexcept { return client; }
        [[nodiscard]] const RpcClient& get() const noexcept { return client; }
    };

    class RpcClientPool
    {
    public:
        explicit RpcClientPool(RpcClientPoolConfig cfg);

        RpcClientPool(const RpcClientPool&) = delete;
        RpcClientPool& operator=(const RpcClientPool&) = delete;

        RpcClientLease try_acquire();

        [[nodiscard]] std::size_t size() const noexcept
        {
            return this->size_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return this->cfg_.max_clients;
        }

        [[nodiscard]] const RpcClientPoolConfig& config() const noexcept
        {
            return cfg_;
        }

    private:
        std::optional<std::size_t> try_create_one();

    private:
        RpcClientPoolConfig cfg_;
        std::atomic<std::size_t> size_{0};
        std::atomic<std::size_t> rr_{0};

        usub::array::concurrent::LockFreeVector<std::shared_ptr<RpcClient>> clients_;
    };
} // namespace urpc

#endif // URPC_RPCCLIENTPOOL_H