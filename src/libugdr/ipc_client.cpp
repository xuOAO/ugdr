#include <sys/socket.h>
#include "ipc_client.h"
#include "../common/logger.h"

namespace ugdr{
namespace lib{

void IpcClient::sendReqAndHandleRsp(int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    // 1.send request
    ssize_t n = ::send(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n < sizeof(req)) {
        throw std::runtime_error("Failed to send request to server");
    }

    // 2.recv response
    n = ::recv(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
    if (n < sizeof(rsp)) {
        throw std::runtime_error("Failed to receive response from server");
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

void IpcClient::sendReqAndHandleRspWithFds(int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp, std::vector<int>& fds){
    // 1.send request
    ssize_t n = ::send(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n < sizeof(req)) {
        throw std::runtime_error("Failed to send request to server");
    }

    // 2.recv response
    if (fds.size() > 0 && fds.size() <= common::UGDR_MAX_SEND_FDS_NUM) {
        n = ipc::recv_rsp_with_fds(client_fd, &rsp, sizeof(rsp), fds);
        if (n < 0) {
            throw std::runtime_error("Failed to receive fd from server");
        }
    } else {
        throw std::runtime_error("Invalid fd number");
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