#pragma once
#include "ugdr_internal.h"
#include "../common/ipc/socket_utils.h"
#include "../common/ipc/ipc_proto.h"

namespace ugdr{
namespace lib{

class IpcClient {
public:
    static void sendReqAndHandleRsp(int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp, int* out_fd = nullptr);
};

}
}
