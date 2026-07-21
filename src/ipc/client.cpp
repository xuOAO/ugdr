#include "ipc/ipc.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

#include <utility>

namespace ugdr::ipc {

int IpcClient::connect(const std::string &socket_path) {
    if (socket_.valid()) {
        return -EISCONN;
    }
    if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return -ENAMETOOLONG;
    }

    UniqueFd candidate(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!candidate.valid()) {
        return -errno;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(candidate.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) <
        0) {
        return -errno;
    }
    socket_ = std::move(candidate);
    next_request_id_ = 1;
    return 0;
}

int IpcClient::call(std::uint32_t method, std::vector<std::byte> payload,
                    std::vector<UniqueFd> file_descriptors, IpcMessage *response) {
    if (!socket_.valid()) {
        return -ENOTCONN;
    }
    if (method == 0 || response == nullptr) {
        return -EINVAL;
    }

    IpcMessage request;
    request.envelope.method = method;
    request.envelope.request_id = next_request_id_++;
    if (next_request_id_ == 0) {
        next_request_id_ = 1;
    }
    request.payload = std::move(payload);
    request.file_descriptors = std::move(file_descriptors);
    const int send_status = send_message(socket_.get(), request);
    if (send_status != 0) {
        return send_status;
    }

    ReceiveResult received = receive_message(socket_.get());
    if (received.state == ReceiveState::eof) {
        return -ECONNRESET;
    }
    if (received.state == ReceiveState::error) {
        return -received.error_number;
    }
    if ((received.message.envelope.flags & kResponseFlag) == 0 ||
        received.message.envelope.request_id != request.envelope.request_id ||
        received.message.envelope.method != method) {
        return -EPROTO;
    }
    *response = std::move(received.message);
    return 0;
}

void IpcClient::close() noexcept {
    socket_.reset();
}

bool IpcClient::connected() const noexcept {
    return socket_.valid();
}

}  // namespace ugdr::ipc
