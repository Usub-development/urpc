// urpc/registry/RpcMethodRegistry.h
//
// Created by root on 11/29/25.
//

#ifndef RPCMETHODREGISTRY_H
#define RPCMETHODREGISTRY_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <urpc/context/RPCContext.h>
#include <urpc/utils/Hash.h>

namespace urpc
{
    class RpcMethodRegistry
    {
    public:
        template <uint64_t MethodId, typename F>
        void register_method_ct(F&& f)
        {
            this->handlers_[MethodId] =
                static_cast<RpcHandlerPtr>(+f);
        }

        void register_method(uint64_t method_id, RpcHandlerPtr fn);
        void register_method(std::string_view name, RpcHandlerPtr fn);

        RpcHandlerPtr find(uint64_t method_id) const;

    private:
        std::unordered_map<uint64_t, RpcHandlerPtr> handlers_;
    };
}

#endif // RPCMETHODREGISTRY_H