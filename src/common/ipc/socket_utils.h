#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>
#include "../../common/logger.h"
#include "ipc_proto.h"

namespace ugdr{
namespace ipc{

const std::string UDS_PATH_DEFAULT = "/tmp/ugdr.sock";

class Socket {
public:
    Socket(): fd_(-1) {}
    explicit Socket(int fd): fd_(fd) {}
    ~Socket(){
        close_fd();
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) : fd_(other.fd_) {
        other.fd_ = -1;
    }
    Socket& operator=(Socket&& other) noexcept {
        if(this != &other){
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    inline int get_fd() const { return fd_; }

    void close_fd(){
        if(fd_ >= 0){
            ::close(fd_);
            fd_ = -1;
        }
    }
private:
    int fd_;
};

inline Socket create_server_socket(const std::string& path){
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

    Socket sock(fd);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));
    }

    if (::listen(fd, 128) < 0) {
        throw std::runtime_error("listen() failed " + std::string(strerror(errno)));
    }

    return sock;
}

inline Socket connect_to_server(const std::string& path){
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

    Socket sock(fd);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("connect() failed: " + std::string(strerror(errno)));
        return Socket(-1);
    }

    return sock;
}

inline ssize_t send_rsp_with_fds(int socket, const void* buf, size_t len, const std::vector<int>& fds) {
    ssize_t n = 0;
    struct msghdr msg = {};
    struct iovec iov[1] = {};

    // union {
    //     struct cmsghdr cmsghdr;
    //     char control[CMSG_SPACE(sizeof(int)) * fds.size()];
    // } ctl_un;
    
    struct cmsghdr* cmsg = nullptr;
    std::vector<char> cmsg_buf;

    iov[0].iov_base = const_cast<void*>(buf);
    iov[0].iov_len = len;

    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    if (0 < fds.size() && fds.size() <= common::UGDR_MAX_SEND_FDS_NUM) {
        // 1. check
        for (int fd : fds) {
            if (fd < 0) throw std::runtime_error("[Server]: Invalid fd to send");
        }
        // 2. reverse space for cmsg
        size_t fds_size = fds.size() * sizeof(int);
        cmsg_buf.resize(CMSG_SPACE(fds_size));

        msg.msg_control = cmsg_buf.data();
        msg.msg_controllen = cmsg_buf.size();

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(fds_size);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

        ::memcpy(CMSG_DATA(cmsg), fds.data(), fds_size);
    } else {
        throw std::runtime_error("[Server]: No fd to send");
    }

    // if (fd >= 0) {
    //     msg.msg_control = ctl_un.control;
    //     msg.msg_controllen = sizeof(ctl_un.control);

    //     cmsg = CMSG_FIRSTHDR(&msg);
    //     cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    //     cmsg->cmsg_level = SOL_SOCKET;
    //     cmsg->cmsg_type = SCM_RIGHTS;

    //     *reinterpret_cast<int*>(CMSG_DATA(cmsg)) = fd;
    // } else {
    //     UGDR_LOG_ERROR("[Server]: Invalid fd to send");
    //     return -1;
    // }

    n = ::sendmsg(socket, &msg, MSG_WAITALL);
    if (n < 0) {
        throw std::runtime_error("[Server]: sendmsg failed: " + std::string(strerror(errno)));
    }
    return n;
}

inline ssize_t recv_rsp_with_fds(int sock, void* buf, ssize_t len, std::vector<int>& fds) {
    ssize_t n = 0;
    struct msghdr msg = {};
    struct iovec iov[1] = {};

    // union {
    //     struct cmsghdr cmsghdr;
    //     char control[CMSG_SPACE(sizeof(int))];
    // } ctl_un;

    // 1. prepare enough space for cmsg
    std::vector<char> cmsg_buf(CMSG_SPACE(sizeof(int) * common::UGDR_MAX_SEND_FDS_NUM));

    struct cmsghdr* cmsg = nullptr;

    iov[0].iov_base = buf;
    iov[0].iov_len = len;

    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf.data();
    msg.msg_controllen = cmsg_buf.size();

    n = ::recvmsg(sock, &msg, MSG_WAITALL);
    if (n < 0) {
        UGDR_LOG_ERROR("[Client]: recvmsg failed: %s", strerror(errno));
        return -1;
    }

    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int* ptr = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        size_t payload_size = cmsg->cmsg_len - ((size_t)CMSG_DATA(cmsg) - (size_t)cmsg);
        int count = payload_size / sizeof(int);
        if(count != fds.size()) throw std::runtime_error("[Client]: Mismatch fd count");
        for(int i = 0; i < count; ++i) {
            fds[i] = ptr[i];
        }
    } else {
        UGDR_LOG_ERROR("[Client]: No fd received");
        return -1;
    }

    return n;
}

}
}