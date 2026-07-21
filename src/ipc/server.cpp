#include "ipc/ipc.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <utility>
#include <vector>

namespace ugdr::ipc {

IpcServer::IpcServer(IpcHandler &handler) noexcept : handler_(handler) {
}

IpcServer::~IpcServer() {
    close();
}

int IpcServer::start(const std::string &socket_path) {
    if (listener_.valid()) {
        return -EALREADY;
    }
    if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return -ENAMETOOLONG;
    }
    struct stat path_status {};
    if (::lstat(socket_path.c_str(), &path_status) == 0) {
        return -EADDRINUSE;
    }
    if (errno != ENOENT) {
        return -errno;
    }

    UniqueFd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!listener.valid()) {
        return -errno;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        return -errno;
    }
    struct stat bound_path_status {};
    if (::lstat(socket_path.c_str(), &bound_path_status) != 0 ||
        !S_ISSOCK(bound_path_status.st_mode)) {
        return -EIO;
    }
    owns_socket_path_ = true;
    socket_path_ = socket_path;
    socket_device_ = static_cast<std::uint64_t>(bound_path_status.st_dev);
    socket_inode_ = static_cast<std::uint64_t>(bound_path_status.st_ino);
    if (::listen(listener.get(), SOMAXCONN) < 0) {
        const int error_number = errno;
        remove_socket_path();
        return -error_number;
    }
    listener_ = std::move(listener);
    next_session_id_ = 1;
    return 0;
}

int IpcServer::accept_ready_connections() {
    for (;;) {
        const int accepted =
            ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (accepted < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        Connection connection;
        connection.socket = UniqueFd(accepted);
        connection.session_id = next_session_id_++;
        if (next_session_id_ == 0) {
            next_session_id_ = 1;
        }
        connections_.emplace(accepted, std::move(connection));
    }
}

int IpcServer::poll_once(int timeout_ms) {
    if (!listener_.valid()) {
        return -ENOTCONN;
    }
    std::vector<pollfd> descriptors;
    descriptors.reserve(connections_.size() + 1);
    descriptors.push_back({listener_.get(), POLLIN, 0});
    for (const auto &[socket_fd, connection] : connections_) {
        static_cast<void>(connection);
        descriptors.push_back({socket_fd, POLLIN, 0});
    }

    const int ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
    if (ready < 0) {
        return errno == EINTR ? 0 : -errno;
    }
    if (ready == 0) {
        return 0;
    }
    if ((descriptors.front().revents & POLLIN) != 0) {
        const int accept_status = accept_ready_connections();
        if (accept_status != 0) {
            return accept_status;
        }
    }

    for (std::size_t index = 1; index < descriptors.size(); ++index) {
        const pollfd descriptor = descriptors[index];
        auto connection = connections_.find(descriptor.fd);
        if (connection == connections_.end()) {
            continue;
        }
        bool should_close = (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
        if ((descriptor.revents & POLLIN) != 0) {
            ReceiveResult received = receive_message(descriptor.fd);
            if (received.state == ReceiveState::eof) {
                should_close = true;
            } else if (received.state == ReceiveState::error) {
                if (received.error_number != EAGAIN && received.error_number != EWOULDBLOCK) {
                    should_close = true;
                }
            } else {
                const Envelope request_envelope = received.message.envelope;
                IpcMessage response;
                try {
                    response =
                        handler_.handle(connection->second.session_id, std::move(received.message));
                } catch (...) {
                    response.envelope.status = EIO;
                }
                response.envelope.magic = kProtocolMagic;
                response.envelope.version_major = kProtocolMajor;
                response.envelope.version_minor = kProtocolMinor;
                response.envelope.method = request_envelope.method;
                response.envelope.flags = kResponseFlag;
                response.envelope.request_id = request_envelope.request_id;
                if (send_message(descriptor.fd, response) != 0) {
                    should_close = true;
                }
            }
        }
        if ((descriptor.revents & POLLHUP) != 0) {
            should_close = true;
        }
        if (should_close) {
            close_session(descriptor.fd);
        }
    }
    return 0;
}

void IpcServer::close_session(int socket_fd) noexcept {
    const auto connection = connections_.find(socket_fd);
    if (connection == connections_.end()) {
        return;
    }
    const SessionId session_id = connection->second.session_id;
    connections_.erase(connection);
    handler_.on_disconnect(session_id);
}

void IpcServer::close() noexcept {
    while (!connections_.empty()) {
        close_session(connections_.begin()->first);
    }
    listener_.reset();
    remove_socket_path();
}

void IpcServer::remove_socket_path() noexcept {
    struct stat path_status {};
    if (owns_socket_path_ && !socket_path_.empty() &&
        ::lstat(socket_path_.c_str(), &path_status) == 0 && S_ISSOCK(path_status.st_mode) &&
        static_cast<std::uint64_t>(path_status.st_dev) == socket_device_ &&
        static_cast<std::uint64_t>(path_status.st_ino) == socket_inode_) {
        ::unlink(socket_path_.c_str());
    }
    owns_socket_path_ = false;
    socket_path_.clear();
    socket_device_ = 0;
    socket_inode_ = 0;
}

bool IpcServer::running() const noexcept {
    return listener_.valid();
}

std::size_t IpcServer::session_count() const noexcept {
    return connections_.size();
}

}  // namespace ugdr::ipc
