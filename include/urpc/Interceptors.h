#ifndef URPC_INTERCEPTORS_H
#define URPC_INTERCEPTORS_H

#include <functional>
#include <string>
#include "Wire.h"

namespace urpc
{
    struct ClientInterceptor
    {
        std::function<void(UrpcHdr&, std::string&, std::string&)> before_send;
        std::function<void(ParsedFrame&)> after_recv;
    };
}
#endif