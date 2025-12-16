#include <sys/socket.h>
#include "ipc_client.h"
#include "../common/logger.h"

namespace ugdr{
namespace lib{


void IpcClient::sendReqAndHandleRsp(int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp, int* out_fd) {
    // 1.send request
    ssize_t n = ::send(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n < sizeof(req)) {
        throw std::runtime_error("Failed to send request to server");
    }

    // 2.recv response
    if (out_fd != nullptr) {
        //TODO: slice2处理fd转移
    } else {
        n = ::recv(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
        if (n < sizeof(rsp)) {
            throw std::runtime_error("Failed to receive response from server");
        }
    }

    // 3.normal handle
    if (rsp.header.magic != ipc::UGDR_PROTO_MAGIC) {
        throw std::runtime_error("Invalid magic number in response");
    }

    if (rsp.header.status != 0 ) {
        throw std::runtime_error("Server returned error status: " + std::to_string(rsp.header.status));
    }

    if (rsp.header.cmd != req.header.cmd) {
        throw std::runtime_error("Mismatched cmd in response");
    }
}

}
}