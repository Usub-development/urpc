//
// Created by root on 12/1/25.
//

#ifndef TLSPEER_H
#define TLSPEER_H

#include <string>
#include <vector>

namespace urpc
{
    struct RpcPeerIdentity
    {
        bool authenticated{false};
        std::string subject;
        std::string issuer;
        std::string common_name;
        std::vector<std::string> dns_sans;
        std::string pem;
    };
}


#endif //TLSPEER_H
