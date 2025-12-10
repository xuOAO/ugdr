#pragma once
#include <string>
#include "ugdr_internal.h"
#include "../common/ipc/socket_utils.h"

namespace ugdr{
namespace lib{

class IpcClient {
public:
    static struct ::ugdr_context connect_and_handshake(const std::string& dev_name);
    static bool close(struct ugdr_context* ctx);
};

}
}
