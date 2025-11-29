#include <urpc/registry/RPCMethodRegistry.h>

namespace urpc
{
    void RpcMethodRegistry::register_method(uint64_t method_id,
                                            RpcHandlerPtr fn)
    {
        this->handlers_[method_id] = fn;
    }

    void RpcMethodRegistry::register_method(std::string_view name,
                                            RpcHandlerPtr fn)
    {
        uint64_t mid = fnv1a64_rt(name);
        this->handlers_[mid] = fn;
    }

    RpcHandlerPtr RpcMethodRegistry::find(uint64_t method_id) const
    {
        auto it = this->handlers_.find(method_id);
        return it == this->handlers_.end() ? nullptr : it->second;
    }
}
